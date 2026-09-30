#include "shader_lab/lab.hpp"
#include <iostream>
using namespace sl;
namespace {
std::vector<uint8_t> bytes(const std::vector<uint32_t> &words) {
    std::vector<uint8_t> data;
    for (auto word : words)
        for (unsigned i = 0; i < 4; ++i)
            data.push_back(uint8_t(word >> (8 * i)));
    return data;
}
} // namespace
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
        const auto code = bytes({0x7e020287u, 0xe0702000u, 0x80000100u, 0xbf810000u});
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
        ++checks;
    };
    run_case(32, false, false);
    run_case(64, false, false);
    run_case(32, true, false);
    run_case(32, false, true);
    run_case(32, false, false, 1);
    return checks;
}
