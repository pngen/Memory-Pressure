#include "memory_pressure/runtime.h"
#include "memory_pressure/serialize.h"
#include "memory_pressure/version.h"
#include "memory_pressure/providers/cuda.h"
#include "memory_pressure/providers/storage.h"
#include "memory_pressure/providers/synthetic.h"
#include "memory_pressure/providers/windows.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

using namespace memory_pressure;

namespace {

std::string human_bytes(std::uint64_t b) {
    const double u = static_cast<double>(b);
    if (u >= 1024.0*1024.0*1024.0) return std::to_string(u/(1024.0*1024.0*1024.0)) + " GiB";
    if (u >= 1024.0*1024.0) return std::to_string(u/(1024.0*1024.0)) + " MiB";
    if (u >= 1024.0) return std::to_string(u/1024.0) + " KiB";
    return std::to_string(b) + " B";
}

void print_usage() {
    std::cout << "memory-pressure - Memory Pressure " << version_string() << std::endl;
    std::cout << "usage: memory-pressure <command> [options]" << std::endl;
    std::cout << "commands:" << std::endl;
    std::cout << "  info                    show version and detected platform/capabilities" << std::endl;
    std::cout << "  observe                 sample providers and print raw observations" << std::endl;
    std::cout << "  domains                 refresh and list pressure domains" << std::endl;
    std::cout << "  status [--json]         refresh and print current pressure status" << std::endl;
    std::cout << "  snapshot [--json]       refresh and print the latest snapshot" << std::endl;
    std::cout << "  watch [--steps N] [--interval-ms M]  bounded periodic refresh display" << std::endl;
    std::cout << "  budget --domain <id> --hard B [--emergency B]  set a governed budget" << std::endl;
    std::cout << "  policy                  show the active policy" << std::endl;
    std::cout << "  pressure                show pressure levels" << std::endl;
    std::cout << "  admit --domain <id> --bytes N  show an admission hint" << std::endl;
    std::cout << "  explain --domain <id>   show a structured explanation" << std::endl;
    std::cout << "  simulate --scenario <name> [--steps N]  run a synthetic scenario" << std::endl;
    std::cout << "  events [--json]         collect and print events from a refresh" << std::endl;
    std::cout << "  stats [--json]          show runtime telemetry" << std::endl;
    std::cout << "  selftest                run built-in self-checks" << std::endl;
    std::cout << "  benchmark               run micro-benchmarks" << std::endl;
}

std::uint64_t parse_len(const char* s) { return static_cast<std::uint64_t>(std::strtoull(s, nullptr, 10)); }

void print_domain_state(const DomainState& d) {
    std::cout << d.id.to_hex() << " " << to_string(d.type) << " level=" << to_string(d.level)
              << " prev=" << to_string(d.previous_level) << " util=" << d.utilization
              << " avail=" << human_bytes(d.available) << " score=" << d.score
              << " trend=" << to_string(d.trend) << " ts=" << d.timestamp_ms << std::endl;
}

// Register the real Windows + CUDA providers (constructed in place; providers
// are non-copyable).
void register_windows_cuda(PressureRuntime& rt) {
    rt.register_provider(std::make_shared<WindowsHostProvider>());
    rt.register_provider(std::make_shared<CudaDeviceProvider>());
}

std::shared_ptr<const Snapshot> refresh_all(PressureRuntime& rt) {
    static std::uint64_t now = 1000000ULL;
    now += 100;
    return rt.refresh(now);
}

} // anonymous namespace

int main(int argc, char** argv) {
    if (argc < 2) { print_usage(); return 0; }
    const std::string cmd = argv[1];

    if (cmd == "info") {
        std::cout << "Memory Pressure " << version_string() << std::endl;
        std::cout << "Platform: Windows (native)" << std::endl;
        WindowsHostProvider host;
        auto h = host.sample(0);
        for (const auto& o : h.observations)
            std::cout << "  " << to_string(o.type) << ": total=" << human_bytes(o.total_capacity)
                      << " committed=" << human_bytes(o.committed) << " available=" << human_bytes(o.available)
                      << " provenance=" << to_string(o.provenance) << std::endl;
        CudaDeviceProvider cuda;
        std::cout << "CUDA: " << (cuda.supported() ? "supported" : "not supported")
                  << " devices=" << cuda.device_count() << std::endl;
        if (cuda.supported()) {
            auto cs = cuda.sample(0);
            for (const auto& o : cs.observations)
                std::cout << "  " << to_string(o.type) << " (" << o.native_resource_id << "): total="
                          << human_bytes(o.total_capacity) << " free=" << human_bytes(o.available)
                          << " provenance=" << to_string(o.provenance) << std::endl;
        }
        return 0;
    }

    if (cmd == "observe") {
        PressureRuntime rt;
        WindowsHostProvider host;
        CudaDeviceProvider cuda;
        StorageProvider storage(".", DomainType::PersistentStorageCapacity);
        rt.register_provider(std::make_shared<WindowsHostProvider>());
        rt.register_provider(std::make_shared<CudaDeviceProvider>());
        rt.register_provider(std::make_shared<StorageProvider>(".", DomainType::PersistentStorageCapacity));
        auto snap = refresh_all(rt);
        for (const auto& d : snap->domains) {
            std::cout << d.id.to_hex() << " " << to_string(d.type) << " provider=" << d.provider
                      << " total=" << human_bytes(d.total_capacity) << " committed=" << human_bytes(d.committed)
                      << " available=" << human_bytes(d.available) << " conf=" << to_string(d.confidence)
                      << " prov=" << to_string(d.provenance) << std::endl;
        }
        return 0;
    }

    if (cmd == "domains") {
        PressureRuntime rt;
        register_windows_cuda(rt);
        auto snap = refresh_all(rt);
        for (const auto& d : snap->domains) print_domain_state(d);
        return 0;
    }

    if (cmd == "status" || cmd == "snapshot") {
        const bool as_json = (argc >= 3 && std::string(argv[2]) == "--json");
        PressureRuntime rt;
        register_windows_cuda(rt);
        rt.register_provider(std::make_shared<StorageProvider>(".", DomainType::PersistentStorageCapacity));
        auto snap = refresh_all(rt);
        if (as_json) { std::cout << json_dump(snapshot_to_json(*snap)) << std::endl; return 0; }
        std::cout << "aggregate=" << to_string(snap->aggregate_level) << " score=" << snap->aggregate_score
                  << " generation=" << snap->generation << " policy=" << snap->policy_name
                  << " domains=" << snap->domains.size() << std::endl;
        for (const auto& d : snap->domains) print_domain_state(d);
        return 0;
    }

    if (cmd == "watch") {
        std::uint64_t steps = 10;
        std::uint64_t interval_ms = 200;
        for (int i = 2; i < argc; ++i) {
            if (std::string(argv[i]) == "--steps") { steps = parse_len(argv[i+1]); ++i; }
            if (std::string(argv[i]) == "--interval-ms") { interval_ms = parse_len(argv[i+1]); ++i; }
        }
        PressureRuntime rt;
        register_windows_cuda(rt);
        for (std::uint64_t i = 0; i < steps; ++i) {
            auto snap = refresh_all(rt);
            std::cout << "[" << i << "] aggregate=" << to_string(snap->aggregate_level)
                      << " gen=" << snap->generation << std::endl;
            for (const auto& d : snap->domains)
                std::cout << "  " << d.id.to_hex() << " " << to_string(d.type) << " util=" << d.utilization
                          << " level=" << to_string(d.level) << std::endl;
            if (interval_ms) std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        }
        return 0;
    }

    if (cmd == "budget") {
        PressureRuntime rt;
        if (argc >= 4 && std::string(argv[2]) == "--domain") {
            auto id = PressureDomainId::from_hex(std::string(argv[3]));
            if (!id) { std::cerr << "invalid domain id" << std::endl; return 1; }
            Budget b;
            b.hard_capacity = 0;
            for (int i = 4; i < argc; ++i) {
                if (std::string(argv[i]) == "--hard") { b.hard_capacity = parse_len(argv[i+1]); ++i; }
                if (std::string(argv[i]) == "--emergency") { b.emergency_reserve_bytes = parse_len(argv[i+1]); ++i; }
            }
            auto err = rt.set_budget(*id, b);
            if (err) { std::cerr << "invalid budget: " << *err << std::endl; return 1; }
            std::cout << "budget set for " << id->to_hex() << " hard=" << human_bytes(b.hard_capacity) << std::endl;
        } else {
            std::cout << "budget command requires --domain <id> --hard <bytes> [--emergency <bytes>]" << std::endl;
        }
        return 0;
    }

    if (cmd == "policy") {
        PressurePolicy p = default_policy();
        std::cout << "policy version=" << p.version << " name=" << p.name << std::endl;
        std::cout << json_dump(policy_to_json(p)) << std::endl;
        return 0;
    }

    if (cmd == "pressure") {
        PressureRuntime rt;
        register_windows_cuda(rt);
        auto snap = refresh_all(rt);
        for (const auto& d : snap->domains) print_domain_state(d);
        return 0;
    }

    if (cmd == "admit") {
        std::string idstr; std::uint64_t bytes = 0;
        for (int i = 2; i < argc; ++i) {
            if (std::string(argv[i]) == "--domain") { idstr = argv[i+1]; ++i; }
            if (std::string(argv[i]) == "--bytes") { bytes = parse_len(argv[i+1]); ++i; }
        }
        auto id = PressureDomainId::from_hex(idstr);
        if (!id) { std::cerr << "invalid domain id" << std::endl; return 1; }
        PressureRuntime rt;
        register_windows_cuda(rt);
        refresh_all(rt);
        auto hint = rt.admit(*id, bytes);
        std::cout << "admit(" << id->to_hex() << ", " << human_bytes(bytes) << ") = " << to_string(hint.decision)
                  << std::endl;
        std::cout << "  " << hint.explanation << std::endl;
        return 0;
    }

    if (cmd == "explain") {
        std::string idstr;
        for (int i = 2; i < argc; ++i) if (std::string(argv[i]) == "--domain") { idstr = argv[i+1]; ++i; }
        auto id = PressureDomainId::from_hex(idstr);
        if (!id) { std::cerr << "invalid domain id" << std::endl; return 1; }
        PressureRuntime rt;
        register_windows_cuda(rt);
        refresh_all(rt);
        std::cout << rt.explain(*id) << std::endl;
        return 0;
    }

    if (cmd == "simulate") {
        std::string scen = "gradual-growth"; std::uint64_t steps = 60;
        for (int i = 2; i < argc; ++i) {
            if (std::string(argv[i]) == "--scenario") { scen = argv[i+1]; ++i; }
            if (std::string(argv[i]) == "--steps") { steps = parse_len(argv[i+1]); ++i; }
        }
        SyntheticScenario sc = SyntheticScenario::GradualGrowth;
        auto psc = [&](const char* n){ return std::string(n) == scen; };
        if (psc("rapid-spike")) sc = SyntheticScenario::RapidSpike;
        if (psc("sawtooth")) sc = SyntheticScenario::Sawtooth;
        if (psc("slow-recovery")) sc = SyntheticScenario::SlowRecovery;
        if (psc("no-recovery")) sc = SyntheticScenario::NoRecovery;
        if (psc("provider-failure")) sc = SyntheticScenario::ProviderFailure;
        if (psc("stale-provider")) sc = SyntheticScenario::StaleProvider;
        if (psc("gpu-only")) sc = SyntheticScenario::GpuOnly;
        if (psc("host-only")) sc = SyntheticScenario::HostOnly;
        if (psc("simultaneous")) sc = SyntheticScenario::Simultaneous;
        if (psc("persistent-tier")) sc = SyntheticScenario::PersistentTier;
        if (psc("pinned-exhaustion")) sc = SyntheticScenario::PinnedExhaustion;
        if (psc("multi-gpu")) sc = SyntheticScenario::MultiGpu;
        if (psc("one-recovering")) sc = SyntheticScenario::OneRecovering;

        PressureRuntime rt;
        std::vector<SyntheticDomainSpec> ds;
        SyntheticDomainSpec gpu0; gpu0.id = PressureDomainId{1, 1}; gpu0.type = DomainType::AcceleratorMemory; gpu0.group = "gpu"; gpu0.provider = "synthetic"; gpu0.total_capacity = 32ULL*1024*1024*1024; ds.push_back(gpu0);
        SyntheticDomainSpec h; h.id = PressureDomainId{1, 2}; h.type = DomainType::HostMemory; h.group = "host"; h.provider = "synthetic"; h.total_capacity = 64ULL*1024*1024*1024; ds.push_back(h);
        rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), sc, 100, 4));
        Budget gb; gb.hard_capacity = 8ULL*1024*1024*1024; gb.emergency_reserve_bytes = 1ULL*1024*1024*1024; rt.set_budget(gpu0.id, gb);
        Budget hb; hb.hard_capacity = 16ULL*1024*1024*1024; rt.set_budget(h.id, hb);

        std::uint64_t now = 1000ULL;
        std::string last_level;
        for (std::uint64_t i = 0; i < steps; ++i) {
            auto snap = rt.refresh(now + i*100);
            const DomainState* d = snap->find_domain(gpu0.id);
            const std::string lvl = d ? to_string(d->level) : "none";
            std::cout << "step=" << i << " gen=" << snap->generation << " gpu=" << lvl
                      << " util=" << (d?d->utilization:0.0) << " score=" << (d?d->score:0.0);
            if (!last_level.empty() && lvl != last_level) std::cout << "  [transition]";
            std::cout << std::endl;
            last_level = lvl;
        }
        return 0;
    }

    if (cmd == "events") {
        PressureRuntime rt;
        register_windows_cuda(rt);
        SubscriptionFilter f;
        auto sub = rt.subscribe(f);
        refresh_all(rt);
        PressureEvent e; int n = 0;
        while (sub->try_pop(e)) {
            if (argc >= 3 && std::string(argv[2]) == "--json") std::cout << json_dump(event_to_json(e)) << std::endl;
            else std::cout << to_string(e.type) << " gen=" << e.generation << " domain="
                           << (e.domain?e.domain->to_hex():"") << " detail=" << e.detail << std::endl;
            ++n;
        }
        std::cout << "events=" << n << std::endl;
        return 0;
    }

    if (cmd == "stats") {
        PressureRuntime rt;
        register_windows_cuda(rt);
        for (int i = 0; i < 3; ++i) refresh_all(rt);
        rt.admit(PressureDomainId{1,1}, 1024);
        bool as_json = (argc >= 3 && std::string(argv[2]) == "--json");
        if (as_json) std::cout << json_dump(stats_to_json(rt.stats())) << std::endl;
        else std::cout << "snapshots=" << rt.stats().snapshots << " transitions=" << rt.stats().pressure_transitions
                       << " refresh_latency_us=" << rt.stats().refresh_latency_us << std::endl;
        return 0;
    }

    if (cmd == "selftest") {
        PressurePolicy p = default_policy();
        if (validate_policy(p)) { std::cerr << "selftest failed: default policy invalid" << std::endl; return 1; }
        PressureRuntime rt;
        std::vector<SyntheticDomainSpec> ds;
        SyntheticDomainSpec d0; d0.id = PressureDomainId{9,9}; d0.type = DomainType::AcceleratorMemory; d0.group="gpu"; d0.total_capacity = 8ULL*1024*1024*1024; ds.push_back(d0);
        rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::GradualGrowth, 100, 4));
        Budget b; b.hard_capacity = 4ULL*1024*1024*1024; b.emergency_reserve_bytes = 512ULL*1024*1024; rt.set_budget(d0.id, b);
        for (int i = 0; i < 20; ++i) rt.refresh(1000ULL + i*100);
        auto snap = rt.current_snapshot();
        bool ok = snap && snap->generation > 0;
        std::cout << "selftest " << (ok ? "PASS" : "FAIL") << " gen=" << (snap?snap->generation:0)
                  << " domains=" << (snap?snap->domains.size():0) << std::endl;
        return ok ? 0 : 1;
    }

    if (cmd == "benchmark") {
        PressureRuntime rt;
        std::vector<SyntheticDomainSpec> ds;
        for (int i = 0; i < 64; ++i) {
            SyntheticDomainSpec d; d.id = PressureDomainId{static_cast<std::uint64_t>(i+1), 0};
            d.type = DomainType::AcceleratorMemory; d.group="gpu"; d.total_capacity=8ULL*1024*1024*1024;
            d.phase = static_cast<double>(i)/64.0; ds.push_back(d);
        }
        rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::GradualGrowth, 100, 4));
        for (int i = 0; i < 5; ++i) rt.refresh(1000ULL+i*100);
        const int N = 200;
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < N; ++i) rt.refresh(10000ULL + i*100);
        auto t1 = std::chrono::steady_clock::now();
        double us = std::chrono::duration_cast<std::chrono::microseconds>(t1-t0).count() / double(N);
        std::cout << "refresh (64 domains): " << us << " us/snapshot" << std::endl;
        auto h = rt.admit(PressureDomainId{1,0}, 1024);
        (void)h;
        return 0;
    }

    std::cerr << "unknown command: " << cmd << std::endl;
    print_usage();
    return 1;
}
