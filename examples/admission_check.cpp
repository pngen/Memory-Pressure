#include <iostream>
#include "memory_pressure/runtime.h"
#include "memory_pressure/providers/synthetic.h"
using namespace memory_pressure;
int main() {
    PressureRuntime rt;
    std::vector<SyntheticDomainSpec> ds;
    SyntheticDomainSpec d; d.id = PressureDomainId{2,2}; d.type = DomainType::AcceleratorMemory;
    d.group = "gpu"; d.total_capacity = 4ULL*1024*1024*1024; ds.push_back(d);
    rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::GradualGrowth, 100, 4));
    Budget b; b.hard_capacity = 2ULL*1024*1024*1024; rt.set_budget(d.id, b);
    rt.refresh(1000);
    const std::uint64_t req = 256ULL*1024*1024;
    AdmissionHint h = rt.admit(d.id, req);
    std::cout << "admit " << req << " bytes -> " << to_string(h.decision) << std::endl;
    std::cout << "  " << h.explanation << std::endl;
    return 0;
}
