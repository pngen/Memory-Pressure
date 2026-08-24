#include "test_harness.h"
#include "memory_pressure/runtime.h"
#include "memory_pressure/serialize.h"
#include "memory_pressure/trace.h"
#include "memory_pressure/domain.h"

using namespace memory_pressure;

TEST(observation_json_roundtrip) {
    DomainObservation o;
    o.id = PressureDomainId{0xAA, 0xBB};
    o.type = DomainType::AcceleratorMemory;
    o.provider = "cuda.driver";
    o.native_resource_id = "uuid|name";
    o.total_capacity = 100;
    o.committed = 40;
    o.resident = 40;
    o.reclaimable = 20;
    o.available = 60;
    o.usable_capacity = 100;
    o.reserved = 10;
    o.confidence = Confidence::High;
    o.provenance = Provenance::CudaDriverApi;
    o.validity = Validity::Valid;
    o.observed_at_ms = 12345;
    o.allocation_failures = 3;
    o.growth_bytes_per_s = 1000.5;
    o.detail = "test";
    Json j = observation_to_json(o);
    auto back = observation_from_json(j);
    EXPECT(back.has_value());
    if (back) {
        EXPECT(back->id == o.id);
        EXPECT(back->provider == "cuda.driver");
        EXPECT(back->committed == 40);
        EXPECT(back->reclaimable == 20);
        EXPECT(back->confidence == Confidence::High);
        EXPECT(back->allocation_failures == 3);
        EXPECT(back->growth_bytes_per_s == 1000.5);
    }
    // Missing id is rejected.
    Json noid = Json::object();
    noid["type"] = Json("HOST_MEMORY");
    EXPECT(!observation_from_json(noid).has_value());
}

TEST(trace_replay_is_deterministic) {
    // Build a deterministic trace: committed grows on each frame.
    auto trace = std::make_shared<PressureTrace>();
    trace->policy_version = 1;
    for (int i = 0; i < 40; ++i) {
        TraceFrame f;
        f.timestamp_ms = 1000ULL + i * 100;
        DomainObservation o;
        o.id = PressureDomainId{0x11, 0x22};
        o.type = DomainType::AcceleratorMemory;
        o.provider = "trace";
        o.total_capacity = 8ULL * 1024 * 1024 * 1024;
        o.committed = static_cast<std::uint64_t>(static_cast<double>(o.total_capacity) *
                           (0.1 + 0.85 * std::min(1.0, static_cast<double>(i) / 40.0)));
        o.available = o.committed < o.total_capacity ? o.total_capacity - o.committed : 0;
        o.confidence = Confidence::High;
        o.provenance = Provenance::ImportedSnapshot;
        o.validity = Validity::Valid;
        o.observed_at_ms = f.timestamp_ms;
        f.observations.push_back(o);
        trace->frames.push_back(f);
    }

    auto replay = [&]() {
        PressureRuntime rt;
        rt.register_provider(std::make_shared<TraceProvider>(trace));
        Budget b; b.hard_capacity = 4ULL * 1024 * 1024 * 1024; rt.set_budget(PressureDomainId{0x11, 0x22}, b);
        std::vector<std::string> levels;
        for (std::size_t i = 0; i < trace->frames.size(); ++i) {
            auto sn = rt.refresh(trace->frames[i].timestamp_ms);
            const DomainState* d = sn->find_domain(PressureDomainId{0x11, 0x22});
            levels.push_back(d ? std::string(to_string(d->level)) : "none");
        }
        return levels;
    };
    auto a = replay();
    auto b = replay();
    EXPECT(a == b);   // identical trace => identical result
    EXPECT(a.size() == 40);
}

TEST(trace_json_roundtrip_preserves_frames) {
    PressureTrace t;
    t.policy_version = 3;
    TraceFrame f; f.timestamp_ms = 999;
    DomainObservation o; o.id = PressureDomainId{1,2}; o.type = DomainType::HostMemory; o.total_capacity=50; o.committed=25; f.observations.push_back(o);
    t.frames.push_back(f);
    Json j = trace_to_json(t);
    auto back = trace_from_json(j);
    EXPECT(back.has_value());
    if (back) {
        EXPECT(back->policy_version == 3);
        EXPECT(back->frames.size() == 1);
        if (!back->frames.empty()) EXPECT(back->frames[0].observations[0].total_capacity == 50);
    }
    Json bad = j; bad["schema"] = Json(static_cast<std::int64_t>(42));
    EXPECT(!trace_from_json(bad).has_value());
}

TEST(json_rejects_nan_and_oversize) {
    // Non-finite numbers are rejected during parsing.
    auto parsed = json_parse("{\"a\": 1e999}");
    EXPECT(!parsed.has_value());
    // Oversized input is rejected by the byte limit.
    std::string big = "[";
    for (int i = 0; i < 100000; ++i) big += "1,";
    big += "1]";
    JsonParseLimits lim; lim.max_bytes = 1000;
    EXPECT(!json_parse(big, lim).has_value());
    // Trailing garbage rejected.
    EXPECT(!json_parse("{} x").has_value());
    // Unescaped control char rejected.
    {
        std::string ctrl;
        ctrl.push_back('"'); ctrl.push_back('a'); ctrl.push_back(static_cast<char>(0x01)); ctrl.push_back('"');
        EXPECT(!json_parse(ctrl).has_value());
    }
    // Duplicate keys are fine (object), but malformed snippets rejected.
    EXPECT(json_parse("{\"a\":1,\"b\":2}").has_value());
}

TEST(json_serialize_string_escaping) {
    Json s = Json("a \"b\" \\ c\n");
    auto out = json_serialize(s);
    EXPECT(out.has_value());
    if (out) {
        auto back = json_parse(*out);
        EXPECT(back.has_value());
        if (back) EXPECT(back->as_string() == std::string("a \"b\" \\ c\n"));
    }
}
