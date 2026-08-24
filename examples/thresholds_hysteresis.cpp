#include <iostream>
#include "memory_pressure/hysteresis.h"
#include "memory_pressure/policy.h"
using namespace memory_pressure;
int main() {
    PressurePolicy pol = default_policy();
    HysteresisState hs(pol.hysteresis);
    const double usages[] = {0.10, 0.88, 0.88, 0.81, 0.81, 0.81, 0.70, 0.70, 0.55, 0.55};
    std::cout << "usage -> level (with hysteresis):" << std::endl;
    for (int i = 0; i < 10; ++i) {
        PressureLevel raw = evaluate_level(usages[i], hs.current(), pol.thresholds);
        PressureLevel lv = hs.update(raw, 100 + i);
        std::cout << "  " << usages[i] << " -> " << to_string(lv) << " (raw " << to_string(raw) << ")" << std::endl;
    }
    return 0;
}
