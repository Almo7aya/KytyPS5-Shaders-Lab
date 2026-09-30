#include "shader_lab/lab.hpp"
#include <iostream>
#include <sstream>
using namespace sl;
namespace {
std::vector<uint8_t> bytes(const std::vector<uint32_t> &words) {
    std::vector<uint8_t> data;
    for (auto word : words)
        for (unsigned i = 0; i < 4; ++i)
            data.push_back(uint8_t(word >> (8 * i)));
    return data;
}
std::vector<uint8_t> store_kernel() {
    return bytes({0x7e020287u, 0xe0702000u, 0x80000100u, 0xbf810000u});
}
} // namespace
unsigned vulkan_encoding_tests() {
    const auto assembly = read_bytes(fs::path(SL_TEST_SOURCE_DIR) / "buffer-store.s");
    const std::string text(assembly.begin(), assembly.end()), marker = "// EXPECTED: ";
    const auto position = text.find(marker);
    if (position == std::string::npos)
        throw std::runtime_error("Vulkan fixture assembler envelope missing");
    const auto begin = position + marker.size();
    std::istringstream words(text.substr(begin, text.find('\n', begin) - begin));
    std::vector<uint8_t> expected;
    unsigned value = 0;
    while (words >> std::hex >> value) {
        if (value > 255)
            throw std::runtime_error("Vulkan fixture assembler byte out of range");
        expected.push_back(uint8_t(value));
    }
    if (expected != store_kernel())
        throw std::runtime_error("Vulkan fixture bytes differ from LLVM-checked assembly envelope");
    return 1;
}
unsigned vulkan_guard_tests(const fs::path &root, const fs::path &worker) {
    fs::create_directories(root / "backend");
    const auto request = root / "request.json";
    for (const auto &kind : {"cpu", "gpu"}) {
        atomic_json(request, {{"output", path_text(fs::absolute(root / "backend"))},
                              {"backend_kind", kind},
                              {"allow_gpu", false}});
        const auto child = process(worker, {"--execute-fixture", path_text(fs::absolute(request))},
                                   root, root / "worker.log", std::chrono::seconds(10));
        const auto response = read_json(root / "backend/response.json");
        if (child.exit_code || child.timed_out || response.at("status") != "unsupported" ||
            response.at("reason").get<std::string>().find("explicit allow_gpu") ==
                std::string::npos ||
            fs::exists(root / "backend/compilation"))
            throw std::runtime_error(
                "Vulkan worker opt-in gate did not precede compiler/device work");
    }
    return 2;
}
unsigned vulkan_real_tests(const fs::path &root, Bytes input_header, const fs::path &worker) {
    unsigned checks = 0, serial = 0;
    auto run_case = [&](unsigned wave, bool wrong_reference, bool partial_exec,
                        unsigned first_word = 0) {
        const auto dir = root / std::to_string(serial++), fixture_dir = dir / "fixture";
        // v_mov_b32 v1,7; buffer_store_dword v1,v0,s[0:3],0 idxen; s_endpgm.
        // v0 is local invocation x. Descriptor stride four means each lane writes its own word.
        const auto code = store_kernel();
        std::vector<uint8_t> header(input_header.begin(), input_header.end());
        for (unsigned i = 0; i < 4; ++i)
            header[0x44 + i] = uint8_t(code.size() >> (8 * i));
        write_bytes(fixture_dir / "header.bin", header);
        write_bytes(fixture_dir / "code.bin", code);
        std::vector<uint32_t> initial(wave + 4, 0x12345678u), expected = initial;
        for (unsigned i = 0; i < wave; ++i)
            expected[first_word + i] = wrong_reference ? 8u : 7u;
        write_bytes(fixture_dir / "initial.bin", bytes(initial));
        write_bytes(fixture_dir / "expected.bin", bytes(expected));
        atomic_json(fixture_dir / "profile.json",
                    {{"schema", 1},
                     {"mode", "context_snapshot"},
                     {"stage", "CS"},
                     {"wave_size", wave},
                     {"host_subgroup_size", 64},
                     {"user_data", {0x1000u + first_word * 4, 0x00040000u, wave, 0x20014facu}},
                     {"compute",
                      {{"threads", {wave, 1, 1}},
                       {"group_id", {false, false, false}},
                       {"thread_ids_num", 1},
                       {"workgroup_register", 4},
                       {"tg_size_en", false},
                       {"lds_size_dwords", 0},
                       {"scratch_size_dwords", 0},
                       {"float_mode", 192}}}});
        auto file = [&](const char *name) {
            return json{{"file", name}, {"sha256", hash_file(fixture_dir / name)}};
        };
        auto resource = file("initial.bin");
        resource["kind"] = "buffer";
        resource["type"] = "u32";
        resource["access"] = "read_write";
        resource["binding"] = {3, 19}; // Fixture IDs are not Kyty native descriptor slots.
        resource["guest_address"] = "0000000000001000";
        atomic_json(fixture_dir / "fixture.json",
                    {{"schema", 1},
                     {"kind", "shader_lab_execution_fixture"},
                     {"id", "buffer-indexed-store-golden-v1"},
                     {"shader", {{"header", file("header.bin")}, {"code", file("code.bin")}}},
                     {"profile", file("profile.json")},
                     {"execution",
                      {{"stage", "CS"},
                       {"wave_size", wave},
                       {"exec_mask", partial_exec ? "0000000000000001"
                                     : wave == 32 ? "00000000ffffffff"
                                                  : "ffffffffffffffff"},
                       {"workgroup_size", {wave, 1, 1}},
                       {"dispatch_size", {1, 1, 1}}}},
                     {"resources", {{"result", resource}}}});
        auto output = file("expected.bin");
        output["kind"] = "buffer";
        output["type"] = "u32";
        output["comparison"] = {{"mode", "exact"}};
        atomic_json(
            fixture_dir / "reference.json",
            {{"schema", 1},
             {"fixture", execution_fixture_identity(fixture_dir / "fixture.json")},
             {"reference_source",
              {{"kind", "independent_model"},
               {"identifier", "hand-specified-store-golden/1"},
               {"method",
                "Each active lane stores literal seven in its own slot; surrounding sentinels "
                "remain unchanged. Expected bytes are independent of Kyty and Vulkan."}}},
             {"outputs", {{"result", output}}}});
        if (!partial_exec && !wrong_reference) {
#ifdef _WIN32
            const auto cpu_worker = worker.parent_path() / "shader-cpu-reference.exe";
#else
            const auto cpu_worker = worker.parent_path() / "shader-cpu-reference";
#endif
            const auto reference_result =
                execute_fixture({fixture_dir / "fixture.json", fixture_dir / "reference.json",
                                 cpu_worker, dir / "cpu-run", 10000, "cpu", false});
            if (reference_result.at("status") != "match")
                throw std::runtime_error(
                    "independent CPU model did not match Vulkan fixture's golden reference: " +
                    reference_result.dump());
            const auto cpu_trace = read_json(dir / "cpu-run/backend/model-trace.json");
            if (cpu_trace.at("buffer_stores") != wave || cpu_trace.at("host_gpu_used") != false)
                throw std::runtime_error("CPU descriptor-buffer execution evidence missing");
            std::cout << "CPU_BUFFER_EVIDENCE " << cpu_trace.dump() << '\n';
        }
        auto result = execute_fixture({fixture_dir / "fixture.json", fixture_dir / "reference.json",
                                       worker, dir / "run", 60000, "gpu", true});
        const auto wanted = partial_exec ? "unsupported" : wrong_reference ? "mismatch" : "match";
        if (result.at("status") != wanted) {
            const auto log = read_bytes(dir / "run/backend/worker.log");
            throw std::runtime_error("Vulkan replay fixture: " + result.dump() + "\n" +
                                     std::string(log.begin(), log.end()));
        }
        if (!partial_exec) {
            const auto trace = read_json(dir / "run/backend/replay-trace.json");
            if (trace.at("execution") != "Vulkan_compute_dispatch_and_fence_readback" ||
                trace.at("resource_mapping").at(0).at("fixture_resource") != "result")
                throw std::runtime_error(
                    "Vulkan replay trace missing actual execution/resource mapping");
            const auto &mapping = trace.at("resource_mapping").at(0);
            if (mapping.at("host_offset").get<uint64_t>() +
                    mapping.at("packed_byte_adjustment").get<uint32_t>() !=
                first_word * 4)
                throw std::runtime_error("Vulkan subrange offset was not preserved");
            const auto log_bytes = read_bytes(dir / "run/backend/worker.log");
            const std::string log(log_bytes.begin(), log_bytes.end());
            if (log.find("Validation Error") != std::string::npos ||
                log.find("VUID-") != std::string::npos)
                throw std::runtime_error("Vulkan validation-layer diagnostic: " + log);
            std::cout << "VULKAN_REPLAY_EVIDENCE " << trace.dump() << '\n';
        }
        if (wave == 32 && !partial_exec && first_word == 0) {
            // The public two-folder command must exercise the same real backend and
            // expose both matching and deliberately wrong references in its HTML.
            const auto input = dir / "i", evidence = input / ".shader-lab";
            fs::create_directories(evidence);
            fs::copy(fixture_dir, evidence / "f", fs::copy_options::recursive);
            write_text(input / "game/eboot.bin", "synthetic");
            write_bytes(input / "game/test.header", header);
            write_bytes(input / "game/test.code", code);
            const auto id = sha256(header) + "-" + sha256(code);
            atomic_json(evidence / "semantics.json",
                        {{"schema", 1},
                         {"tests", json::array({{{"case_id", id},
                                                 {"fixture", "f/fixture.json"},
                                                 {"reference", "f/reference.json"}}})}});
            const auto cli =
                worker.parent_path() / fs::path("shader-lab").replace_extension(worker.extension());
            const auto attempt =
                process(cli, {path_text(input), path_text(dir / "o"), "--semantic", "--allow-gpu"},
                        dir, dir / "main.log", std::chrono::seconds(90));
            if (attempt.exit_code || attempt.timed_out)
                throw std::runtime_error("semantic main command failed: " +
                                         path_text(dir / "main.log"));
            const auto semantic = read_json(dir / "o/semantic/results.json");
            if (semantic.at("cases").at(id).at("status") !=
                (wrong_reference ? "mismatch" : "matched_test_inputs"))
                throw std::runtime_error("main command lost actual Vulkan comparison outcome");
            const auto html_bytes = read_bytes(dir / "o/report.html");
            const std::string html(html_bytes.begin(), html_bytes.end());
            if (html.find("comparison.json") == std::string::npos ||
                html.find("semantic-filter") == std::string::npos)
                throw std::runtime_error("semantic report lacks comparison links/filter");
            ++checks;
            if (!wrong_reference) {
                const auto automatic_input = dir / "ai", automatic_output = dir / "ao";
                write_text(automatic_input / "game/eboot.bin", "synthetic");
                write_bytes(automatic_input / "game/test.header", header);
                write_bytes(automatic_input / "game/test.code", code);
                const auto increment_code =
                    bytes({0xe0302000u, 0x80000100u, 0xbf8c0000u, 0x4a020281u, 0xe0702000u,
                           0x80000100u, 0xbf810000u});
                auto increment_header = header;
                for (unsigned b = 0; b < 4; ++b)
                    increment_header[0x44 + b] = uint8_t(increment_code.size() >> (8 * b));
                write_bytes(automatic_input / "game/inc.header", increment_header);
                write_bytes(automatic_input / "game/inc.code", increment_code);
                const auto prepared = process(
                    cli, {path_text(automatic_input), path_text(automatic_output), "--semantic"},
                    dir, dir / "prepare.log", std::chrono::seconds(90));
                if (prepared.exit_code || prepared.timed_out)
                    throw std::runtime_error("main command CPU-only fixture preparation failed");
                const auto preparation = read_json(automatic_output / "semantic/results.json");
                if (preparation.at("status_counts").value("gpu_not_allowed", 0) != 2)
                    throw std::runtime_error(
                        "CPU-only preparation unexpectedly executed/failed translated shaders");
                const auto automatic =
                    process(cli,
                            {path_text(automatic_input), path_text(automatic_output), "--semantic",
                             "--allow-gpu"},
                            dir, dir / "auto.log", std::chrono::seconds(90));
                if (automatic.exit_code || automatic.timed_out)
                    throw std::runtime_error("automatic main-command semantic generation failed: " +
                                             path_text(dir / "auto.log"));
                const auto assessment =
                    read_json(automatic_output / "semantic/results.json").at("cases").at(id);
                if (assessment.at("status") != "matched_test_inputs" ||
                    assessment.at("tests").size() != 6)
                    throw std::runtime_error("automatic CPU/Vulkan tests did not all match: " +
                                             assessment.dump());
                if (fs::exists(automatic_input / ".shader-lab"))
                    throw std::runtime_error("automatic semantic workflow wrote into game input");
                const auto increment_assessment =
                    read_json(automatic_output / "semantic/results.json")
                        .at("cases")
                        .at(sha256(increment_header) + "-" + sha256(increment_code));
                if (increment_assessment.at("status") != "matched_test_inputs" ||
                    increment_assessment.at("tests").size() != 6)
                    throw std::runtime_error(
                        "generated load/modify/store Vulkan comparisons failed: " +
                        increment_assessment.dump());
                const auto report_bytes = read_bytes(automatic_output / "report.html");
                const std::string report_text(report_bytes.begin(), report_bytes.end());
                if (report_text.find("model-trace.json") == std::string::npos ||
                    report_text.find("generation.json") == std::string::npos)
                    throw std::runtime_error(
                        "automatic report is missing generation/model evidence links");
                ++checks;
            }
        }
        ++checks;
    };
    run_case(32, false, false);
    run_case(64, false, false);
    run_case(32, true, false);
    run_case(32, false, true);
    run_case(32, false, false, 1);
    return checks;
}
