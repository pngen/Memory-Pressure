#include "memory_pressure/runtime.h"
#include "memory_pressure/providers/windows.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <iostream>
using namespace memory_pressure;
int main() {
    // Bounded REAL host-memory pressure test against a governed budget.
    PressureRuntime rt;
    rt.register_provider(std::make_shared<WindowsHostProvider>());
    auto snap0 = rt.refresh(1000);
    const DomainState* host0 = nullptr;
    PressureDomainId hostId;
    for (const auto& d : snap0->domains)
        if (d.type == DomainType::HostMemory) { host0 = &d; hostId = d.id; }
    if (!host0) { std::cout << "no HOST_MEMORY domain" << std::endl; return 1; }
    const std::uint64_t committed0 = host0->committed;
    // Governed budget slightly above the current committed physical footprint.
    Budget b; b.hard_capacity = committed0 + 256ULL*1024*1024; b.emergency_reserve_bytes = 64ULL*1024*1024;
    if (auto e = rt.set_budget(hostId, b)) { std::cout << "budget rejected: " << *e << std::endl; return 1; }
    auto s1 = rt.refresh(2000);
    const DomainState* h1 = s1->find_domain(hostId);
    std::cout << "baseline governed: level=" << (h1?to_string(h1->level):"?") << " util=" << (h1?h1->utilization:0.0) << std::endl;

    // Commit a bounded 512 MiB and touch it so physical pages are charged.
    const std::uint64_t sz = 512ULL*1024*1024;
    unsigned char* p = static_cast<unsigned char*>(VirtualAlloc(nullptr, sz, MEM_COMMIT, PAGE_READWRITE));
    if (!p) { std::cout << "allocation failed" << std::endl; return 1; }
    for (std::uint64_t i = 0; i < sz; i += 4096) p[i] = 0xAB;
    auto s2 = rt.refresh(3000);
    const DomainState* h2 = s2->find_domain(hostId);
    std::cout << "after 512 MiB commit: level=" << (h2?to_string(h2->level):"?")
              << " util=" << (h2?h2->utilization:0.0) << " avail=" << (h2?h2->available:0) << std::endl;

    VirtualFree(p, 0, MEM_RELEASE);
    auto s3 = rt.refresh(4000);
    const DomainState* h3 = s3->find_domain(hostId);
    std::cout << "after release: level=" << (h3?to_string(h3->level):"?") << " util=" << (h3?h3->utilization:0.0) << std::endl;
    return 0;
}
