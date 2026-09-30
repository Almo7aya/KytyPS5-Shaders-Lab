#include "shader_lab/compiler_trace.hpp"
#include <iostream>

using namespace sl;

int pass_fixture_worker(const json &request) {
    const auto &profile = request.at("profile");
    auto mode = profile.value("test_mode", "");
    if (mode.find("pass_bisect") != 0)
        return -1;
    const auto out = path_from(request.at("output").get<std::string>());
    const auto catalog = compiler_pass_catalog();
    const auto diagnostics = profile.value("diagnostics", json::object());
    const int stop = diagnostics.value("stop_after_pass", -1);
    const bool traced = diagnostics.value("pass_trace", false);
    const unsigned failure = profile.value("fail_at", 7u);
    atomic_json(out / "phase.json", {{"phase", "translate"}});
    json trace = {{"schema", 1},
                  {"catalog_schema", 1},
                  {"id", request.at("id")},
                  {"code_sha256", hash_file(path_from(request.at("code").get<std::string>()))},
                  {"events", json::array()}};
    auto event = [&](unsigned index, const char *state) {
        trace["events"].push_back({{"index", index},
                                   {"name", catalog.at("passes").at(index).at("name")},
                                   {"state", state}});
        trace["last"] = trace["events"].back();
        if (mode == "pass_bisect_bad_trace")
            trace["code_sha256"] = std::string(64, '0');
        if (traced && mode != "pass_bisect_no_trace")
            atomic_json(out / "pass-trace.json", trace);
    };
    for (unsigned i = 0; i < catalog.at("passes").size(); ++i) {
        event(i, "started");
        if (i == failure)
            break;
        event(i, "completed");
        if (stop == int(i)) {
            atomic_json(out / "response.json", {{"schema", 1},
                                                {"id", request.at("id")},
                                                {"status", "pass_checkpoint_reached"},
                                                {"pass_checkpoint", catalog.at("passes").at(i)}});
            return 0;
        }
    }
    std::cout << "--- Error ---\n"
              << (mode == "pass_bisect_changed" && stop >= 0 ? "different assertion"
                                                             : "fixture pass assertion")
              << "\n";
    return 17;
}

unsigned pass_tests(const fs::path &root, Bytes input_header, const fs::path &worker) {
    unsigned checks = 0;
    auto test = [&](bool ok, const char *name) {
        if (!ok)
            throw std::runtime_error(name);
        ++checks;
    };
    auto rejects = [&](auto action, const char *name) {
        bool rejected = false;
        try {
            action();
        } catch (const std::exception &) {
            rejected = true;
        }
        test(rejected, name);
    };
    const std::vector<uint8_t> code = {0, 0, 0x81, 0xbf};
    write_bytes(root / "input/fixture.header", input_header);
    write_bytes(root / "input/fixture.code", code);
    auto manifest = scan({root / "input", root / "dataset"});
    const auto id = manifest.at("shaders").begin().key();
    auto make_bundle = [&](const char *mode, unsigned at, const std::string &name) {
        auto source = root / name;
        atomic_json(source / "profile.json", {{"test_mode", mode}, {"fail_at", at}});
        run({root / "dataset", source / "run", worker, source / "profile.json", 1, 5000, 0, false});
        export_repro(root / "dataset", source / "run/results.json", id, source / "bundle");
        return source / "bundle";
    };
    for (unsigned at : {0u, 1u, 7u, 20u, 21u}) {
        auto name = "failure-" + std::to_string(at);
        auto bundle = make_bundle("pass_bisect", at, name);
        auto result = bisect_passes({bundle, worker, root / (name + "-bisect"), 0, 2});
        test(result.at("boundary_verified") == true, "repeated fixture pass boundary verified");
        test(result.at("state") == (at == 21 ? "after_last_checkpoint" : "localized"),
             "pass search distinguishes failures beyond the instrumented passes");
        test(at == 21 ? result.at("first_unreachable").is_null()
                      : result.at("first_unreachable").at("index") == at,
             "binary search identifies the actual failing prefix");
        test(at == 0 ? result.at("last_completed").is_null()
                     : result.at("last_completed").at("index") == at - 1,
             "last completed boundary is adjacent to failure");
        test(result.at("attempts").size() <= 21, "pass bisection has a bounded probe count");
        for (const auto &attempt : result.at("attempts")) {
            auto results = read_json(root / (name + "-bisect") /
                                     path_from(attempt.at("results").get<std::string>()));
            test(results.at("results").at(id).at("cache_hit") == false,
                 "all pass observations use fresh workers");
        }
        rejects([&] { bisect_passes({bundle, worker, root / (name + "-bisect"), 0, 2}); },
                "pass bisection preserves existing output");
    }
    auto changed = make_bundle("pass_bisect_changed", 7, "changed");
    auto different = bisect_passes({changed, worker, root / "changed-bisect", 0, 2});
    test(different.at("state") == "inconclusive" && different.at("boundary_verified") == false,
         "same exit code with a different assertion cannot establish a boundary");
    auto missing = make_bundle("pass_bisect_no_trace", 7, "missing");
    test(bisect_passes({missing, worker, root / "missing-bisect", 0, 2}).at("state") ==
             "instrumented_baseline_not_reproduced",
         "missing trace cannot prove pass reachability");
    auto malformed = make_bundle("pass_bisect_bad_trace", 7, "malformed");
    rejects([&] { bisect_passes({malformed, worker, root / "malformed-bisect", 0, 2}); },
            "trace for another shader is rejected");
    test(read_json(root / "malformed-bisect/pass-bisection.json").at("state") == "error",
         "invalid trace retains a diagnostic checkpoint");
    rejects([&] { bisect_passes({changed, worker, root / "bad-count", 0, 1}); },
            "pass bisection requires repeated confirmation");
    rejects([&] { bisect_passes({changed, worker, changed / "overlap", 0, 2}); },
            "pass bisection output cannot modify its bundle");
    auto descriptor = read_json(changed / "repro.json");
    descriptor["worker_sha256"] = std::string(64, '0');
    atomic_json(changed / "repro.json", descriptor);
    rejects([&] { bisect_passes({changed, worker, root / "wrong-worker", 0, 2}); },
            "changing the compiler cannot masquerade as pass bisection");
    return checks;
}

unsigned pass_real_tests(const fs::path &root, Bytes input_header, const fs::path &worker) {
    unsigned checks = 0;
    auto test = [&](bool ok, const char *name) {
        if (!ok)
            throw std::runtime_error(name);
        ++checks;
    };
    std::vector<uint8_t> header(input_header.begin(), input_header.end());
    const std::vector<uint8_t> end = {0, 0, 0x81, 0xbf};
    write_bytes(root / "input/end.header", header);
    write_bytes(root / "input/end.code", end);
    auto manifest = scan({root / "input", root / "dataset"});
    const auto id = manifest.at("shaders").begin().key();
    auto profile_path = root / "profile.json";
    json profile = {{"schema", 1}, {"stage", "CS"}, {"mode", "header_probe"}};
    atomic_json(profile_path, profile);
    auto normal =
        run({root / "dataset", root / "normal", worker, profile_path, 1, 10000, 0, false});
    profile["diagnostics"] = {{"pass_trace", true}};
    atomic_json(profile_path, profile);
    auto traced =
        run({root / "dataset", root / "traced", worker, profile_path, 1, 10000, 0, false});
    test(normal.at("results").at(id).at("details").at("spirv_sha256") ==
             traced.at("results").at(id).at("details").at("spirv_sha256"),
         "pass tracing preserves emitted SPIR-V on the real compiler fixture");
    const auto catalog = compiler_pass_catalog();
    for (unsigned i = 0; i < catalog.at("passes").size(); ++i) {
        profile["diagnostics"]["stop_after_pass"] = i;
        atomic_json(profile_path, profile);
        auto output = root / ("stop-" + std::to_string(i));
        auto stopped = run({root / "dataset", output, worker, profile_path, 1, 10000, 0, false});
        const auto &row = stopped.at("results").at(id);
        test(row.at("status") == "pass_checkpoint_reached" &&
                 row.at("details").at("pass_checkpoint") == catalog.at("passes").at(i),
             "real upstream prefix stops at the requested pass, not at a synthetic compiler");
        auto artifacts = output / path_from(row.at("artifacts").get<std::string>());
        auto trace = read_json(artifacts / "pass-trace.json");
        test(trace.at("events").size() == (i + 1) * 2,
             "real pass trace has every ordered entry/exit");
        test(!row.at("details").contains("spirv_sha256"), "prefix stop is not a validation pass");
        if (i)
            test(hash_file(artifacts / "pass-stop.ir") ==
                     trace.at("stop_ir_sha256").get<std::string>(),
                 "selected checkpoint retains content-identified upstream IR");
    }
    profile["diagnostics"]["stop_after_pass"] = catalog.at("passes").size();
    atomic_json(profile_path, profile);
    test(run({root / "dataset", root / "bad-index", worker, profile_path, 1, 10000, 0, false})
                 .at("results")
                 .at(id)
                 .at("status") == "adapter_error",
         "unknown real pass index is rejected");

    // RDNA2 SOP1 S_GETPC_B64 with VCC_HI destination: a recognized instruction,
    // but not a valid scalar pair. Official Control.cpp deliberately asserts.
    header[0x44] = 8;
    const std::vector<uint8_t> invalid_pair = {0, 0x1f, 0xeb, 0xbe, 0, 0, 0x81, 0xbf};
    write_bytes(root / "assert-input/pair.header", header);
    write_bytes(root / "assert-input/pair.code", invalid_pair);
    auto assertion_manifest = scan({root / "assert-input", root / "assert-dataset"});
    auto assertion_id = assertion_manifest.at("shaders").begin().key();
    profile.erase("diagnostics");
    atomic_json(profile_path, profile);
    auto assertion = run(
        {root / "assert-dataset", root / "assert-run", worker, profile_path, 1, 10000, 0, false});
    auto signature = failure_signature(assertion.at("results").at(assertion_id));
    test(!signature.is_null() && signature.at("status") == "worker_crash_or_error",
         "real invalid scalar pair produces a detailed upstream assertion");
    export_repro(root / "assert-dataset", root / "assert-run/results.json", assertion_id,
                 root / "assert-bundle");
    auto result = bisect_passes({root / "assert-bundle", worker, root / "assert-bisect", 0, 2});
    test(result.at("state") == "localized" && result.at("boundary_verified") == true &&
             result.at("last_completed").at("index") == 0 &&
             result.at("first_unreachable").at("index") == 1,
         "bisection localizes genuine upstream assertion to frontend translation");
    return checks;
}
