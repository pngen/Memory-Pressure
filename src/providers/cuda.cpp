#include "memory_pressure/providers/cuda.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace memory_pressure {

namespace {

void* load_symbol(void* lib, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    return reinterpret_cast<void*>(dlsym(lib, name));
#endif
}

// Stable per-device identity: prefer the CUDA device UUID; fall back to a hash
// of name + ordinal when the UUID is unavailable.
PressureDomainId DeviceDomainId(const unsigned char* uuid, bool have_uuid,
                                const char* name, int ordinal) {
    if (have_uuid) {
        std::uint64_t hi = 0, lo = 0;
        for (int j = 0; j < 8; ++j) hi = (hi << 8) | uuid[j];
        for (int j = 8; j < 16; ++j) lo = (lo << 8) | uuid[j];
        return PressureDomainId{hi, lo};
    }
    const std::string key = std::string(name) + "#" + std::to_string(ordinal);
    return PressureDomainId{0x63756461ULL, PressureDomainId::fnv1a(key)};
}

std::string uuid_hex(const unsigned char* uuid, bool have_uuid) {
    if (!have_uuid) return std::string();
    const char* hexc = "0123456789abcdef";
    std::string s;
    s.reserve(32);
    for (int j = 0; j < 16; ++j) {
        s.push_back(hexc[(uuid[j] >> 4) & 0xF]);
        s.push_back(hexc[uuid[j] & 0xF]);
    }
    return s;
}

#ifdef _WIN32
HMODULE find_cudart() {
    const wchar_t* names[] = { L"cudart64_13.dll", L"cudart64_12.dll", L"cudart64_11.dll" };
    for (const wchar_t* n : names) { HMODULE h = LoadLibraryW(n); if (h) return h; }
    // Search the NVIDIA CUDA toolkit installation directories.
    const std::filesystem::path base = L"C:\\Program Files\\NVIDIA GPU Computing Toolkit\\CUDA";
    if (std::filesystem::exists(base)) {
        // Gather candidate paths, preferring the highest toolkit version.
        std::vector<std::wstring> candidates;
        std::error_code ec;
        for (auto it = std::filesystem::directory_iterator(base, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            if (!it->is_directory()) continue;
            for (const auto& sub : { L"bin", L"bin\x64" }) {
                auto bin = it->path() / sub;
                std::error_code ec2;
                for (auto f = std::filesystem::directory_iterator(bin, ec2); !ec2 && f != std::filesystem::directory_iterator(); f.increment(ec2)) {
                    const auto name = f->path().filename().wstring();
                    if (name.find(L"cudart64_") == 0 && name.find(L".dll") != std::wstring::npos) {
                        candidates.push_back(f->path().wstring());
                    }
                }
            }
        }
        // Prefer the highest-version DLL (larger numeric part).
        auto version_of = [](const std::wstring& p) -> int {
            size_t s = p.find(L"cudart64_"); if (s == std::wstring::npos) return 0;
            size_t e = p.find(L".dll", s); if (e == std::wstring::npos) e = p.size();
            std::wstring num = p.substr(s + 9, e - (s + 9));
            try { return std::stoi(num); } catch (...) { return 0; }
        };
        std::stable_sort(candidates.begin(), candidates.end(), [&](const std::wstring& a, const std::wstring& b) {
            return version_of(a) > version_of(b);
        });
        for (const auto& c : candidates) { HMODULE h = LoadLibraryW(c.c_str()); if (h) return h; }
    }
    return nullptr;
}
#endif

} // anonymous namespace

struct CudaDeviceProvider::Impl {
    bool loaded = false;
    bool cuda_available = false;
    int device_count = 0;
    bool probes_enabled = false;

    // Driver API (identity / utilities).
    void* nvcuda = nullptr;
    int (*cuInitFn)(unsigned int) = nullptr;
    int (*cuDeviceGetCountFn)(int*) = nullptr;
    int (*cuDeviceGetFn)(int*, int) = nullptr;
    int (*cuDeviceGetNameFn)(char*, int, int) = nullptr;
    int (*cuDeviceGetUuidFn)(unsigned char*, int) = nullptr;

    // Runtime API (memory + allocation).  The runtime reports authoritative
    // per-device totals on WDDM where the driver cuMemGetInfo may be capped.
    void* cudart = nullptr;
    int (*cudaSetDeviceFn)(int) = nullptr;
    int (*cudaGetDeviceCountFn)(int*) = nullptr;
    int (*cudaMemGetInfoFn)(std::uint64_t*, std::uint64_t*) = nullptr;
    int (*cudaMallocFn)(void**, std::uint64_t) = nullptr;
    int (*cudaFreeFn)(void*) = nullptr;
};

CudaDeviceProvider::CudaDeviceProvider(bool enable_probes)
    : impl_(std::make_unique<Impl>()) {
    impl_->probes_enabled = enable_probes;

    // Runtime API first (authoritative memory).
#ifdef _WIN32
    impl_->cudart = find_cudart();
#else
    impl_->cudart = dlopen("libcudart.so.12", RTLD_NOW | RTLD_GLOBAL);
    if (!impl_->cudart) impl_->cudart = dlopen("libcudart.so", RTLD_NOW | RTLD_GLOBAL);
#endif
    if (impl_->cudart) {
        impl_->cudaSetDeviceFn = reinterpret_cast<int(*)(int)>(load_symbol(impl_->cudart, "cudaSetDevice"));
        impl_->cudaGetDeviceCountFn = reinterpret_cast<int(*)(int*)>(load_symbol(impl_->cudart, "cudaGetDeviceCount"));
        impl_->cudaMemGetInfoFn = reinterpret_cast<int(*)(std::uint64_t*, std::uint64_t*)>(load_symbol(impl_->cudart, "cudaMemGetInfo"));
        impl_->cudaMallocFn = reinterpret_cast<int(*)(void**, std::uint64_t)>(load_symbol(impl_->cudart, "cudaMalloc"));
        impl_->cudaFreeFn = reinterpret_cast<int(*)(void*)>(load_symbol(impl_->cudart, "cudaFree"));
    }

    // Driver API for identity.
#ifdef _WIN32
    impl_->nvcuda = LoadLibraryW(L"nvcuda.dll");
#else
    impl_->nvcuda = dlopen("libcuda.so.1", RTLD_NOW | RTLD_GLOBAL);
#endif
    if (impl_->nvcuda) {
        impl_->cuInitFn = reinterpret_cast<int(*)(unsigned int)>(load_symbol(impl_->nvcuda, "cuInit"));
        impl_->cuDeviceGetCountFn = reinterpret_cast<int(*)(int*)>(load_symbol(impl_->nvcuda, "cuDeviceGetCount"));
        impl_->cuDeviceGetNameFn = reinterpret_cast<int(*)(char*, int, int)>(load_symbol(impl_->nvcuda, "cuDeviceGetName"));
        impl_->cuDeviceGetUuidFn = reinterpret_cast<int(*)(unsigned char*, int)>(load_symbol(impl_->nvcuda, "cuDeviceGetUuid"));
        impl_->cuDeviceGetFn = reinterpret_cast<int(*)(int*, int)>(load_symbol(impl_->nvcuda, "cuDeviceGet"));
    }

    int count = 0;
    if (impl_->cudart && impl_->cudaGetDeviceCountFn && impl_->cudaGetDeviceCountFn(&count) == 0 && count > 0) {
        impl_->device_count = count;
        impl_->cuda_available = true;
    }
    if (impl_->nvcuda && impl_->cuInitFn && impl_->cuInitFn(0) == 0 &&
        impl_->cuDeviceGetCountFn && impl_->cuDeviceGetCountFn(&count) == 0 && count > 0) {
        impl_->device_count = count;
        impl_->cuda_available = true;
    }
    impl_->loaded = impl_->cuda_available;
}

CudaDeviceProvider::~CudaDeviceProvider() {
    // Deliberately do NOT unload the dynamically-loaded CUDA libraries here.
    // Unloading the CUDA runtime while it still holds initialized driver state
    // can abort the process during teardown (observed as exit code 3).  The OS
    // reclaims these modules at process exit, which is the robust pattern for
    // optional dynamic dependencies.
    impl_->nvcuda = nullptr;
    impl_->cudart = nullptr;
}

bool CudaDeviceProvider::supported() const noexcept { return impl_ && impl_->loaded; }
int CudaDeviceProvider::device_count() const noexcept { return impl_ ? impl_->device_count : 0; }

ProviderSample CudaDeviceProvider::sample(std::uint64_t now_ms) {
    ProviderSample out;
    out.sampled_at_ms = now_ms;
    if (!supported()) {
        out.status = ProviderStatus::Unavailable;
        out.error = "CUDA runtime/driver not available";
        return out;
    }
    out.status = ProviderStatus::Healthy;

    for (int i = 0; i < impl_->device_count; ++i) {
        // Identity from the driver API where available.  The device handle is
        // fetched via the API (ordinal != handle is not guaranteed).
        int devHandle = i;
        if (impl_->cuDeviceGetFn) impl_->cuDeviceGetFn(&devHandle, i);
        char devName[256] = {0};
        if (impl_->cuDeviceGetNameFn) impl_->cuDeviceGetNameFn(devName, static_cast<int>(sizeof(devName)), devHandle);
        unsigned char uuid[16] = {0};
        bool have_uuid = (impl_->cuDeviceGetUuidFn && impl_->cuDeviceGetUuidFn(uuid, devHandle) == 0);

        std::uint64_t freeMem = 0, totalMem = 0;
        bool ok = false;
        if (impl_->cudart && impl_->cudaSetDeviceFn && impl_->cudaSetDeviceFn(i) == 0 &&
            impl_->cudaMemGetInfoFn && impl_->cudaMemGetInfoFn(&freeMem, &totalMem) == 0) {
            ok = true;
        }

        DomainObservation o;
        o.id = DeviceDomainId(uuid, have_uuid, devName, i);
        o.type = DomainType::AcceleratorMemory;
        o.provider = name();
        o.native_resource_id = uuid_hex(uuid, have_uuid) + "|" + std::string(devName);
        if (ok) {
            o.total_capacity = totalMem;
            o.committed = totalMem > freeMem ? totalMem - freeMem : 0;
            o.resident = o.committed;
            o.available = freeMem;
            o.confidence = Confidence::High;
            o.provenance = Provenance::CudaRuntimeApi;
            o.validity = Validity::Valid;
            o.detail = "device=" + std::string(devName) + " ordinal=" + std::to_string(i) +
                       " total=" + std::to_string(totalMem) + " free=" + std::to_string(freeMem) +
                       " via=cudaMemGetInfo(runtime)";
        } else {
            o.confidence = Confidence::Unknown;
            o.provenance = Provenance::CudaRuntimeApi;
            o.validity = Validity::Failed;
            o.detail = "cudaMemGetInfo failed for device " + std::to_string(i);
            out.status = ProviderStatus::Partial;
        }
        o.observed_at_ms = now_ms;
        out.observations.push_back(std::move(o));
    }
    return out;
}

bool CudaDeviceProvider::probe_device(int ordinal, std::uint64_t bytes) {
    if (!supported() || !impl_->cudart) return false;
    if (ordinal < 0 || ordinal >= impl_->device_count) return false;
    if (impl_->cudaSetDeviceFn && impl_->cudaSetDeviceFn(ordinal) != 0) return false;
    void* ptr = nullptr;
    if (impl_->cudaMallocFn && impl_->cudaMallocFn(&ptr, bytes) == 0) {
        const bool ok = impl_->cudaFreeFn && impl_->cudaFreeFn(ptr) == 0;
        return ok;
    }
    return false;
}

} // namespace memory_pressure
