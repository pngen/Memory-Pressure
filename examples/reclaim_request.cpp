#include <iostream>
#include "memory_pressure/runtime.h"
#include "memory_pressure/providers/synthetic.h"
using namespace memory_pressure;
int main() {
    PressureRuntime rt;
    std::vector<SyntheticDomainSpec> ds;
    SyntheticDomainSpec d; d.id = PressureDomainId{4,4}; d.type = DomainType::AcceleratorMemory;
    d.group = "gpu"; d.total_capacity = 4ULL*1024*1024*1024; ds.push_back(d);
    rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::GradualGrowth, 100, 4));
    Budget b; b.hard_capacity = 128ULL*1024*1024; rt.set_budget(d.id, b);
    for (int i = 0; i < 40; ++i) rt.refresh(1000 + i*100);
    DomainResponse dr = rt.response_for(d.id);
    if (dr.relief && dr.relief->is_reclaim())
        std::cout << "reclaim target=" << dr.relief->target_bytes
                  << " urgency=" << to_string(dr.relief->urgency) << std::endl;
    else
        std::cout << "no reclaim request (level=" << to_string(dr.level) << ")" << std::endl;
    return 0;
}
