#include <iostream>
#include "memory_pressure/runtime.h"
#include "memory_pressure/providers/synthetic.h"
using namespace memory_pressure;
int main() {
    PressureRuntime rt;
    std::vector<SyntheticDomainSpec> ds;
    SyntheticDomainSpec d; d.id = PressureDomainId{8,1}; d.type = DomainType::AcceleratorMemory; d.group = "gpu"; d.total_capacity = 4ULL*1024*1024*1024; ds.push_back(d);
    rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::SlowRecovery, 100, 4));
    Budget b; b.hard_capacity = 256ULL*1024*1024; rt.set_budget(d.id, b);
    std::cout << "slow-recovery:" << std::endl;
    std::string prev;
    for (int i = 0; i < 60; ++i) {
        auto sn = rt.refresh(1000 + i*100);
        const DomainState* st = sn->find_domain(d.id);
        const std::string lvl = st ? to_string(st->level) : "none";
        if (lvl != prev) { std::cout << "  t=" << i << " -> " << lvl << std::endl; prev = lvl; }
    }
    return 0;
}
