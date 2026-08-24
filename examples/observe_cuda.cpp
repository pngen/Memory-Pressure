#include <iostream>
#include "memory_pressure/providers/cuda.h"
using namespace memory_pressure;
int main() {
    CudaDeviceProvider p;
    if (!p.supported()) { std::cout << "CUDA not available" << std::endl; return 0; }
    std::cout << "CUDA device count: " << p.device_count() << std::endl;
    ProviderSample s = p.sample(1000);
    for (const auto& o : s.observations)
        std::cout << "  " << to_string(o.type) << " id=" << o.id.to_hex()
                  << " total=" << o.total_capacity << " free=" << o.available
                  << " provenance=" << to_string(o.provenance) << std::endl;
    return 0;
}
