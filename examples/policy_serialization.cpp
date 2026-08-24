#include <iostream>
#include "memory_pressure/policy.h"
#include "memory_pressure/serialize.h"
using namespace memory_pressure;
int main() {
    PressurePolicy p = default_policy();
    p.version = 3; p.name = "example";
    std::string json = json_dump(policy_to_json(p));
    std::cout << "policy json: " << json << std::endl;
    auto back = policy_from_json(json_parse(json).value());
    if (back) std::cout << "round-trip version=" << back->version << " name=" << back->name << std::endl;
    else std::cout << "round-trip failed" << std::endl;
    return 0;
}
