#include "shader_lab/host_profile.hpp"
using namespace sl;
namespace {
json declared_host() {
    return {{"schema", 1},
            {"api_version", "1.3"},
            {"subgroup_size", 32},
            {"enabled_features", json::object()},
            {"enabled_extensions", json::array()},
            {"properties", {{"shaderSignedZeroInfNanPreserveFloat32", true}}},
            {"subgroup_operations", {"basic", "ballot"}},
            {"subgroup_stages", {"CS"}},
            {"limits",
             {{"maxComputeWorkGroupSize", {1024, 1024, 64}},
              {"maxComputeWorkGroupInvocations", 1024}}}};
}
std::vector<uint8_t> bytes(const std::vector<uint32_t> &words) {
    std::vector<uint8_t> result;
    for (auto w : words)
        for (unsigned b = 0; b < 4; ++b)
            result.push_back(uint8_t(w >> (8 * b)));
    return result;
}
std::vector<uint32_t> inventory() {
    // Deliberately only an inventory envelope; SPIRV-Tools validates real modules
    // in host_real_tests. This parser is not a second SPIR-V validator.
    return {0x07230203,
            0x00010600,
            0,
            16,
            0,
            (2u << 16) | 17,
            1,
            (2u << 16) | 17,
            4466,
            (5u << 16) | 15,
            5,
            1,
            0x6e69616d,
            0,
            (6u << 16) | 16,
            1,
            17,
            32,
            1,
            1,
            (4u << 16) | 16,
            1,
            4461,
            32};
}
void extension(std::vector<uint32_t> &words, std::string text) {
    text.push_back('\0');
    while (text.size() % 4)
        text.push_back('\0');
    words.push_back((uint32_t(1 + text.size() / 4) << 16) | 10);
    for (size_t i = 0; i < text.size(); i += 4) {
        uint32_t w = 0;
        for (unsigned b = 0; b < 4; ++b)
            w |= uint32_t(uint8_t(text[i + b])) << (8 * b);
        words.push_back(w);
    }
}
} // namespace

unsigned host_tests() {
    unsigned checks = 0;
    auto test = [&](bool ok, const char *message) {
        if (!ok)
            throw std::runtime_error(message);
        ++checks;
    };
    auto host = declared_host();
    const auto code = inventory();
    auto assessment = [&](const std::vector<uint32_t> &words, const json &h) {
        return assess_spirv_host(bytes(words), h);
    };
    test(assessment(code, host).at("status") == "satisfied",
         "explicit host requirements satisfied");
    test(assessment(code, host).at("runtime_compatibility") == "not_established",
         "host assessment never claims runtime compatibility");
    test(assessment(code, nullptr).at("status") == "not_supplied", "absence of host is disclosed");
    host["properties"].erase("shaderSignedZeroInfNanPreserveFloat32");
    test(assessment(code, host).at("status") == "unknown",
         "missing property is unknown not supported");
    host["properties"]["shaderSignedZeroInfNanPreserveFloat32"] = false;
    test(assessment(code, host).at("status") == "unsupported",
         "float execution width requires its own property");
    host = declared_host();
    auto modified = code;
    modified.back() = 64;
    test(assessment(modified, host).at("status") == "unknown",
         "float32 support cannot certify float64 mode");
    modified = code;
    modified.insert(modified.end(), {(2u << 16) | 17, 11});
    test(assessment(modified, host).at("status") == "unknown",
         "missing integer feature is not inferred");
    host["enabled_features"]["shaderInt64"] = false;
    test(assessment(modified, host).at("status") == "unsupported",
         "disabled Int64 feature rejected");
    host["enabled_features"]["shaderInt64"] = true;
    test(assessment(modified, host).at("status") == "satisfied",
         "enabled integer feature satisfies capability");
    modified = code;
    modified.insert(modified.end(), {(2u << 16) | 17, 12});
    host["enabled_features"]["shaderBufferInt64Atomics"] = true;
    test(assessment(modified, host).at("status") == "unknown",
         "atomic storage class cannot be inferred from capability");
    modified = code;
    modified.insert(modified.end(), {(2u << 16) | 17, 64});
    host["subgroup_operations"] = {"basic"};
    test(assessment(modified, host).at("status") == "unsupported",
         "ballot requires subgroup operation bit");
    host["subgroup_operations"] = {"basic", "ballot"};
    host["subgroup_stages"] = {"PS"};
    test(assessment(modified, host).at("status") == "unsupported",
         "subgroup support is stage-specific");
    host = declared_host();
    modified = code;
    modified.insert(modified.end(), {(2u << 16) | 17, 5284});
    extension(modified, "SPV_KHR_fragment_shader_barycentric");
    host["enabled_features"]["fragmentShaderBarycentric"] = true;
    test(assessment(modified, host).at("status") == "unsupported",
         "feature alone does not enable extension");
    host["enabled_extensions"] = {"VK_KHR_fragment_shader_barycentric"};
    test(assessment(modified, host).at("status") == "satisfied",
         "declared feature and extension checked together");
    extension(modified, "SPV_VENDOR_unknown");
    test(assessment(modified, host).at("status") == "unknown",
         "unmodeled SPIR-V extension is explicit unknown");
    modified = code;
    modified.insert(modified.end(), {(2u << 16) | 17, 999999});
    test(assessment(modified, host).at("status") == "unknown", "unmodeled capability cannot pass");
    host = declared_host();
    host["limits"]["maxComputeWorkGroupInvocations"] = 16;
    test(assessment(code, host).at("status") == "unsupported",
         "actual emitted local size checked against host limit");
    host = declared_host();
    host["limits"]["maxComputeWorkGroupSize"] = {16, 1024, 64};
    test(assessment(code, host).at("status") == "unsupported",
         "per-axis limit enforced independently");
    host = declared_host();
    host["enabled_extensions"] = {"VK_KHR_portability_subset"};
    test(assessment(code, host).at("status") == "unknown",
         "portability restrictions are not silently ignored");
    host = declared_host();
    test(compiler_host_subgroup(json::object()) == 32, "legacy default subgroup retained");
    host["subgroup_size"] = 64;
    test(compiler_host_subgroup({{"host", host}}) == 64, "declared host drives compiler subgroup");
    auto small_host = host;
    small_host["subgroup_size"] = 8;
    test(normalize_host_profile(small_host).at("subgroup_size") == 8,
         "queried software device subgroup accepted by assessment schema");
    auto reject = [&](const json &p) {
        bool threw = false;
        try {
            compiler_host_subgroup(p);
        } catch (const std::exception &) {
            threw = true;
        }
        test(threw, "malformed or conflicting host profile rejected");
    };
    reject({{"host", host}, {"host_subgroup_size", 32}});
    reject({{"host_subgroup_size", true}});
    reject({{"host", small_host}});
    reject({{"host_subgroup_size", 32.5}});
    reject({{"host_subgroup_size", -32}});
    for (const auto &field : {"subgroup_size", "schema"}) {
        auto invalid = host;
        invalid[field] = true;
        reject({{"host", invalid}});
    }
    auto invalid = host;
    invalid["enabled_features"]["shaderInt64"] = "true";
    reject({{"host", invalid}});
    invalid = host;
    invalid["enabled_features"]["inventedFeature"] = true;
    reject({{"host", invalid}});
    invalid = host;
    invalid["enabled_extensions"] = {"VK_EXT_mesh_shader", "VK_EXT_mesh_shader"};
    reject({{"host", invalid}});
    invalid = host;
    invalid["limits"]["maxComputeWorkGroupSize"] = {1024, -1, 64};
    reject({{"host", invalid}});
    modified = code;
    modified.push_back(0);
    bool malformed = false;
    try {
        assessment(modified, host);
    } catch (const std::exception &) {
        malformed = true;
    }
    test(malformed, "zero wordcount cannot stall module parser");
    return checks;
}

unsigned host_real_tests(const fs::path &root, Bytes input_header, const fs::path &worker) {
    unsigned checks = 0;
    auto test = [&](bool ok, const char *message) {
        if (!ok)
            throw std::runtime_error(message);
        ++checks;
    };
    write_bytes(root / "input/shader.header", input_header);
    write_bytes(root / "input/shader.code", bytes({0xbf810000u}));
    const auto manifest = scan({root / "input", root / "dataset"});
    const auto id = manifest.at("shaders").begin().key();
    auto profile = json{{"schema", 1},
                        {"mode", "context_snapshot"},
                        {"stage", "CS"},
                        {"wave_size", 64},
                        {"user_data", json::array()},
                        {"host", declared_host()},
                        {"compute",
                         {{"threads", {64, 1, 1}},
                          {"group_id", {false, false, false}},
                          {"thread_ids_num", 1},
                          {"workgroup_register", 0},
                          {"float_mode", 192}}}};
    auto compile = [&](const json &p, const char *name) {
        const auto dir = root / name;
        atomic_json(dir / "profile.json", p);
        auto summary =
            run({root / "dataset", dir / "run", worker, dir / "profile.json", 1, 10000, 0, false});
        return summary.at("results").at(id);
    };
    const auto paired = compile(profile, "paired");
    if (paired.at("status") != "spirv_valid_under_profile")
        throw std::runtime_error("host fixture failed: " + paired.dump());
    test(paired.at("details").at("host_assessment").at("status") == "satisfied",
         "actual emitted paired-wave requirements match host");
    test(paired.at("details").at("effective_compute").at("host_subgroup_size") == 32,
         "actual compiler receives host32");
    const auto paired_dir =
        root / "paired/run" / path_from(paired.at("artifacts").get<std::string>());
    test(read_json(paired_dir / "host-assessment.json") ==
             paired.at("details").at("host_assessment"),
         "host artifact agrees with worker result");
    profile["host"]["subgroup_size"] = 64;
    const auto native = compile(profile, "native");
    test(native.at("status") == "spirv_valid_under_profile" &&
             native.at("details").at("host_assessment").at("status") == "satisfied",
         "actual host64 module requirements pass");
    test(native.at("details").at("effective_compute").at("host_subgroup_size") == 64 &&
             native.at("details").at("spirv_sha256") != paired.at("details").at("spirv_sha256"),
         "host subgroup profile changes actual compilation");
    profile["host"]["properties"]["shaderSignedZeroInfNanPreserveFloat32"] = false;
    const auto unsupported = compile(profile, "disabled-float-property");
    test(unsupported.at("status") == "spirv_valid_under_profile" &&
             unsupported.at("details").at("host_assessment").at("status") == "unsupported",
         "structural validity stays distinct from host support");
    profile["host_subgroup_size"] = 32;
    test(compile(profile, "conflict").at("status") == "adapter_error",
         "conflicting host state rejected before translation");
    return checks;
}
