#include <iostream>
#include <memory>
#include <algorithm>
#include "memory_pressure/runtime.h"
#include "memory_pressure/trace.h"
#include "memory_pressure/domain.h"
using namespace memory_pressure;
int main() {
    auto trace = std::make_shared<PressureTrace>();
    trace->policy_version = 1;
    for (int i = 0; i < 40; ++i) {
        TraceFrame f; f.timestamp_ms = 1000ULL + i*100;
        DomainObservation o; o.id = PressureDomainId{0x55, 0x66};
        o.type = DomainType::AcceleratorMemory; o.provider = "trace";
        o.total_capacity = 4ULL*1024*1024*1024;
        o.committed = static_cast<std::uint64_t>(static_cast<double>(o.total_capacity) * (0.1 + 0.8 * std::min(1.0, static_cast<double>(i)/40.0)));
        o.available = o.committed < o.total_capacity ? o.total_capacity - o.committed : 0;
        o.confidence = Confidence::High; o.provenance = Provenance::ImportedSnapshot; o.validity = Validity::Valid;
        f.observations.push_back(o);
        trace->frames.push_back(f);
    }
    PressureRuntime rt;
    rt.register_provider(std::make_shared<TraceProvider>(trace));
    Budget b; b.hard_capacity = 1ULL*1024*1024*1024; rt.set_budget(PressureDomainId{0x55, 0x66}, b);
    std::cout << "trace replay:" << std::endl;
    for (std::size_t i = 0; i < trace->frames.size(); ++i) {
        auto sn = rt.refresh(trace->frames[i].timestamp_ms);
        const DomainState* st = sn->find_domain(PressureDomainId{0x55, 0x66});
        if (st && i % 5 == 0) std::cout << "  frame " << i << " " << to_string(st->level) << std::endl;
    }
    return 0;
}
