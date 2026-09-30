#include "shader_lab/lab.hpp"
using namespace sl;
namespace {
std::vector<uint8_t> encode(const std::vector<uint32_t> &words) {
    std::vector<uint8_t> result;
    for (auto word : words)
        for (unsigned byte = 0; byte < 4; ++byte)
            result.push_back(uint8_t(word >> (8 * byte)));
    return result;
}
} // namespace
unsigned layout_real_tests(const fs::path &root, Bytes input_header, const fs::path &worker) {
    unsigned checks = 0;
    auto test = [&](bool ok, const char *message) {
        if (!ok)
            throw std::runtime_error(message);
        ++checks;
    };
    auto compile = [&](const char *name, const std::vector<uint32_t> &words,
                       const std::vector<uint32_t> &user_data) {
        const auto dir = root / name;
        std::vector<uint8_t> header(input_header.begin(), input_header.end());
        auto code = encode(words);
        for (unsigned byte = 0; byte < 4; ++byte)
            header[0x44 + byte] = uint8_t(code.size() >> (8 * byte));
        write_bytes(dir / "input/shader.header", header);
        write_bytes(dir / "input/shader.code", code);
        auto manifest = scan({dir / "input", dir / "dataset"});
        auto id = manifest.at("shaders").begin().key();
        const json profile = {{"schema", 1},
                              {"mode", "context_snapshot"},
                              {"stage", "CS"},
                              {"wave_size", 64},
                              {"host_subgroup_size", 64},
                              {"user_data", user_data},
                              {"compute",
                               {{"threads", {64, 1, 1}},
                                {"group_id", {false, false, false}},
                                {"thread_ids_num", 1},
                                {"workgroup_register", uint32_t(user_data.size())},
                                {"float_mode", 192}}}};
        atomic_json(dir / "profile.json", profile);
        auto summary =
            run({dir / "dataset", dir / "run", worker, dir / "profile.json", 1, 10000, 0, false});
        const auto &row = summary.at("results").at(id);
        if (row.at("status") != "spirv_valid_under_profile" ||
            row.at("details").at("compiler_layout").at("status") != "exported")
            throw std::runtime_error("layout fixture failed: " + row.dump());
        const auto artifacts = dir / "run" / path_from(row.at("artifacts").get<std::string>());
        const auto layout = read_json(artifacts / "compiler-layout.json");
        test(sha256(read_bytes(artifacts / "compiler-layout.json")) ==
                 row.at("details").at("compiler_layout").at("sha256").get<std::string>(),
             "layout artifact hash");
        test(layout.at("identity").at("spirv_sha256") ==
                     sha256(read_bytes(artifacts / "shader.spv")) &&
                 layout.at("identity").at("code_sha256") == sha256(code) &&
                 layout.at("identity").at("header_sha256") == sha256(header),
             "layout binds exact binaries");
        const auto profile_text = profile.dump();
        test(layout.at("identity").at("profile_json_sha256") ==
                 sha256(Bytes(reinterpret_cast<const uint8_t *>(profile_text.data()),
                              profile_text.size())),
             "layout binds effective profile JSON");
        test(layout.at("runtime_bindings") == "not_created" &&
                 layout.at("semantic_correctness") == "not_tested" &&
                 layout.at("spirv_valid") == true,
             "compiler layout does not claim execution");
        return layout;
    };
    const auto empty = compile("empty", {0xbf810000u}, {});
    test(empty.at("descriptors").empty() && empty.at("buffers").empty() &&
             empty.at("shader_data").at("location") == "none",
         "empty shader requires no runtime bindings");
    // v_mov_b32 v0,s4; buffer_store_dword v0,off,s[0:3],0; s_endpgm.
    const std::vector<uint32_t> store = {0x7e000204u, 0xe0700000u, 0x80000000u, 0xbf810000u};
    const std::vector<uint32_t> data = {0x1000u, 0u, 64u, 0x20014facu, 7u};
    const auto single = compile("buffer", store, data);
    const auto &binding = single.at("descriptors").at(0);
    test(binding.at("set") == 0 && binding.at("binding") == 0 &&
             binding.at("descriptor_type") == "storage_buffer" &&
             binding.at("descriptor_count") == 1 &&
             binding.at("resource_indices") == json::array({0}),
         "actual live buffer descriptor array");
    const auto &buffer = single.at("buffers").at(0);
    test(buffer.at("guest_address") == "0x000000001000" &&
             buffer.at("descriptor_size_bytes") == 64 && buffer.at("written") == true &&
             buffer.at("descriptor_words") == json::array({0x1000u, 0u, 64u, 0x20014facu}),
         "materialized buffer descriptor and access preserved");
    const auto &shader_data = single.at("shader_data");
    test(shader_data.at("location") == "push_constants" &&
             shader_data.at("words") == json::array({7, nullptr}) &&
             shader_data.at("user_data_registers").at(0).at("register") == 4,
         "live user-data copied and packed buffer offsets unresolved");
    test(single.at("buffer_offsets").at(0).at("shader_data_dword") == 1 &&
             single.at("buffer_offsets").at(0).at("bit_offset") == 0 &&
             single.at("buffer_offsets").at(0).at("value").is_null(),
         "runtime offset slot exact and not fabricated");
    auto relocated_data = data;
    relocated_data[0] = 0x9000;
    const auto relocated = compile("relocated", store, relocated_data);
    test(relocated.at("buffers").at(0).at("guest_address") != buffer.at("guest_address") &&
             relocated.at("identity").at("spirv_sha256") ==
                 single.at("identity").at("spirv_sha256") &&
             relocated.at("identity").at("profile_json_sha256") !=
                 single.at("identity").at("profile_json_sha256"),
         "relocation changes resource identity without changing compiled shader");
    const auto pair =
        compile("two-buffers",
                {0x7e000287u, 0xe0700000u, 0x80000000u, 0xe0700000u, 0x80010000u, 0xbf810000u},
                {0x1000u, 0u, 64u, 0x20014facu, 0x2000u, 0u, 128u, 0x20014facu});
    test(pair.at("descriptors").at(0).at("resource_indices") == json::array({0, 1}) &&
             pair.at("descriptors").at(0).at("descriptor_count") == 2 &&
             pair.at("buffer_offsets").at(1).at("bit_offset") == 8 &&
             pair.at("buffer_offsets").at(1).at("resource") == 1 &&
             pair.at("buffers").at(1).at("guest_address") == "0x000000002000",
         "descriptor array order and packed offset bytes preserved");
    return checks;
}
