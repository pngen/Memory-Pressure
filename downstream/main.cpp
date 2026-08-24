#include "memory_pressure/runtime.h"
#include "memory_pressure/version.h"
#include "memory_pressure/providers/windows.h"
#include "memory_pressure/providers/cuda.h"
#include <iostream>
using namespace memory_pressure;
int main() {
    PressureRuntime rt;
    rt.register_provider(std::make_shared<WindowsHostProvider>());
    rt.register_provider(std::make_shared<CudaDeviceProvider>());
    auto snap = rt.refresh(1000);
    std::cout << "Memory Pressure " << version_string() << " via find_package" << std::endl;
    for (const auto& d : snap->domains) {
        if (d.type == DomainType::HostMemory || d.type == DomainType::AcceleratorMemory || d.type == DomainType::SystemCommit)
            std::cout << "  " << to_string(d.type) << " total=" << d.total_capacity
                      << " available=" << d.available << " level=" << to_string(d.level) << std::endl;
    }
    return 0;
}
