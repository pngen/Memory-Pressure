#include <iostream>
#include "memory_pressure/runtime.h"
#include "memory_pressure/providers/synthetic.h"
using namespace memory_pressure;
int main() {
    PressureRuntime rt;
    std::vector<SyntheticDomainSpec> ds;
    SyntheticDomainSpec d; d.id = PressureDomainId{3,3}; d.type = DomainType::AcceleratorMemory;
    d.group = "gpu"; d.total_capacity = 4ULL*1024*1024*1024; ds.push_back(d);
    rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::GradualGrowth, 100, 4));
    Budget b; b.hard_capacity = 256ULL*1024*1024; rt.set_budget(d.id, b);
    auto sub = rt.subscribe(SubscriptionFilter{});
    for (int i = 0; i < 30; ++i) rt.refresh(1000 + i*100);
    Backpressure bp = rt.backpressure(d.id);
    std::cout << "backpressure severity=" << to_string(bp.severity)
              << " recommended=" << to_string(bp.recommended_response)
              << " max_new_allocation=" << bp.max_new_allocation << std::endl;
    PressureEvent e;
    while (sub->try_pop(e))
        if (e.type == PressureEventType::BackpressureIssued)
            std::cout << "  event: " << e.detail << std::endl;
    return 0;
}
