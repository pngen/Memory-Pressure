#include <iostream>
#include "memory_pressure/runtime.h"
#include "memory_pressure/providers/synthetic.h"
using namespace memory_pressure;
int main() {
    PressureRuntime rt;
    std::vector<SyntheticDomainSpec> ds;
    SyntheticDomainSpec d; d.id = PressureDomainId{1,1}; d.type = DomainType::AcceleratorMemory;
    d.group = "gpu"; d.total_capacity = 32ULL*1024*1024*1024; ds.push_back(d);
    rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::GradualGrowth, 100, 4));
    Budget b; b.hard_capacity = 24ULL*1024*1024*1024; b.emergency_reserve_bytes = 2ULL*1024*1024*1024;
    if (auto err = rt.set_budget(d.id, b)) { std::cerr << "budget rejected: " << *err << std::endl; return 1; }
    auto snap = rt.refresh(1000);
    const DomainState* st = snap->find_domain(d.id);
    std::cout << "governed capacity=" << b.usable_capacity() << " available=" << (st ? st->available : 0) << std::endl;
    return 0;
}
