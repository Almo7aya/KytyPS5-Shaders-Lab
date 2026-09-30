#include "shader_lab/lab.hpp"

using namespace sl;

// A deliberately tiny test oracle, never used as a replacement for the Kyty compiler.
bool minimize_fixture_worker(const json &request) {
    const auto &profile = request.at("profile");
    auto mode = profile.value("test_mode", "");
    if (mode != "reduce" && mode != "reduce_bad_inventory")
        return false;
    auto code = read_bytes(path_from(request.at("code").get<std::string>()));
    auto out = path_from(request.at("output").get<std::string>());
    bool has_input = profile.at("user_data")[0] == 7, has_memory = false;
    for (const auto &range : profile.at("memory"))
        if (range.at("base") == 4096 && range.at("words")[0] == 42)
            has_memory = true;
    bool has_failure = integer(code, 4, 4) == 0xdeadbeef;
    bool has_end = integer(code, 12, 4) == 0xbf810000;
    json response = {{"schema", 1}, {"id", request.at("id")}, {"last_phase", "decode"}};
    if (!has_failure || !has_end) {
        response["status"] = "fixture_pass";
    } else {
        response["status"] = "unsupported_instruction";
        response["unsupported"] =
            json::array({{{"family", "FIXTURE"},
                          {"opcode_id", 99},
                          {"text", has_input && has_memory ? "0x00000004: fixture original failure"
                                                           : "0x00000004: DIFFERENT failure"}}});
    }
    json instructions = json::array();
    for (size_t word = 0; word < code.size() / 4; ++word)
        instructions.push_back({{"pc", word * 4}, {"words", 1}});
    if (mode == "reduce_bad_inventory")
        instructions[0]["pc"] = 4;
    atomic_json(out / "instructions.json",
                {{"schema", 1}, {"code_sha256", sha256(code)}, {"instructions", instructions}});
    atomic_json(out / "response.json", response);
    return true;
}

unsigned minimize_tests(const fs::path &root, Bytes input_header, const fs::path &worker) {
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
    json signature_row = {
        {"status", "unsupported_instruction"},
        {"details",
         {{"last_phase", "decode"},
          {"unsupported",
           json::array(
               {{{"family", "fixture"}, {"opcode_id", 1}, {"text", "original diagnostic"}}})}}};
    auto original_signature = failure_signature(signature_row);
    test(!original_signature.is_null(), "detailed unsupported instruction has a predicate");
    signature_row["details"]["unsupported"][0]["text"] = "different diagnostic";
    test(failure_signature(signature_row) != original_signature,
         "same status with changed diagnostic is not same predicate");
    test(failure_signature({{"status", "timeout"}, {"last_phase", {{"phase", "translate"}}}})
             .is_null(),
         "timeouts are not stable reduction predicates");
    test(failure_signature({{"status", "worker_crash_or_error"},
                            {"exit_code", 17},
                            {"last_phase", {{"phase", "translate"}}}})
             .is_null(),
         "bare exit code does not identify a failure");
    test(failure_signature(
             {{"status", "unsupported_instruction"}, {"details", {{"last_phase", "decode"}}}})
             .is_null(),
         "incomplete diagnostics cannot authorize a reduction");
    std::vector<uint8_t> header(input_header.begin(), input_header.end()), code(16);
    header[0x44] = 16;
    const uint32_t words[] = {0xbe800005, 0xdeadbeef, 0xbe800006, 0xbf810000};
    for (size_t i = 0; i < 4; ++i)
        for (size_t byte = 0; byte < 4; ++byte)
            code[i * 4 + byte] = uint8_t(words[i] >> (byte * 8));
    write_bytes(root / "input/fixture.header", header);
    write_bytes(root / "input/fixture.code", code);
    auto manifest = scan({root / "input", root / "dataset"});
    test(manifest.at("shaders").size() == 1, "reduction fixture has one source case");
    auto id = manifest.at("shaders").begin().key();
    auto profile_path = root / "profile.json";
    json profile = {{"test_mode", "reduce"},
                    {"user_data", {7, 8, 9}},
                    {"memory", json::array({{{"base", 4096}, {"words", {42, 43}}},
                                            {{"base", 8192}, {"words", {67}}}})}};
    atomic_json(profile_path, profile);
    run({root / "dataset", root / "source-run", worker, profile_path, 1, 5000, 0, false});
    export_repro(root / "dataset", root / "source-run/results.json", id, root / "bundle");
    MinimizeOptions options{root / "bundle", worker, root / "reduced", 0, 128, 2};
    auto reduced = minimize_repro(options);
    test(reduced["state"] == "fixed_point" && reduced["final_verified"] == true,
         "reducer reaches a freshly confirmed transformation fixed point");
    test(reduced["attempts_used"].get<uint64_t>() <= 128,
         "attempt budget includes baselines and final checks");
    auto best_profile = read_json(root / "reduced/minimized-bundle/profile.json");
    test(best_profile["user_data"] == json({7, 0, 0}),
         "reducer zeros only unnecessary input words");
    test(best_profile["memory"].size() == 1 && best_profile["memory"][0]["words"] == json({42, 0}),
         "reducer removes unrelated ranges and preserves necessary memory");
    auto best_code = read_bytes(root / "reduced/minimized-bundle/code.bin");
    test(best_code.size() == code.size() && integer(best_code, 0, 4) == 0xbf800000 &&
             integer(best_code, 8, 4) == 0xbf800000 && integer(best_code, 4, 4) == 0xdeadbeef &&
             integer(best_code, 12, 4) == 0xbf810000,
         "instruction reduction preserves PCs and necessary instructions");
    auto replay =
        replay_repro(root / "reduced/minimized-bundle", worker, root / "replayed-minimum");
    test(replay["status_matches"] == true && replay["worker_matches"] == true,
         "reduced case remains portable and independently replayable");
    bool rejected_other_failure = false;
    for (const auto &attempt : reduced["attempts"])
        if (attempt["status"] == "unsupported_instruction" && attempt["matches"] == false)
            rejected_other_failure = true;
    test(rejected_other_failure, "reducer rejects a different failure with identical status");
    test(hash_file(root / "bundle/code.bin") == sha256(code), "original repro is never changed");
    options.output = root / "limited";
    options.max_attempts = 4;
    auto limited = minimize_repro(options);
    test(limited["state"] == "budget_exhausted" && limited["attempts_used"] == 4 &&
             limited["final_verified"] == true,
         "exhausted search still reserves final confirmation attempts");
    test(hash_file(root / "limited/minimized-bundle/code.bin") == sha256(code),
         "zero proposal budget returns confirmed original");
    rejects([&] { minimize_repro(options); }, "reduction refuses output overwrite");
    options.output = root / "bundle/nested";
    rejects([&] { minimize_repro(options); }, "reduction cannot modify source bundle");
    auto mismatched = read_json(root / "source-run/results.json");
    mismatched["results"][id]["details"]["unsupported"][0]["text"] =
        "a different claimed original failure";
    atomic_json(root / "source-run/mismatch.json", mismatched);
    export_repro(root / "dataset", root / "source-run/mismatch.json", id, root / "mismatch-bundle");
    auto mismatch =
        minimize_repro({root / "mismatch-bundle", worker, root / "mismatch-reduction", 0, 8, 2});
    test(mismatch["state"] == "baseline_not_reproduced" && mismatch["final_verified"] == false &&
             mismatch["attempts_used"] == 1 &&
             !fs::exists(root / "mismatch-reduction/minimized-bundle"),
         "original predicate must repeat before any reduction is attempted");
    profile["test_mode"] = "reduce_bad_inventory";
    atomic_json(profile_path, profile);
    run({root / "dataset", root / "bad-inventory-run", worker, profile_path, 1, 5000, 0, false});
    export_repro(root / "dataset", root / "bad-inventory-run/results.json", id,
                 root / "bad-inventory-bundle");
    options = {root / "bad-inventory-bundle", worker, root / "bad-inventory-reduction", 0, 32, 2};
    rejects([&] { minimize_repro(options); }, "fresh but overlapping decoder extents are rejected");
    test(!fs::exists(root / "bad-inventory-reduction/minimized-bundle"),
         "invalid decoder metadata cannot yield a minimized bundle");
    return checks;
}

unsigned minimize_real_tests(const fs::path &root, Bytes input_header, const fs::path &worker) {
    std::vector<uint8_t> header(input_header.begin(), input_header.end()), code(16);
    header[0x44] = 16;
    // RDNA SOP1 s_mov_b32 s0, literal; reserved SOPP opcode; s_endpgm.
    // The literal instruction must be replaced as two dwords, preserving the failure PC.
    const uint32_t words[] = {0xbe8003ff, 0x3f800000, 0xbfff0000, 0xbf810000};
    for (size_t i = 0; i < 4; ++i)
        for (size_t byte = 0; byte < 4; ++byte)
            code[i * 4 + byte] = uint8_t(words[i] >> (byte * 8));
    write_bytes(root / "input/fixture.header", header);
    write_bytes(root / "input/fixture.code", code);
    auto manifest = scan({root / "input", root / "dataset"});
    auto id = manifest.at("shaders").begin().key();
    atomic_json(root / "profile.json", {{"schema", 1}, {"mode", "header_probe"}, {"stage", "CS"}});
    auto original = run(
        {root / "dataset", root / "source-run", worker, root / "profile.json", 1, 10000, 0, false});
    if (original.at("results").at(id).at("status") != "unsupported_instruction")
        throw std::runtime_error("real reduction fixture must report reserved SOPP instruction");
    export_repro(root / "dataset", root / "source-run/results.json", id, root / "bundle");
    auto reduced = minimize_repro({root / "bundle", worker, root / "reduced", 0, 48, 2});
    if (!reduced.at("final_verified").get<bool>() || reduced.at("state") != "fixed_point")
        throw std::runtime_error(
            "real compiler reduction must preserve its detailed unsupported diagnostic");
    auto result = read_bytes(root / "reduced/minimized-bundle/code.bin");
    if (result.size() != 16 || integer(result, 0, 4) != 0xbf800000 ||
        integer(result, 4, 4) != 0xbf800000 || integer(result, 8, 4) != 0xbfff0000 ||
        integer(result, 12, 4) != 0xbf810000)
        throw std::runtime_error(
            "real reducer must preserve PCs while replacing a two-word literal instruction");
    return 3;
}
