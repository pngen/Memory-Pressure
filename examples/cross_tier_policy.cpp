#include <iostream>
#include "memory_pressure/runtime.h"
#include "memory_pressure/providers/synthetic.h"
using namespace memory_pressure;
int main() {
    // GPU CRITICAL + host CRITICAL => demotion is NOT safe (cross-tier check).
    PressureRuntime rt;
    std::vector<SyntheticDomainSpec> ds;
    SyntheticDomainSpec g; g.id = PressureDomainId{6,1}; g.type = DomainType::AcceleratorMemory; g.group = "gpu"; g.total_capacity = 4ULL*1024*1024*1024; ds.push_back(g);
    SyntheticDomainSpec h; h.id = PressureDomainId{6,2}; h.type = DomainType::HostMemory; h.group = "host"; h.total_capacity = 4ULL*1024*1024*1024; ds.push_back(h);
    rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::Simultaneous, 100, 4));
    Budget gb; gb.hard_capacity = 200ULL*1024*1024; rt.set_budget(g.id, gb);
    Budget hb; hb.hard_capacity = 200ULL*1024*1024; rt.set_budget(h.id, hb);
    for (int i = 0; i < 50; ++i) rt.refresh(1000 + i*100);
    DomainResponse dr = rt.response_for(g.id);
    bool demote = false;
    for (const auto& a : dr.actions) if (a == ResponseAction::RequestDemotion) demote = true;
    std::cout << "host=";
    const DomainState* hst = rt.current_snapshot()->find_domain(h.id);
    if (hst) std::cout << to_string(hst->level);
    std::cout << " gpu=" << to_string(dr.level) << " demotion_recommended=" << (demote ? "yes" : "no") << std::endl;
    return 0;
}
