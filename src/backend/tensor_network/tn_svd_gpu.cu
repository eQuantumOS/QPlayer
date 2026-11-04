/// @file tn_svd_gpu.cu
/// @brief cusolverDnZgesvd offload for the MPS two-site SVD.
///
/// Measured on H100 NVL against single-threaded OpenBLAS (the production
/// setting): 20-26x at the default chi=256, with the host round trip
/// at 1% of the solve. gesvdj (Jacobi) was rejected -- it is ~2x slower here on
/// the low-rank matrices an MPS block actually produces, which is the opposite
/// of the usual expectation for matrices this size.
///
/// Resources live in a fixed pool of slots, kGpuSvdSlotsPerDevice per device,
/// each with its own non-blocking stream and cuSolver handle. A
/// trajectory thread borrows a slot for one solve. The pool replaced one context
/// per thread, which put every thread on device 0's legacy default stream and
/// ran 7x slower than the CPU at 64 trajectories.

#include "backend/tensor_network/tn_svd_gpu.hpp"

#include <cuda_runtime.h>
#include <cusolverDn.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

extern "C" int qplayer_cuda_device_count();

namespace qplayer::detail {

namespace {

// Device buffers are grown on demand and kept for the process lifetime.
// Allocating per call would put a cudaMalloc in front of every two-qubit gate.
struct Slot {
    int                device = 0;
    cusolverDnHandle_t handle = nullptr;
    cudaStream_t       stream = nullptr;

    cuDoubleComplex* d_a    = nullptr;
    cuDoubleComplex* d_u    = nullptr;
    cuDoubleComplex* d_vt   = nullptr;
    cuDoubleComplex* d_work = nullptr;
    double*          d_s    = nullptr;
    double*          d_rwork = nullptr;
    int*             d_info = nullptr;

    // Pinned host staging. A copy from pageable memory goes through the driver's
    // own staging buffer, which serialises across threads: measured 59.5 s at 8
    // trajectory threads but 75-77 s at 16-64 on the same pool.
    cuDoubleComplex* h_a    = nullptr;
    cuDoubleComplex* h_u    = nullptr;
    cuDoubleComplex* h_vt   = nullptr;
    double*          h_s    = nullptr;
    int*             h_info = nullptr;

    std::size_t cap_a = 0, cap_u = 0, cap_vt = 0, cap_work = 0, cap_s = 0, cap_rwork = 0;
    std::size_t cap_ha = 0, cap_hu = 0, cap_hvt = 0, cap_hs = 0;

    ~Slot() {
        // Best effort: at process teardown the CUDA context may already be gone,
        // and a failure here has nothing left to report to.
        cudaSetDevice(device);
        cudaFree(d_a); cudaFree(d_u); cudaFree(d_vt); cudaFree(d_work);
        cudaFree(d_s); cudaFree(d_rwork); cudaFree(d_info);
        cudaFreeHost(h_a); cudaFreeHost(h_u); cudaFreeHost(h_vt);
        cudaFreeHost(h_s); cudaFreeHost(h_info);
        if (handle != nullptr) cusolverDnDestroy(handle);
        if (stream != nullptr) cudaStreamDestroy(stream);
    }
};

// The current device is per host thread and other GPU code may run on the same
// OpenMP worker, so a solve selects its slot's device and puts the old one back.
class DeviceScope {
public:
    explicit DeviceScope(int device) : device_(device) {
        ok_ = cudaGetDevice(&prev_) == cudaSuccess &&
              (prev_ == device_ || cudaSetDevice(device_) == cudaSuccess);
    }
    ~DeviceScope() {
        if (ok_ && prev_ != device_) cudaSetDevice(prev_);
    }
    DeviceScope(const DeviceScope&) = delete;
    DeviceScope& operator=(const DeviceScope&) = delete;
    bool ok() const { return ok_; }

private:
    int  device_;
    int  prev_ = 0;
    bool ok_   = false;
};

class SlotPool {
public:
    SlotPool() {
        const int ndev = qplayer_cuda_device_count();
        if (ndev <= 0) return;
        cudaDeviceProp ref{};
        if (cudaGetDeviceProperties(&ref, 0) != cudaSuccess) return;
        for (int dev = 0; dev < ndev; ++dev) {
            // Only devices of device 0's model: the solve then gives the same bits
            // whichever slot a trajectory draws, so seeded runs stay reproducible.
            cudaDeviceProp prop{};
            if (cudaGetDeviceProperties(&prop, dev) != cudaSuccess ||
                std::strcmp(prop.name, ref.name) != 0 || prop.major != ref.major ||
                prop.minor != ref.minor) {
                continue;
            }
            const DeviceScope scope(dev);
            if (!scope.ok()) continue;
            for (int i = 0; i < kGpuSvdSlotsPerDevice; ++i) {
                auto slot = std::make_unique<Slot>();
                slot->device = dev;
                if (cudaStreamCreateWithFlags(&slot->stream, cudaStreamNonBlocking) != cudaSuccess ||
                    cusolverDnCreate(&slot->handle) != CUSOLVER_STATUS_SUCCESS ||
                    cusolverDnSetStream(slot->handle, slot->stream) != CUSOLVER_STATUS_SUCCESS) {
                    continue;
                }
                free_.push_back(slot.get());
                owned_.push_back(std::move(slot));
            }
        }
        live_ = static_cast<int>(free_.size());
    }

    // nullptr once every slot has been retired, so the caller falls back to the
    // CPU instead of waiting for a slot that will never come back.
    Slot* acquire() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !free_.empty() || live_ == 0; });
        if (free_.empty()) return nullptr;
        Slot* slot = free_.back();
        free_.pop_back();
        return slot;
    }

    void release(Slot* slot) {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            free_.push_back(slot);
        }
        cv_.notify_one();
    }

    // One hard failure is enough: a device that cannot solve this will not solve
    // the next either, and retrying per gate would turn a fast run into a slow
    // one that also spams the log. The slot stays owned, only unreachable.
    void retire() {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            --live_;
        }
        cv_.notify_all();   // waiters must see live_ == 0 if this was the last
    }

    int live() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return live_;
    }

private:
    std::vector<std::unique_ptr<Slot>> owned_;
    std::vector<Slot*>                 free_;
    int                                live_ = 0;
    std::mutex                         mutex_;
    std::condition_variable            cv_;
};

SlotPool& pool() {
    static SlotPool instance;
    return instance;
}

template <typename T>
bool ensure(T** ptr, std::size_t* cap, std::size_t want) {
    if (*cap >= want) return true;
    cudaFree(*ptr);
    *ptr = nullptr;
    *cap = 0;
    void* p = nullptr;
    if (cudaMalloc(&p, sizeof(T) * want) != cudaSuccess) return false;
    *ptr = static_cast<T*>(p);
    *cap = want;
    return true;
}

template <typename T>
bool ensure_pinned(T** ptr, std::size_t* cap, std::size_t want) {
    if (*cap >= want) return true;
    cudaFreeHost(*ptr);
    *ptr = nullptr;
    *cap = 0;
    void* p = nullptr;
    if (cudaMallocHost(&p, sizeof(T) * want) != cudaSuccess) return false;
    *ptr = static_cast<T*>(p);
    *cap = want;
    return true;
}

// Conjugate transpose on the host. Only used on the rows < cols branch, where it
// costs O(rows*cols) against a solve that is O(n^3) -- doing it with cuBLAS
// would add a second library handle to manage for no measurable gain.
void conj_transpose(const std::complex<double>* src, int rows, int cols,
                    std::vector<std::complex<double>>& dst) {
    dst.resize(static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols));
    for (int j = 0; j < cols; ++j)
        for (int i = 0; i < rows; ++i)
            dst[static_cast<std::size_t>(j) + static_cast<std::size_t>(i) * cols] =
                std::conj(src[static_cast<std::size_t>(i) + static_cast<std::size_t>(j) * rows]);
}

bool solve(Slot& g, int m, int n, const std::complex<double>* a, double* s,
           std::complex<double>* u, std::complex<double>* vt) {
    // Caller guarantees m >= n here; cuSolver's gesvd requires it.
    const int k = n;
    const std::size_t a_elems  = static_cast<std::size_t>(m) * n;
    const std::size_t u_elems  = static_cast<std::size_t>(m) * k;
    const std::size_t vt_elems = static_cast<std::size_t>(k) * n;

    if (!ensure(&g.d_a, &g.cap_a, a_elems) ||
        !ensure(&g.d_u, &g.cap_u, u_elems) ||
        !ensure(&g.d_vt, &g.cap_vt, vt_elems) ||
        !ensure(&g.d_s, &g.cap_s, static_cast<std::size_t>(k)) ||
        !ensure(&g.d_rwork, &g.cap_rwork, static_cast<std::size_t>(5 * k))) {
        return false;
    }
    if (g.d_info == nullptr &&
        cudaMalloc(reinterpret_cast<void**>(&g.d_info), sizeof(int)) != cudaSuccess) {
        return false;
    }

    int lwork = 0;
    if (cusolverDnZgesvd_bufferSize(g.handle, m, n, &lwork) != CUSOLVER_STATUS_SUCCESS) {
        return false;
    }
    if (!ensure(&g.d_work, &g.cap_work, static_cast<std::size_t>(std::max(lwork, 1)))) {
        return false;
    }

    if (!ensure_pinned(&g.h_a, &g.cap_ha, a_elems) ||
        !ensure_pinned(&g.h_u, &g.cap_hu, u_elems) ||
        !ensure_pinned(&g.h_vt, &g.cap_hvt, vt_elems) ||
        !ensure_pinned(&g.h_s, &g.cap_hs, static_cast<std::size_t>(k))) {
        return false;
    }
    if (g.h_info == nullptr &&
        cudaMallocHost(reinterpret_cast<void**>(&g.h_info), sizeof(int)) != cudaSuccess) {
        return false;
    }

    // Everything is queued on the slot's own stream and waited for once. A
    // synchronous cudaMemcpy would go through the legacy default stream and
    // serialise every slot on the device again.
    std::memcpy(g.h_a, a, sizeof(cuDoubleComplex) * a_elems);
    if (cudaMemcpyAsync(g.d_a, g.h_a, sizeof(cuDoubleComplex) * a_elems,
                        cudaMemcpyHostToDevice, g.stream) != cudaSuccess) {
        return false;
    }
    if (cusolverDnZgesvd(g.handle, 'S', 'S', m, n, g.d_a, m, g.d_s,
                         g.d_u, m, g.d_vt, k, g.d_work, lwork,
                         g.d_rwork, g.d_info) != CUSOLVER_STATUS_SUCCESS) {
        return false;
    }
    if (cudaMemcpyAsync(g.h_info, g.d_info, sizeof(int), cudaMemcpyDeviceToHost,
                        g.stream) != cudaSuccess ||
        cudaMemcpyAsync(g.h_s, g.d_s, sizeof(double) * static_cast<std::size_t>(k),
                        cudaMemcpyDeviceToHost, g.stream) != cudaSuccess ||
        cudaMemcpyAsync(g.h_u, g.d_u, sizeof(cuDoubleComplex) * u_elems,
                        cudaMemcpyDeviceToHost, g.stream) != cudaSuccess ||
        cudaMemcpyAsync(g.h_vt, g.d_vt, sizeof(cuDoubleComplex) * vt_elems,
                        cudaMemcpyDeviceToHost, g.stream) != cudaSuccess ||
        cudaStreamSynchronize(g.stream) != cudaSuccess) {
        return false;
    }
    if (*g.h_info != 0) return false;      // non-convergence: let the CPU try
    std::memcpy(s, g.h_s, sizeof(double) * static_cast<std::size_t>(k));
    std::memcpy(static_cast<void*>(u), g.h_u, sizeof(cuDoubleComplex) * u_elems);
    std::memcpy(static_cast<void*>(vt), g.h_vt, sizeof(cuDoubleComplex) * vt_elems);
    return true;
}

// Returns the slot on every path, including the early returns inside solve().
class SlotLease {
public:
    SlotLease() : slot_(pool().acquire()) {}
    ~SlotLease() {
        if (slot_ == nullptr) return;
        if (failed_) pool().retire();
        else         pool().release(slot_);
    }
    SlotLease(const SlotLease&) = delete;
    SlotLease& operator=(const SlotLease&) = delete;
    Slot* get() const { return slot_; }
    void  mark_failed() { failed_ = true; }

private:
    Slot* slot_;
    bool  failed_ = false;
};

}  // namespace

bool gpu_svd_available() {
    static const bool present = (qplayer_cuda_device_count() > 0);
    return present;
}

int gpu_svd_slot_count() {
    return gpu_svd_available() ? pool().live() : 0;
}

bool gpu_zgesvd_thin(int rows, int cols, const std::complex<double>* a,
                     double* s, std::complex<double>* u, std::complex<double>* vt) {
    if (!gpu_svd_available()) return false;
    if (std::min(rows, cols) < kGpuSvdMinDim) return false;

    SlotLease lease;
    if (lease.get() == nullptr) return false;
    Slot& slot = *lease.get();
    const DeviceScope scope(slot.device);
    if (!scope.ok()) {
        lease.mark_failed();
        return false;
    }

    const int k = std::min(rows, cols);
    bool ok = false;
    if (rows >= cols) {
        ok = solve(slot, rows, cols, a, s, u, vt);
    } else {
        // A^H is cols x rows, so it satisfies m >= n. From A^H = U' S VT' we get
        // A = (VT')^H S (U')^H, i.e. U = (VT')^H and VT = (U')^H.
        std::vector<std::complex<double>> ah;
        conj_transpose(a, rows, cols, ah);
        std::vector<std::complex<double>> up(static_cast<std::size_t>(cols) * k);
        std::vector<std::complex<double>> vtp(static_cast<std::size_t>(k) * rows);
        ok = solve(slot, cols, rows, ah.data(), s, up.data(), vtp.data());
        if (ok) {
            for (int j = 0; j < k; ++j)
                for (int i = 0; i < rows; ++i)
                    u[static_cast<std::size_t>(i) + static_cast<std::size_t>(j) * rows] =
                        std::conj(vtp[static_cast<std::size_t>(j) + static_cast<std::size_t>(i) * k]);
            for (int j = 0; j < cols; ++j)
                for (int i = 0; i < k; ++i)
                    vt[static_cast<std::size_t>(i) + static_cast<std::size_t>(j) * k] =
                        std::conj(up[static_cast<std::size_t>(j) + static_cast<std::size_t>(i) * cols]);
        }
    }
    if (!ok) lease.mark_failed();
    return ok;
}

}  // namespace qplayer::detail
