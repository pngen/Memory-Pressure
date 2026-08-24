#include <iostream>
#include "memory_pressure/runtime.h"
#include "memory_pressure/providers/cuda.h"
#include "memory_pressure/providers/windows.h"
using namespace memory_pressure;
// A downstream runtime consuming pressure facts before placing work.
int main() {
    PressureRuntime rt;
    rt.register_provider(std::make_shared<WindowsHostProvider>());
    rt.register_provider(std::make_shared<CudaDeviceProvider>());
    auto snap = rt.refresh(1000);
    std::cout << "aggregate=" << to_string(snap->aggregate_level) << " score=" << snap->aggregate_score << std::endl;
    for (const auto& d : snap->domains) {
        if (d.type != DomainType::AcceleratorMemory) continue;
        // A consumer decides admission based on the pressure facts.
        AdmissionHint h = rt.admit(d.id, 256ULL*1024*1024);
        std::cout << "domain " << d.id.to_hex() << " level=" << to_string(d.level)
                  << " admit=" << to_string(h.decision) << " avail=" << d.available << std::endl;
    }
    return 0;
}
