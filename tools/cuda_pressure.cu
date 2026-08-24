#include "memory_pressure/runtime.h"
#include "memory_pressure/providers/cuda.h"
#include <cstdio>
#include <cuda_runtime.h>
using namespace memory_pressure;
int main() {
    PressureRuntime rt;
    rt.register_provider(std::make_shared<CudaDeviceProvider>());
    auto snap0 = rt.refresh(1000);
    PressureDomainId gpuId; bool found = false;
    for (const auto& d : snap0->domains)
        if (d.type == DomainType::AcceleratorMemory && d.validity == Validity::Valid) { gpuId = d.id; found = true; }
    if (!found) { std::printf("no CUDA device domain\n"); return 1; }
    const DomainState* b0 = snap0->find_domain(gpuId);
    // Govern the accelerator to 4 GiB usable so we can climb from NORMAL.
    Budget b; b.hard_capacity = 4ULL*1024*1024*1024; b.emergency_reserve_bytes = 256ULL*1024*1024;
    rt.set_budget(gpuId, b);
    std::printf("baseline device committed=%llu byte (%.2f GiB); governed budget=4 GiB\n",
        (unsigned long long)(b0?b0->committed:0), (b0?b0->committed/1073741824.0:0.0));
    void* ptrs[24]; int n = 0;
    const std::uint64_t chunk = 128ULL*1024*1024;
    std::printf("allocating bounded device memory in 128 MiB chunks, refreshing each step:\n");
    const char* last = "";
    for (int i = 0; i < 24; ++i) {
        void* p = nullptr;
        if (cudaMalloc(&p, chunk) != cudaSuccess) { std::printf("  cudaMalloc failed at chunk %d (bounded, safe)\n", i+1); break; }
        ptrs[n++] = p;
        auto sn = rt.refresh(1000ULL + (i+1)*100);
        const DomainState* d = sn->find_domain(gpuId);
        const char* lvl = d ? to_string(d->level) : "?";
        if (std::string(lvl) != std::string(last)) {
            std::printf("  +%2d %s: (%.2f GiB) level=%s\n", i+1, last, (d?d->committed/1073741824.0:0.0), lvl);
            last = lvl;
        }
    }
    for (int i = 0; i < n; ++i) cudaFree(ptrs[i]);
    std::printf("-- freeing all %d allocations --\n", n);
    auto sn2 = rt.refresh(20000);
    const DomainState* d2 = sn2->find_domain(gpuId);
    std::printf("  after full release: level=%s committed=%.2f GiB\n",
        d2?to_string(d2->level):"?", d2?d2->committed/1073741824.0:0.0);
    return 0;
}
