#include <iostream>
#include "memory_pressure/runtime.h"
#include "memory_pressure/providers/synthetic.h"
using namespace memory_pressure;
int main() {
    PressureRuntime rt;
    std::vector<SyntheticDomainSpec> ds;
    SyntheticDomainSpec g; g.id = PressureDomainId{5,1}; g.type = DomainType::AcceleratorMemory; g.group = "gpu"; g.total_capacity = 4ULL*1024*1024*1024; ds.push_back(g);
    SyntheticDomainSpec h; h.id = PressureDomainId{5,2}; h.type = DomainType::HostMemory; h.group = "host"; h.total_capacity = 16ULL*1024*1024*1024; ds.push_back(h);
    rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::Simultaneous, 100, 4));
    Budget gb; gb.hard_capacity = 200ULL*1024*1024; rt.set_budget(g.id, gb);
    Budget hb; hb.hard_capacity = 4ULL*1024*1024*1024; rt.set_budget(h.id, hb);
    for (int i = 0; i < 45; ++i) rt.refresh(1000 + i*100);
    DomainResponse dr = rt.response_for(g.id);
    std::cout << "gpu level=" << to_string(dr.level) << std::endl;
    if (dr.relief && dr.relief->is_demotion())
        std::cout << "demotion target=" << dr.relief->target_bytes
                  << " preferred_tier=" << to_string(dr.relief->preferred_tier.value_or(DomainType::Unknown)) << std::endl;
    else
        std::cout << "no demotion request (cross-tier check may have blocked it)" << std::endl;
    std::cout << "responses:";
    for (const auto& a : dr.actions) std::cout << " " << to_string(a);
    std::cout << std::endl;
    return 0;
}
