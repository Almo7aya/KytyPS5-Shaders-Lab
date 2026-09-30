#include "shader_lab/lab.hpp"
#include <array>
#include <sstream>
using namespace sl;

unsigned cpu_tests(const fs::path &root, Bytes input_header, const fs::path &worker) {
    unsigned checks = 0, serial = 0;
    auto test = [&](bool ok, const char *name) {
        if (!ok)
            throw std::runtime_error(name);
        ++checks;
    };
    auto bytes = [](const std::vector<uint32_t> &words) {
        std::vector<uint8_t> data;
        for (auto w : words)
            for (unsigned i = 0; i < 4; ++i)
                data.push_back(uint8_t(w >> (i * 8)));
        return data;
    };
    // Independently assembled GFX10.3 sequence; see integer-buffer.s and LLVM MC CI check.
    const std::vector<uint32_t> kernel = {0x34020082u, 0xdc308000u, 0x02000001u, 0xbf8c0000u,
                                          0x4a040481u, 0xdc708000u, 0x00000201u, 0xbf810000u};
    auto assembly = read_bytes(fs::path(SL_TEST_SOURCE_DIR) / "integer-buffer.s");
    std::string text(assembly.begin(), assembly.end());
    const std::string marker = "// EXPECTED: ";
    auto begin = text.find(marker);
    test(begin != std::string::npos, "independent encoding fixture contains expected bytes");
    begin += marker.size();
    std::istringstream encoded(text.substr(begin, text.find('\n', begin) - begin));
    unsigned byte = 0;
    std::vector<uint8_t> encoded_bytes;
    while (encoded >> std::hex >> byte) {
        test(byte <= 255, "assembler fixture expected bytes are bounded");
        encoded_bytes.push_back(uint8_t(byte));
    }
    test(encoded_bytes == bytes(kernel),
         "CPU fixture bytes match the independently assembled instruction sequence");
    const std::array<uint32_t, 8> initial = {0,           1,           0xffffffffu, 0x7fffffffu,
                                             0x80000000u, 0xfffffffeu, 41,          0x12345678u};
    const std::array<uint32_t, 8> incremented = {1,           2,           0,  0x80000000u,
                                                 0x80000001u, 0xffffffffu, 42, 0x12345679u};
    auto run_case = [&](const std::vector<uint32_t> &code, unsigned wave, uint64_t exec,
                        unsigned count, const std::vector<uint32_t> &input,
                        const std::vector<uint32_t> &expected, unsigned groups = 1,
                        const json &host = nullptr, const json &user_data = nullptr) {
        const auto dir = root / std::to_string(serial++), fixture_dir = dir / "fixture";
        auto header = std::vector<uint8_t>(input_header.begin(), input_header.end());
        for (unsigned i = 0; i < 4; ++i)
            header.at(0x44 + i) = uint8_t((code.size() * 4) >> (8 * i));
        write_bytes(fixture_dir / "header.bin", header);
        write_bytes(fixture_dir / "code.bin", bytes(code));
        write_bytes(fixture_dir / "input.bin", bytes(input));
        write_bytes(fixture_dir / "expected.bin", bytes(expected));
        json profile = {{"schema", 1},
                        {"mode", "context_snapshot"},
                        {"stage", "CS"},
                        {"wave_size", wave},
                        {"user_data", {0x1000, 0}},
                        {"compute",
                         {{"threads", {count, 1, 1}},
                          {"group_id", {false, false, false}},
                          {"thread_ids_num", 1},
                          {"workgroup_register", 2},
                          {"tg_size_en", false},
                          {"lds_size_dwords", 0},
                          {"scratch_size_dwords", 0},
                          {"float_mode", 192}}}};
        if (!host.is_null())
            profile["host"] = host;
        if (!user_data.is_null()) {
            profile["user_data"] = user_data;
            profile["compute"]["workgroup_register"] = user_data.size();
        }
        atomic_json(fixture_dir / "profile.json", profile);
        auto file = [&](const char *name) {
            return json{{"file", name}, {"sha256", hash_file(fixture_dir / name)}};
        };
        auto resource = file("input.bin");
        resource["kind"] = "buffer";
        resource["type"] = "u32";
        resource["access"] = "read_write";
        resource["binding"] = {0, 0};
        resource["guest_address"] = "0000000000001000";
        json fixture = {{"schema", 1},
                        {"kind", "shader_lab_execution_fixture"},
                        {"id", "integer-buffer-golden-v1"},
                        {"shader", {{"header", file("header.bin")}, {"code", file("code.bin")}}},
                        {"profile", file("profile.json")},
                        {"execution",
                         {{"stage", "CS"},
                          {"wave_size", wave},
                          {"exec_mask", hex(exec, 16)},
                          {"workgroup_size", {count, 1, 1}},
                          {"dispatch_size", {groups, 1, 1}}}},
                        {"resources", {{"result", resource}}}};
        atomic_json(fixture_dir / "fixture.json", fixture);
        auto output = file("expected.bin");
        output["kind"] = "buffer";
        output["type"] = "u32";
        output["comparison"] = {{"mode", "exact"}};
        atomic_json(fixture_dir / "reference.json",
                    {{"schema", 1},
                     {"fixture", execution_fixture_identity(fixture_dir / "fixture.json")},
                     {"reference_source",
                      {{"kind", "independent_model"},
                       {"identifier", "hand-specified-integer-golden/1"},
                       {"method", "Expected boundary bit patterns specified separately from the "
                                  "ISA interpreter; no Kyty output"}}},
                     {"outputs", {{"result", output}}}});
        return execute_fixture({fixture_dir / "fixture.json", fixture_dir / "reference.json",
                                worker, dir / "run", 10000, "cpu", false});
    };
    for (unsigned wave : {32u, 64u}) {
        const auto full = wave == 32 ? 0xffffffffull : UINT64_MAX;
        for (uint64_t exec :
             {uint64_t(full), uint64_t(full & 0x5555555555555555ull), uint64_t(0)}) {
            std::vector<uint32_t> input, expected;
            for (unsigned lane = 0; lane < wave; ++lane) {
                input.push_back(initial[lane % initial.size()]);
                expected.push_back((exec & (uint64_t(1) << lane))
                                       ? incremented[lane % incremented.size()]
                                       : input.back());
            }
            auto result = run_case(kernel, wave, exec, wave, input, expected);
            if (result.at("status") != "match")
                throw std::runtime_error("CPU golden fixture: " + result.dump());
            ++checks;
            test(result.at("backend").at("identifier") == "rdna2-integer/1",
                 "real CPU backend identity recorded");
        }
    }
    const std::array<std::pair<uint32_t, uint32_t>, 14> alu_cases = {{{17, 0x80000001u},
                                                                      {18, 5},
                                                                      {19, 5},
                                                                      {20, 0x80000001u},
                                                                      {22, 0x04000000u},
                                                                      {24, 0xfc000000u},
                                                                      {26, 0x20},
                                                                      {27, 1},
                                                                      {28, 0x80000005u},
                                                                      {29, 0x80000004u},
                                                                      {30, 0x7ffffffbu},
                                                                      {37, 0x80000006u},
                                                                      {38, 0x80000004u},
                                                                      {39, 0x7ffffffcu}}};
    for (auto [opcode, expected] : alu_cases) {
        auto code = kernel;
        code[4] = (opcode << 25) | (2u << 17) | (2u << 9) | 255u;
        code.insert(code.begin() + 5, 5u); // Literal operand independently specified.
        test(run_case(code, 32, 0xffffffffu, 8, std::vector<uint32_t>(8, 0x80000001u),
                      std::vector<uint32_t>(8, expected))
                     .at("status") == "match",
             "real ISA model matches fixed integer boundary table");
    }
    auto mov = kernel;
    mov[4] = 0x7e0402c1u; // v_mov_b32 v2, -1
    test(run_case(mov, 32, 0xffffffffu, 8, std::vector<uint32_t>(8, 0),
                  std::vector<uint32_t>(8, UINT32_MAX))
                 .at("status") == "match",
         "negative inline integer operand is an exact bit pattern");
    const std::vector<uint32_t> input(32, 0), expected(32, 1);
    for (unsigned subgroup : {32u, 64u})
        test(run_case(kernel, 32, 0xffffffffu, 32, input, expected, 1,
                      {{"schema", 1}, {"api_version", "1.3"}, {"subgroup_size", subgroup}})
                     .at("status") == "match",
             "host compiler subgroup declarations do not alter guest reference execution");
    auto bad = kernel;
    bad.erase(bad.begin() + 3);
    test(run_case(bad, 32, 0xffffffffu, 32, input, expected).at("status") == "unsupported",
         "pending load cannot be consumed without an explicit wait");
    bad = kernel;
    bad[4] = 0x4a040403u;
    test(run_case(bad, 32, 0xffffffffu, 32, input, expected).at("status") == "unsupported",
         "undefined scalar register is not silently zeroed");
    bad = kernel;
    bad[0] = 0x34020080u; // Shift by zero: lane addresses become unaligned.
    test(run_case(bad, 32, 0xffffffffu, 32, input, expected).at("status") == "unsupported",
         "unaligned memory access rejected");
    bad = kernel;
    bad[0] = 0x7e020280u; // v_mov v1, 0: every lane aliases the same output dword.
    test(run_case(bad, 32, 0xffffffffu, 32, input, expected).at("status") == "unsupported",
         "cross-lane memory race has no invented deterministic result");
    test(run_case(kernel, 32, 0xffffffffu, 32, input, expected, 2).at("status") == "unsupported",
         "cross-workgroup conflicting access is rejected");
    test(run_case(kernel, 32, 0xffffffffu, 64, input, expected).at("status") == "unsupported",
         "memory read beyond supplied resource is rejected");
    bad = kernel;
    bad.push_back(0xbf800000u);
    test(run_case(bad, 32, 0xffffffffu, 32, input, expected).at("status") == "unsupported",
         "trailing instructions are not silently ignored");
    bad = kernel;
    bad[4] = 0x06040481u; // floating V_ADD_F32: deliberately outside integer model.
    test(run_case(bad, 32, 0xffffffffu, 32, input, expected).at("status") == "unsupported",
         "floating arithmetic cannot masquerade as integer reference execution");
    for (size_t length = 1; length < kernel.size(); ++length)
        test(run_case({kernel.begin(), kernel.begin() + length}, 32, 0xffffffffu, 32, input,
                      expected)
                     .at("status") == "unsupported",
             "every incomplete program prefix rejected");
    const std::vector<uint32_t> buffer_kernel = {0xe0302000u, 0x80000100u, 0xbf8c0000u, 0x4a020281u,
                                                 0xe0702000u, 0x80000100u, 0xbf810000u};
    const auto buffer_assembly = read_bytes(fs::path(SL_TEST_SOURCE_DIR) / "buffer-increment.s");
    const std::string buffer_text(buffer_assembly.begin(), buffer_assembly.end());
    auto buffer_begin = buffer_text.find(marker);
    test(buffer_begin != std::string::npos, "buffer assembler envelope present");
    buffer_begin += marker.size();
    std::istringstream buffer_encoding(
        buffer_text.substr(buffer_begin, buffer_text.find('\n', buffer_begin) - buffer_begin));
    encoded_bytes.clear();
    while (buffer_encoding >> std::hex >> byte) {
        test(byte <= 255, "buffer assembler byte bounded");
        encoded_bytes.push_back(uint8_t(byte));
    }
    test(encoded_bytes == bytes(buffer_kernel),
         "descriptor-buffer kernel matches LLVM-checked envelope");
    const json descriptor = {0x1000u, 0x00040000u, 32u, 0x20014facu};
    std::vector<uint32_t> buffer_input, buffer_expected;
    for (unsigned i = 0; i < 32; ++i) {
        buffer_input.push_back(initial[i % initial.size()]);
        buffer_expected.push_back(incremented[i % incremented.size()]);
    }
    test(run_case(buffer_kernel, 32, 0xffffffffu, 32, buffer_input, buffer_expected, 1, nullptr,
                  descriptor)
                 .at("status") == "match",
         "independent descriptor-buffer model matches fixed wraparound golden values");
    auto missing_wait = buffer_kernel;
    missing_wait.erase(missing_wait.begin() + 2);
    test(run_case(missing_wait, 32, 0xffffffffu, 32, buffer_input, buffer_expected, 1, nullptr,
                  descriptor)
                 .at("status") == "unsupported",
         "buffer load requires wait before dependent arithmetic");
    auto short_descriptor = descriptor;
    short_descriptor[2] = 31;
    test(run_case(buffer_kernel, 32, 0xffffffffu, 32, buffer_input, buffer_expected, 1, nullptr,
                  short_descriptor)
                 .at("status") == "unsupported",
         "buffer descriptor bounds are checked independently of captured allocation");
    auto swizzled = descriptor;
    swizzled[1] = 0x80040000u;
    test(run_case(buffer_kernel, 32, 0xffffffffu, 32, buffer_input, buffer_expected, 1, nullptr,
                  swizzled)
                 .at("status") == "unsupported",
         "unmodeled swizzled descriptor rejected");
    return checks;
}
