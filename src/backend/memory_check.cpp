/// @file memory_check.cpp
/// @brief Pre-allocation memory budget guard.
///
/// FullState CPU/GPU backends call check_memory_budget() inside prepare() before
/// allocating the 2^n state vector. SparseState / TensorNetwork skip the guard
/// because their footprint is circuit-dependent and cannot be bounded ahead of
/// time — they rely on the post-hoc bad_alloc wrap.

#include "backend/memory_check.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

#include "error.hpp"
#include "log.hpp"

namespace qplayer::detail {

namespace {

// One GiB in bytes — binary unit.
constexpr std::uint64_t kBytesPerGiB = std::uint64_t{1} << 30;

// 80% safety margin (-1): leaving the system 20% headroom keeps the
// shell / SSH / systemd off swap and out of OOM-killer range.
constexpr double kBudgetFraction = 0.8;

// Sentinel meaning "no usable reading" — disables the precheck and defers to
// the post-hoc bad_alloc path (-1 fallback).
constexpr std::uint64_t kNoLimit = UINT64_MAX;

// Parses a GiB float from an env var into a byte budget. Returns false (and
// warns) when the value is missing, non-numeric, negative, or non-finite so
// the caller can fall back to /proc/meminfo (-1).
bool budget_from_env(const char* var_name, std::uint64_t& out_bytes) {
    const char* raw = std::getenv(var_name);
    if (raw == nullptr) {
        return false;
    }
    try {
        const double gb = std::stod(raw);
        // The upper bound must keep gb * 2^30 inside uint64: the old limit of
        // 1e12 GiB is ~1.07e21 bytes against a uint64 max of ~1.84e19, and
        // converting an out-of-range double to an integer type is UB, not a
        // wrap (audit #18). Bound the *byte* product instead of the GiB figure.
        constexpr double kMaxBytes =
            static_cast<double>(std::numeric_limits<std::uint64_t>::max());
        const double bytes = gb * static_cast<double>(kBytesPerGiB);
        if (!(gb >= 0.0) || !(bytes <= kMaxBytes)) {  // rejects NaN, negative, overflow
            log::warn(std::string(var_name) +
                      " is out of range; using /proc/meminfo instead.");
            return false;
        }
        out_bytes = static_cast<std::uint64_t>(bytes);
        return true;
    } catch (...) {
        log::warn(std::string(var_name) +
                  " is not a number; using /proc/meminfo instead.");
        return false;
    }
}

// Reads MemAvailable (kB) from /proc/meminfo and applies the 80% margin.
// Returns kNoLimit when the file or field is unreadable so the precheck is
// disabled rather than blocking on a bad reading (-1).
std::uint64_t meminfo_budget_bytes() {
    std::ifstream f("/proc/meminfo");
    if (!f) {
        return kNoLimit;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("MemAvailable:", 0) == 0) {
            try {
                // "MemAvailable:" is 13 chars; the remainder is "<kB> kB".
                const std::uint64_t kb = std::stoull(line.substr(13));
                return static_cast<std::uint64_t>(
                    static_cast<double>(kb) * 1024.0 * kBudgetFraction);
            } catch (...) {
                return kNoLimit;
            }
        }
    }
    return kNoLimit;
}

// Reads the cgroup memory limit and applies the 80% margin.
// Inside a container /proc/meminfo reports the host, so the cgroup cap is the
// real ceiling. cgroup v2: memory.max ("max" => unlimited) minus memory.current.
// cgroup v1: memory.limit_in_bytes minus memory.usage_in_bytes. Returns kNoLimit
// when no cgroup limit applies or the files are unreadable so the caller's min()
// drops this term and falls back to /proc/meminfo.
std::uint64_t cgroup_budget_bytes() {
    // Read a single whitespace-delimited token as uint64; "max" -> kNoLimit.
    auto read_u64 = [](const char* path, std::uint64_t& out) -> bool {
        std::ifstream f(path);
        if (!f) {
            return false;
        }
        std::string tok;
        if (!(f >> tok)) {
            return false;
        }
        if (tok == "max") {  // cgroup v2 unlimited sentinel
            out = kNoLimit;
            return true;
        }
        try {
            out = std::stoull(tok);
            return true;
        } catch (...) {
            return false;
        }
    };

    const auto margin = [](std::uint64_t limit, std::uint64_t usage) -> std::uint64_t {
        const std::uint64_t free_bytes = (limit > usage) ? (limit - usage) : 0;
        return static_cast<std::uint64_t>(static_cast<double>(free_bytes) *
                                          kBudgetFraction);
    };

    std::uint64_t limit = 0;
    std::uint64_t usage = 0;
    // cgroup v2 (unified hierarchy) first.
    if (read_u64("/sys/fs/cgroup/memory.max", limit) &&
        read_u64("/sys/fs/cgroup/memory.current", usage)) {
        return (limit == kNoLimit) ? kNoLimit : margin(limit, usage);
    }
    // cgroup v1 fallback.
    if (read_u64("/sys/fs/cgroup/memory/memory.limit_in_bytes", limit) &&
        read_u64("/sys/fs/cgroup/memory/memory.usage_in_bytes", usage)) {
        // v1 "unlimited" is a near-UINT64_MAX sentinel (PAGE_COUNTER_MAX); treat
        // very large limits as no limit to avoid a bogus multi-exabyte budget.
        constexpr std::uint64_t kV1Unlimited = std::uint64_t{1} << 62;
        return (limit >= kV1Unlimited) ? kNoLimit : margin(limit, usage);
    }
    return kNoLimit;
}

// Formats a byte count as a GiB string with two decimals (unit).
std::string to_gib(std::uint64_t bytes) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(2)
       << static_cast<double>(bytes) / static_cast<double>(kBytesPerGiB);
    return os.str();
}

}  // namespace

std::uint64_t available_memory_bytes() {
    // Priority: explicit env override (exact) > min(/proc/meminfo MemAvailable,
    // cgroup limit) × 0.8 (-1). The 0.8 margin is
    // applied inside each source; the tighter of host-available and container
    // cap wins. Either source returns kNoLimit when unreadable and drops out of
    // the min(), so the result degrades gracefully to whichever is available.
    std::uint64_t budget = 0;
    if (budget_from_env("QPLAYER_MEMORY_BUDGET_GB", budget)) {
        return budget;
    }
    return std::min(meminfo_budget_bytes(), cgroup_budget_bytes());
}

void check_memory_budget(std::uint64_t    required_bytes,
                         std::string_view backend_name,
                         QubitIndex       num_qubits) {
    const std::uint64_t available = available_memory_bytes();

    // required == kNoLimit means peak_state_bytes() refused to model this n (>= 59)
    // because 2^(n+5) overflows uint64. That must fail unconditionally: when both
    // /proc/meminfo and the cgroup limit are unreadable, `available` is kNoLimit
    // too and the comparison below became the tautology kNoLimit <= kNoLimit, so
    // even n = 100 passed. The state allocation then truncated 2^100 bytes to 0
    // (size_t cast in the backend) and wrote into a zero-byte block (audit #17).
    if (required_bytes == kNoLimit) {
        throw SimulatorError{
            std::string{backend_name},
            "state vector size is not representable for n_qubits = " +
                std::to_string(num_qubits) +
                " (2^(n+5) exceeds 64-bit byte counts); the maximum modellable "
                "size is n_qubits = 58"};
    }

    // required <= available passes; kNoLimit (unreadable budget) on the *available*
    // side still passes and defers to the post-hoc path.
    if (required_bytes <= available) {
        return;
    }

    // Byte-level diagnostic before throwing
    // — the exception message carries the user-facing GiB form.
    log::error("OOM precheck: required=" + std::to_string(required_bytes) +
               " bytes, available=" + std::to_string(available) +
               " bytes (80% budget)");

    // Message format per / memory_check.hpp contract. SimulatorError
    // prepends "[<backend_name>] " automatically (error.hpp format_).
    throw SimulatorError{
        std::string{backend_name},
        "insufficient memory: requires " + to_gib(required_bytes) +
            " GiB, available " + to_gib(available) +
            " GiB (n_qubits = " + std::to_string(num_qubits) +
            "); QPLAYER_MEMORY_BUDGET_GB overrides this check but does not shrink"
            " the allocation, so the host may swap or be OOM-killed instead"};
}

std::uint64_t peak_state_bytes(QubitIndex num_qubits, WorkingSet ws) {
    // 2^n * 16 = 2^(n+4); the Execute 2x term reaches 2^(n+5). Refuse at n>=59
    // so neither overflows uint64 (max ~2^64), rather than silently wrapping
    // Any n this large dwarfs every real machine anyway.
    if (num_qubits >= 59) {
        return kNoLimit;
    }
    const std::uint64_t base = (std::uint64_t{1} << num_qubits) * 16ULL;
    // Execute layers one transient buffer (reorder scratch or sampling prob/cdf,
    // non-overlapping) on top of the base -> 2x. Materialize is the lone extra
    // AoS copy -> 1x, since the resident base is already reflected in the live
    // available_memory_bytes() at get_statevector() time.
    return (ws == WorkingSet::Execute) ? base * 2ULL : base;
}

std::uint32_t max_qubits_for_budget(WorkingSet ws) {
    const std::uint64_t budget = available_memory_bytes();
    // peak_state_bytes is monotonically increasing in n, so walk up until it no
    // longer fits. When budget == kNoLimit (no usable reading) every n < 59
    // "fits" and the overflow guard caps the answer at 58 — consistent with the
    // prepare() precheck also being disabled in that case.
    std::uint32_t best = 0;
    for (std::uint32_t n = 1; n < 59; ++n) {
        if (peak_state_bytes(static_cast<QubitIndex>(n), ws) <= budget) {
            best = n;
        } else {
            break;
        }
    }
    return best;
}

}  // namespace qplayer::detail
