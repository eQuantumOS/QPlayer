// error.hpp — QPlayer exception hierarchy.
// All throws must use one of these types so Python bindings can map them via
// py::register_exception. std::runtime_error /
// std::exception direct throws are forbidden.

#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace qplayer {

// Root of the QPlayer exception hierarchy. All other QPlayer exceptions derive
// from this type so callers can catch the entire family with a single handler.
class QPlayerError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Raised by the OpenQASM v2 parser when lexing or semantic validation fails.
// file/line/col are captured so error messages can pinpoint the source
// location.
class QASMParseError : public QPlayerError {
public:
    QASMParseError(std::string file, int line, int col, const std::string& msg)
        : QPlayerError(format_(file, line, col, msg)),
          file_(std::move(file)), line_(line), col_(col) {}

    const std::string& file() const noexcept { return file_; }
    int line() const noexcept { return line_; }
    int col() const noexcept { return col_; }

private:
    static std::string format_(const std::string& f, int l, int c, const std::string& m) {
        return f + ":" + std::to_string(l) + ":" + std::to_string(c) + ": " + m;
    }
    std::string file_;
    int         line_;
    int         col_;
};

// Raised for circuit-construction or IR-validation failures (out-of-range qubit
// index, undefined opaque, malformed creg, etc.).
class CircuitError : public QPlayerError {
public:
    explicit CircuitError(const std::string& msg) : QPlayerError(msg) {}
};

// Specialization of CircuitError raised when a 2-qubit gate violates the
// device coupling map after layout/routing. Kept as a
// subclass so callers can catch CircuitError generically while still
// dispatching on connectivity failure when needed.
class ConnectivityError : public CircuitError {
public:
    explicit ConnectivityError(const std::string& msg) : CircuitError(msg) {}
};

// Raised by simulator backends for backend-specific runtime failures (e.g.
// TN does not support Interactive, SparseState rejects IdleOp). bt is a short
// backend identifier literal — "FullState", "FullStateGPU", "SparseState",
// "TensorNetwork", or "GPU". Member kept as std::string
// (not string_view) per decision — callers may construct with a
// dynamically built std::string (e.g. "out of memory at trajectory N"),
// so storing a view would dangle.
class SimulatorError : public QPlayerError {
public:
    SimulatorError(std::string bt, const std::string& msg)
        : QPlayerError(format_(bt, msg)), backend_(std::move(bt)) {}

    std::string_view backend() const noexcept { return backend_; }

private:
    static std::string format_(const std::string& bt, const std::string& m) {
        std::string out;
        out.reserve(bt.size() + m.size() + 4);
        out.append("[").append(bt).append("] ").append(m);
        return out;
    }
    std::string backend_;
};

// Raised when a CUDA runtime call fails. cuda_code preserves the original
// cudaError_t for caller diagnostics — wrap immediately on cudaGetLastError()
// The field uses `int` instead of cudaError_t
// to avoid pulling cuda_runtime.h into CPU-only builds. cudaError_t
// is an enum with integer ABI, so callers pass it implicitly.
class GPUError : public SimulatorError {
public:
    GPUError(int cuda_code, const std::string& msg)
        : SimulatorError("GPU", msg), cuda_code_(cuda_code) {}

    int cuda_code() const noexcept { return cuda_code_; }

private:
    int cuda_code_;
};

// Used by Phase 0-S stub bodies to signal an API surface that is declared but
// not yet implemented. EXPECT_NOT_IMPLEMENTED in tests verifies the stub
// boundary explicitly. Members kept as
// std::string (not string_view) so callers may construct with dynamically
// built phase messages without dangling — same rationale as SimulatorError.
class NotImplementedError : public QPlayerError {
public:
    NotImplementedError(std::string module, std::string phase_msg)
        : QPlayerError(format_(module, phase_msg)),
          module_(std::move(module)), phase_msg_(std::move(phase_msg)) {}

    std::string_view module() const noexcept { return module_; }
    std::string_view phase_msg() const noexcept { return phase_msg_; }

private:
    static std::string format_(const std::string& mod, const std::string& msg) {
        std::string out;
        out.reserve(mod.size() + msg.size() + 16);
        out.append("[").append(mod).append("] NOT_IMPLEMENTED: ").append(msg);
        return out;
    }
    std::string module_;
    std::string phase_msg_;
};

}  // namespace qplayer
