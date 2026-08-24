#include <iostream>
#include "memory_pressure/runtime.h"
#include "memory_pressure/serialize.h"
#include "memory_pressure/providers/synthetic.h"
using namespace memory_pressure;
int main() {
    PressureRuntime rt;
    std::vector<SyntheticDomainSpec> ds;
    SyntheticDomainSpec d; d.id = PressureDomainId{0xAA, 0xBB}; d.type = DomainType::AcceleratorMemory; d.group = "gpu"; d.total_capacity = 8ULL*1024*1024*1024; ds.push_back(d);
    rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::GradualGrowth, 100, 4));
    Budget b; b.hard_capacity = 2ULL*1024*1024*1024; rt.set_budget(d.id, b);
    for (int i = 0; i < 5; ++i) rt.refresh(1000 + i*100);
    auto snap = rt.current_snapshot();
    std::string json = json_dump(snapshot_to_json(*snap));
    auto back = snapshot_from_json(json_parse(json).value());
    std::cout << "snapshot round-trip: " << (back ? "OK gen=" + std::to_string(back->generation) : "FAILED") << std::endl;
    return 0;
}
