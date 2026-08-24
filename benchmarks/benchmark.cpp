#include "memory_pressure/runtime.h"
#include "memory_pressure/serialize.h"
#include "memory_pressure/hysteresis.h"
#include "memory_pressure/score.h"
#include "memory_pressure/providers/synthetic.h"

#include <chrono>
#include <cstdio>

using namespace memory_pressure;

namespace {
std::uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
double per_op_ns(std::uint64_t total_ns, int n) { return static_cast<double>(total_ns) / static_cast<double>(n); }
double per_op_us(std::uint64_t total_ns, int n) { return static_cast<double>(total_ns) / 1000.0 / static_cast<double>(n); }
}

int main() {
    const int scales[] = {1, 8, 64, 1000};
    std::printf("=== Memory Pressure micro-benchmarks ===\n");

    {
        const int N = 100000;
        Thresholds t = default_policy().thresholds;
        ScoreWeights w = default_policy().weights;
        std::uint64_t sum = 0;
        const std::uint64_t t0 = now_ns();
        for (int i = 0; i < N; ++i) {
            PressureLevel lv = evaluate_level(0.5 + 0.5 * (i % 100) / 100.0, PressureLevel::High, t);
            sum += static_cast<std::uint64_t>(lv);
        }
        std::uint64_t t1 = now_ns();
        PressureScore sc = score_domain(0.8, true, 0.2, 0.1, 0.5, 0, Confidence::High, Validity::Valid, w);
        sum += static_cast<std::uint64_t>(sc.value * 10);
        const std::uint64_t tend = now_ns();
        std::printf("threshold+score eval: %8.2f ns/op (sum=%llu)\n", per_op_ns(tend - t0, N), (unsigned long long)sum);
        (void)t1;
    }

    for (const int ndom : scales) {
        PressureRuntime rt;
        std::vector<SyntheticDomainSpec> ds;
        for (int i = 0; i < ndom; ++i) {
            SyntheticDomainSpec d; d.id = PressureDomainId{static_cast<std::uint64_t>(i + 1), 0};
            d.type = DomainType::AcceleratorMemory; d.group = "gpu";
            d.total_capacity = 4ULL*1024*1024*1024;
            if (ndom > 1) d.phase = static_cast<double>(i) / ndom;
            ds.push_back(d);
        }
        rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::GradualGrowth, 100, 4));
        Budget b; b.hard_capacity = 1ULL*1024*1024*1024;
        for (int i = 0; i < ndom; ++i) rt.set_budget(PressureDomainId{static_cast<std::uint64_t>(i + 1), 0}, b);

        for (int i = 0; i < 10; ++i) rt.refresh(1000 + i*100);

        const int N = 200;
        std::shared_ptr<const Snapshot> snap;
        const std::uint64_t t0 = now_ns();
        for (int i = 0; i < N; ++i) snap = rt.refresh(10000 + i*100);
        const std::uint64_t t1 = now_ns();
        std::printf("refresh (%4d domains): %8.1f us/snapshot\n", ndom, per_op_us(t1 - t0, N));

        const int Q = 100000;
        std::uint64_t acc = 0;
        const std::uint64_t q0 = now_ns();
        for (int i = 0; i < Q; ++i) { auto s = rt.current_snapshot(); if (s) acc += s->generation; }
        const std::uint64_t q1 = now_ns();
        std::printf("  current_snapshot:            %8.2f ns/op\n", per_op_ns(q1 - q0, Q));

        const PressureDomainId id{1, 0};
        const DomainState* fd = snap ? snap->find_domain(id) : nullptr;
        const std::uint64_t l0 = now_ns();
        for (int i = 0; i < Q; ++i) fd = snap ? snap->find_domain(id) : nullptr;
        const std::uint64_t l1 = now_ns();
        std::printf("  find_domain:                 %8.2f ns/op (%s)\n", per_op_ns(l1 - l0, Q), fd ? "hit" : "miss");

        AdmissionHint h;
        const std::uint64_t a0 = now_ns();
        for (int i = 0; i < Q; ++i) h = rt.admit(id, 1024);
        const std::uint64_t a1 = now_ns();
        std::printf("  admit:                       %8.2f ns/op (%s)\n", per_op_ns(a1 - a0, Q), to_string(h.decision));

        const std::uint64_t s0 = now_ns();
        std::uint64_t bytes = 0;
        for (int i = 0; i < 50; ++i) { auto j = snapshot_to_json(*snap); auto str = json_serialize(j); if (str) bytes += str->size(); }
        const std::uint64_t s1 = now_ns();
        std::printf("  serialize snapshot:          %8.2f us/op, %llu bytes\n", per_op_us(s1 - s0, 50), (unsigned long long)bytes);

        (void)acc;
    }

    {
        PressureRuntime rt;
        std::vector<SyntheticDomainSpec> ds;
        SyntheticDomainSpec d; d.id = PressureDomainId{999, 1}; d.type = DomainType::AcceleratorMemory; d.group="gpu"; d.total_capacity = 1024ULL*1024*1024; ds.push_back(d);
        rt.register_provider(std::make_shared<SyntheticProvider>(std::move(ds), SyntheticScenario::Sawtooth, 100, 4));
        Budget b; b.hard_capacity = 100ULL*1024*1024; rt.set_budget(d.id, b);
        auto sub = rt.subscribe(SubscriptionFilter{});
        const int R = 500;
        const std::uint64_t e0 = now_ns();
        for (int i = 0; i < R; ++i) rt.refresh(1000 + i*100);
        const std::uint64_t e1 = now_ns();
        PressureEvent ev;
        std::uint64_t count = 0;
        while (sub->try_pop(ev)) ++count;
        std::printf("event dispatch: %8.2f us/refresh (%llu events delivered)\n", per_op_us(e1 - e0, R), (unsigned long long)count);
    }

    std::printf("=== done ===\n");
    return 0;
}
