#include <iostream>
#include "memory_pressure/runtime.h"
#include "memory_pressure/providers/synthetic.h"
using namespace memory_pressure;
int main() {
    PressureRuntime rt;
    std::vector<SyntheticDomainSpec> ds;
    for (int i = 0; i < 4; ++i) {
        SyntheticDomainSpec d; d.id = PressureDomainId{9, static_cast<std::uint64_t>(i)};
        d.type = DomainType::AcceleratorMemory; d.group = "gpu"; d.total_capacity = 8ULL*1024*1024*1024;
        d.phase = static_cast<double>(i)/4.0; ds.push_back(d);
    }
    rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::MultiGpu, 100, 4));
    Budget b; b.hard_capacity = 2ULL*1024*1024*1024;
    for (int i = 0; i < 4; ++i) rt.set_budget(PressureDomainId{9, static_cast<std::uint64_t>(i)}, b);
    auto sn = rt.refresh(1000);
    std::cout << "multi-GPU domains: " << sn->domains.size() << std::endl;
    for (const auto& d : sn->domains) std::cout << "  " << d.id.to_hex() << " " << to_string(d.level) << std::endl;
    return 0;
}
