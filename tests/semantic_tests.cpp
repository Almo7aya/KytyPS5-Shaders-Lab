#include "shader_lab/lab.hpp"
using namespace sl;

unsigned semantic_tests(const fs::path &root, Bytes header, const fs::path &worker) {
    unsigned checks = 0;
    auto test = [&](bool ok, const char *why) {
        if (!ok)
            throw std::runtime_error(why);
        ++checks;
    };
    const auto input = root / "i", evidence = input / ".shader-lab", dir = evidence / "f";
    const std::vector<uint8_t> code{0, 0, 0x81, 0xbf}, initial{0, 1}, expected{1, 2};
    write_bytes(dir / "header.bin", header);
    write_bytes(dir / "code.bin", code);
    write_bytes(dir / "initial.bin", initial);
    write_bytes(dir / "expected.bin", expected);
    const auto id = sha256(header) + "-" + sha256(code);
    json manifest = {
        {"shaders",
         {{id, {{"type", "CS"}, {"header_sha256", sha256(header)}, {"code_sha256", sha256(code)}}},
          {"graphics", {{"type", "PS"}}},
          {"missing", {{"type", "CS"}}}}}};
    auto file = [&](const char *name) {
        return json{{"file", name}, {"sha256", hash_file(dir / name)}};
    };
    auto fixture = [&](const std::string &mode) {
        atomic_json(dir / "profile.json", {{"test_execution", mode}});
        auto resource = file("initial.bin");
        resource.update({{"kind", "buffer"},
                         {"type", "u8"},
                         {"access", "read_write"},
                         {"binding", {0, 0}},
                         {"guest_address", "0000000000001000"}});
        atomic_json(dir / "fixture.json",
                    {{"schema", 1},
                     {"kind", "shader_lab_execution_fixture"},
                     {"id", "semantic-protocol-only"},
                     {"shader", {{"header", file("header.bin")}, {"code", file("code.bin")}}},
                     {"profile", file("profile.json")},
                     {"execution",
                      {{"stage", "CS"},
                       {"wave_size", 32},
                       {"exec_mask", "00000000ffffffff"},
                       {"workgroup_size", {32, 1, 1}},
                       {"dispatch_size", {1, 1, 1}}}},
                     {"resources", {{"result", resource}}}});
        auto output = file("expected.bin");
        output.update({{"kind", "buffer"}, {"type", "u8"}, {"comparison", {{"mode", "exact"}}}});
        atomic_json(
            dir / "reference.json",
            {{"schema", 1},
             {"fixture", execution_fixture_identity(dir / "fixture.json")},
             {"reference_source",
              {{"kind", "independent_model"},
               {"identifier", "protocol-double"},
               {"method", "Protocol test constants only, not shader correctness evidence"}}},
             {"outputs", {{"result", output}}}});
    };
    const json entry = {
        {"case_id", id}, {"fixture", "f/fixture.json"}, {"reference", "f/reference.json"}};
    auto plan = [&](json tests) {
        atomic_json(evidence / "semantics.json", {{"schema", 1}, {"tests", tests}});
    };
    auto assess = [&](SemanticOptions options, const fs::path &backend) {
        return validate_semantics(input, root / "o", manifest, backend, options);
    };
    auto result = assess({}, worker);
    test(result["cases"][id]["status"] == "not_requested", "disabled semantics do not execute");
    result = assess({true, true}, worker);
    test(result["cases"][id]["status"] == "missing_evidence" &&
             result["cases"]["graphics"]["status"] == "unsupported",
         "missing evidence and graphics explicit");
    test(!fs::exists(root / "o"), "no attempts without fixtures");
    fixture("match");
    plan(json::array({entry}));
    result = assess({true, false}, worker);
    test(result["cases"][id]["status"] == "gpu_not_allowed" && !fs::exists(root / "o"),
         "semantic-only validates evidence without starting GPU worker");
    result = assess({true, true}, root / "missing-backend");
    test(result["cases"][id]["status"] == "backend_unavailable", "missing backend never passes");
    for (const auto &mode : {"match", "mismatch", "unsupported", "fail", "wrong-hash"}) {
        fixture(mode);
        result = assess({true, true}, worker);
        const std::string wanted = std::string(mode) == "match"        ? "matched_test_inputs"
                                   : std::string(mode) == "fail"       ? "backend_error"
                                   : std::string(mode) == "wrong-hash" ? "execution_error"
                                                                       : mode;
        test(result["cases"][id]["status"] == wanted, "semantic execution outcome preserved");
        test(result["cases"][id]["semantic_correctness"] == "not_proven",
             "no result certifies full correctness");
    }
    fixture("match");
    auto bad = entry;
    bad["fixture"] = "../../escape.json";
    plan(json::array({entry, bad}));
    result = assess({true, true}, worker);
    test(result["cases"][id]["status"] == "invalid_evidence" &&
             result["cases"][id]["tests"].size() == 2,
         "mixed success and invalid evidence cannot report aggregate match");
    bad = entry;
    bad["case_id"] = "absent";
    plan(json::array({bad}));
    result = assess({true, true}, worker);
    test(result["issues"].size() == 1 && result["cases"][id]["status"] == "missing_evidence",
         "unmatched index entries do not attach to unrelated shaders");
    plan(json::array({entry}));
    manifest["shaders"][id]["code_sha256"] = std::string(64, '0');
    result = assess({true, true}, worker);
    test(result["cases"][id]["status"] == "invalid_evidence", "shader hash mismatch blocks replay");
    write_text(evidence / "semantics.json", "{");
    result = assess({true, true}, worker);
    test(!result["issues"].empty() && result["cases"][id]["status"] == "invalid_evidence",
         "bad index is reported, not silently ignored");

    // Real independent CPU generation; the replay worker below is still a protocol double.
    const auto auto_input = root / "a", dataset = root / "d", auto_output = root / "r";
    fs::create_directories(auto_input);
    auto automatic = [&](std::initializer_list<uint32_t> words, SemanticOptions options) {
        std::vector<uint8_t> binary;
        for (const auto word : words)
            for (unsigned b = 0; b < 4; ++b)
                binary.push_back(uint8_t(word >> (8 * b)));
        std::vector<uint8_t> h(header.begin(), header.end());
        for (unsigned b = 0; b < 4; ++b)
            h[0x44 + b] = uint8_t(binary.size() >> (8 * b));
        const auto key = sha256(h) + "-" + sha256(binary);
        write_bytes(dataset / "objects" / key / "header.bin", h);
        write_bytes(dataset / "objects" / key / "code.bin", binary);
        json catalog = {{"shaders",
                         {{key,
                           {{"type", "CS"},
                            {"code_bytes", binary.size()},
                            {"header_sha256", sha256(h)},
                            {"code_sha256", sha256(binary)}}}}}};
        return validate_semantics(auto_input, auto_output, catalog, worker, options, dataset);
    };
    const std::initializer_list<uint32_t> store{0x7e020287u, 0xe0702000u, 0x80000100u, 0xbf810000u};
    result = automatic(store, {true, false});
    auto generated = result["cases"].begin().value();
    test(generated.at("status") == "gpu_not_allowed" && generated.at("tests").size() == 6,
         "CPU automatically creates six wave/pattern references without GPU opt-in");
    test(!fs::exists(auto_input / ".shader-lab"), "automatic mode never writes the game input");
    for (const auto &attempt : generated.at("tests")) {
        const auto path =
            auto_output / path_from(attempt.at("generated_fixture").get<std::string>());
        const auto expected = read_bytes(path / "expected-buffer0.bin");
        const auto wave = attempt.at("fixture").at("wave_size").get<unsigned>();
        test(integer(expected, 0, 4) == 7 && integer(expected, (wave - 1) * 4, 4) == 7,
             "generated CPU output equals independently specified store constant");
        const auto trace = read_json(path / "model-trace.json");
        test(trace.at("buffer_stores") == wave && trace.at("host_gpu_used") == false,
             "real CPU model executed the original instructions");
    }
    const auto old_directory = result.at("generated_directory");
    const auto repeat = automatic(store, {true, false});
    test(repeat.at("generated_directory") != old_directory &&
             repeat.at("cases").begin().value().at("tests")[0].at("reference_record_sha256") ==
                 generated.at("tests")[0].at("reference_record_sha256"),
         "generated inputs/reference identities deterministic; attempts retained separately");
    result = automatic(store, {true, true});
    test(result.at("cases").begin().value().at("status") == "mismatch",
         "wrong replay output is detected against independently generated reference");
    result = automatic({0xbf810000u}, {true, false});
    test(result.at("cases").begin().value().at("status") == "unsupported" &&
             result.at("cases").begin().value().at("reason").get<std::string>().find(
                 "observable") != std::string::npos,
         "no-output shader never gets a vacuous semantic pass");
    result = automatic({0xffffffffu, 0xbf810000u}, {true, false});
    test(result.at("cases").begin().value().at("status") == "unsupported",
         "unmodeled instruction explicitly unsupported");
    result = automatic({0x7e020287u, 0xe0702000u, 0x80000104u, 0xbf810000u}, {true, false});
    test(result.at("cases").begin().value().at("status") == "unsupported" &&
             result.at("cases").begin().value().at("tests").size() == 6,
         "undefined synthetic registers reject all six variants without inventing state");
    result = automatic(
        {0xe0302000u, 0x80000100u, 0xbf8c0000u, 0x4a020281u, 0xe0702000u, 0x80000100u, 0xbf810000u},
        {true, false});
    const auto increment = result.at("cases").begin().value();
    test(increment.at("status") == "gpu_not_allowed", "load/modify/store fixtures generated");
    for (const auto &attempt : increment.at("tests")) {
        const auto path =
            auto_output / path_from(attempt.at("generated_fixture").get<std::string>());
        const auto initial_bytes = read_bytes(path / "buffer0.bin");
        const auto expected_bytes = read_bytes(path / "expected-buffer0.bin");
        const auto wave = attempt.at("fixture").at("wave_size").get<unsigned>();
        for (unsigned i = 0; i < 1024; ++i) {
            const auto before = uint32_t(integer(initial_bytes, i * 4, 4));
            test(integer(expected_bytes, i * 4, 4) == (i < wave ? uint32_t(before + 1) : before),
                 "generated reference independently checked for wraparound and unchanged bytes");
        }
    }
    result = automatic({0x7e020287u, 0xe0702000u, 0x04000100u, 0xbf810000u}, {true, false});
    const auto partial = result.at("cases").begin().value();
    size_t prepared = 0, refused = 0;
    for (const auto &attempt : partial.at("tests")) {
        prepared += attempt.at("status") == "gpu_not_allowed";
        refused += attempt.at("status") == "unsupported";
    }
    test(partial.at("status") == "unsupported" && prepared == 2 && refused == 4,
         "partially executable synthetic state remains incomplete, never cherry-picked as a pass");
    return checks;
}
