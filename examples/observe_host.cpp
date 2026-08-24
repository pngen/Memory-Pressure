#include <iostream>
#include "memory_pressure/providers/windows.h"
using namespace memory_pressure;
int main() {
    WindowsHostProvider p;
    ProviderSample s = p.sample(1000);
    std::cout << "Windows host observations:" << std::endl;
    for (const auto& o : s.observations)
        std::cout << "  " << to_string(o.type) << " total=" << o.total_capacity
                  << " committed=" << o.committed << " available=" << o.available
                  << " provenance=" << to_string(o.provenance) << std::endl;
    return 0;
}
