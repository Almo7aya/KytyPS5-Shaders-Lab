#include "shader_lab/lab.hpp"

using namespace sl;
namespace {
std::vector<uint8_t> pixel_header(Bytes header) {
    std::vector<uint8_t> result(header.begin(), header.end());
    result.at(0x5a) = 1;
    return result;
}
json basic_pixel() {
    return {{"input_num", 1},
            {"interpolator_settings", {0}},
            {"target_output_mode", {9, 0, 0, 0, 0, 0, 0, 0}}};
}
uint32_t vintrp(uint32_t dst, uint32_t mode) {
    return (0x32u << 26) | (2u << 16) | (dst << 18) | (3u << 8) | mode;
}
uint32_t exp0(uint32_t target, bool done) {
    return (0x3eu << 26) | (target << 4) | 15u | (done ? 1u << 11 : 0);
}
uint32_t exp1(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return a | b << 8 | c << 16 | d << 24;
}
} // namespace

unsigned pixel_tests(Bytes input_header) {
    unsigned checks = 0;
    auto test = [&](bool ok, const char *name) {
        if (!ok)
            throw std::runtime_error(name);
        ++checks;
    };
    const auto header = pixel_header(input_header);
    const auto base = basic_pixel();
    auto normalized = normalize_pixel_profile(header, base, 32);
    const auto &input = normalized.at("input");
    test(input.at("wave_size") == 32 && input.at("ps_perspective_center_vgpr") == UINT32_MAX &&
             input.at("target_export_mapping") == json(std::vector<uint32_t>(8, 0xe4)),
         "pixel defaults retain disabled barycentrics and identity export mappings");
    test(normalized.at("defaulted_fields").size() == 21 &&
             normalized.at("draw_state_replayed") == false,
         "every omitted pixel field is disclosed without claiming draw replay");
    auto explicit_input = input;
    explicit_input.erase("wave_size");
    test(normalize_pixel_profile(header, explicit_input, 32).at("defaulted_fields").empty(),
         "complete explicit pixel profile has no hidden input defaults");
    for (const char *field : {"ps_pos_x", "ps_pos_y", "ps_pos_z", "ps_pos_w", "ps_front_face",
                              "ps_ancillary", "ps_no_perspective", "ps_pixel_kill_enable",
                              "ps_depth_export_enable", "ps_sample_mask_export_enable",
                              "ps_sample_shading", "ps_early_z", "ps_execute_on_noop"}) {
        auto p = base;
        p[field] = true;
        test(normalize_pixel_profile(header, p, 64).at("input").at(field) == true,
             "pixel flags survive normalization at wave64");
    }
    auto reject = [&](json profile, const char *name) {
        bool rejected = false;
        try {
            normalize_pixel_profile(header, profile, 32);
        } catch (const std::exception &) {
            rejected = true;
        }
        test(rejected, name);
    };
    for (auto invalid :
         {json(-1), json(1.5), json(true), json("1"), json(uint64_t(UINT32_MAX) + 1)}) {
        auto p = base;
        p["input_num"] = invalid;
        reject(p, "pixel integer fields reject coercion and overflow");
    }
    auto p = base;
    p["ps_pos_x"] = 1;
    reject(p, "pixel flags require booleans");
    p = base;
    p["unexpected"] = false;
    reject(p, "unknown pixel fields rejected");
    p = base;
    p["interpolator_settings"] = {0, 1};
    reject(p, "interpolation array must match active input count");
    p = base;
    p["custom_interpolation_mask"] = 2;
    reject(p, "custom interpolation bits cannot name absent inputs");
    p = base;
    p["target_output_mode"][0] = 16;
    reject(p, "output mode is a four-bit field");
    p = base;
    p["target_export_mapping"] = {256, 0, 0, 0, 0, 0, 0, 0};
    reject(p, "channel mapping is an eight-bit field");
    p = base;
    p["ps_system_input_base"] = 256;
    reject(p, "system input base is bounded even with no enabled system inputs");
    p = base;
    p["ps_system_input_base"] = 255;
    p["ps_pos_x"] = p["ps_pos_y"] = true;
    reject(p, "pixel system input block cannot overflow the VGPR file");
    p = base;
    p["ps_perspective_center_vgpr"] = 255;
    reject(p, "barycentric pair cannot overflow the VGPR file");
    p["ps_perspective_center_vgpr"] = 254;
    test(normalize_pixel_profile(header, p, 32).at("input").at("ps_perspective_center_vgpr") == 254,
         "last complete barycentric register pair is accepted");
    p["ps_perspective_centroid_vgpr"] = 253;
    reject(p, "overlapping barycentric pairs rejected");
    p = base;
    p["ps_perspective_center_vgpr"] = 0;
    p["ps_pos_x"] = true;
    reject(p, "barycentric and system inputs cannot overwrite one another");
    p = base;
    p["alpha_blend_source_remap"] = true;
    reject(p, "alpha remapping requires dual-source mode");
    p["dual_source_blending"] = true;
    reject(p, "dual-source mode requires MRT1 output metadata");
    p["target_output_mode"] = {4, 4, 0, 0, 0, 0, 0, 0};
    p["target_export_mapping"] = {0x1b, 0xe4, 0xe4, 0xe4, 0xe4, 0xe4, 0xe4, 0xe4};
    test(normalize_pixel_profile(header, p, 32).at("input").at("alpha_blend_source_remap") == true,
         "effective alpha-remap state accepts logical-alpha identity mapping");
    p["alpha_blend_source_remap"] = false;
    reject(p, "guest dual-source mapping must match MRT0");
    p["target_export_mapping"][1] = 0x1b;
    test(normalize_pixel_profile(header, p, 32).at("input").at("dual_source_blending") == true,
         "guest dual-source preserves matching color mappings");
    bool wrong_stage = false;
    try {
        normalize_pixel_profile(input_header, base, 32);
    } catch (const std::exception &) {
        wrong_stage = true;
    }
    test(wrong_stage,
         "pixel profile cannot reinterpret a compute header as captured pixel metadata");
    return checks;
}

unsigned pixel_real_tests(const fs::path &root, Bytes input_header, const fs::path &worker) {
    unsigned checks = 0;
    auto test = [&](bool ok, const char *name) {
        if (!ok)
            throw std::runtime_error(name);
        ++checks;
    };
    auto compile = [&](const char *name, const std::vector<uint32_t> &words, const json &pixel) {
        auto dir = root / name;
        auto header = pixel_header(input_header);
        std::vector<uint8_t> bytes;
        for (uint32_t word : words)
            for (unsigned i = 0; i < 4; ++i)
                bytes.push_back(uint8_t(word >> (8 * i)));
        for (unsigned i = 0; i < 4; ++i)
            header.at(0x44 + i) = uint8_t(bytes.size() >> (8 * i));
        write_bytes(dir / "input/shader.header", header);
        write_bytes(dir / "input/shader.code", bytes);
        auto manifest = scan({dir / "input", dir / "dataset"});
        const auto id = manifest.at("shaders").begin().key();
        atomic_json(dir / "profile.json", {{"schema", 1},
                                           {"stage", "PS"},
                                           {"mode", "context_snapshot"},
                                           {"wave_size", 32},
                                           {"user_data", json::array()},
                                           {"pixel", pixel}});
        auto result =
            run({dir / "dataset", dir / "run", worker, dir / "profile.json", 1, 10000, 0, false});
        const auto row = result.at("results").at(id);
        if (row.at("status") != "spirv_valid_under_profile")
            throw std::runtime_error(std::string("real pixel fixture ") + name + ": " + row.dump());
        ++checks;
        auto spv = read_bytes(dir / "run" / path_from(row.at("artifacts").get<std::string>()) /
                              "shader.spvasm");
        return std::pair{row.at("details"), std::string(spv.begin(), spv.end())};
    };
    auto profile = basic_pixel();
    profile["custom_interpolation_mask"] = 1;
    profile["ps_perspective_center_vgpr"] = 0;
    profile["ps_system_input_base"] = 2;
    profile["interpolator_settings"] = {0x420};
    // Synthetic VINTRP P0/P10/P20, V_ADD_F32 and EXP encodings, as exercised by
    // the official compiler's TestCustomVintrpMovTranslation. No captured shader data.
    const std::vector<uint32_t> custom = {vintrp(12, 2),
                                          vintrp(13, 0),
                                          vintrp(14, 1),
                                          (3u << 25) | (15u << 17) | (12u + 256u),
                                          (3u << 25) | (16u << 17) | (1u << 9) | (13u + 256u),
                                          exp0(0, true),
                                          exp1(15, 16, 14, 12),
                                          0xbf810000u};
    auto [custom_details, custom_spv] = compile("custom-interpolation", custom, profile);
    test(custom_spv.find("FragmentBarycentricKHR") != std::string::npos &&
             custom_spv.find("PerVertexKHR") != std::string::npos &&
             custom_spv.find("BaryCoordKHR") != std::string::npos,
         "custom interpolation metadata reaches actual barycentric SPIR-V lowering");
    test(custom_details.at("effective_pixel").at("input").at("custom_interpolation_mask") == 1,
         "worker reports normalized pixel inputs");
    profile = basic_pixel();
    profile["ps_no_perspective"] = true;
    auto [linear_details, linear_spv] =
        compile("no-perspective", {vintrp(12, 2), exp0(0, true), exp1(12, 12, 12, 12), 0xbf810000u},
                profile);
    test(linear_spv.find("NoPerspective") != std::string::npos,
         "no-perspective input reaches SPIR-V interpolation decorations");
    profile = basic_pixel();
    profile["target_output_mode"] = {4, 4, 0, 0, 0, 0, 0, 0};
    profile["target_export_mapping"] = {0x1b, 0xe4, 0xe4, 0xe4, 0xe4, 0xe4, 0xe4, 0xe4};
    profile["dual_source_blending"] = true;
    profile["alpha_blend_source_remap"] = true;
    const std::vector<uint32_t> exports = {
        0x7e0002ffu,   0x447a0000u,      0x7e0202ffu, 0x40000000u,    0x7e0402ffu,
        0x40400000u,   0x7e0602ffu,      0x3e800000u, exp0(0, false), exp1(0, 1, 2, 3),
        exp0(1, true), exp1(0, 1, 2, 3), 0xbf810000u};
    auto [alpha_details, alpha_spv] = compile("alpha-remap", exports, profile);
    test(alpha_spv.find("OpDecorate %out_mrt_1 Location 0") != std::string::npos &&
             alpha_spv.find("OpDecorate %out_mrt_1 Index 1") != std::string::npos &&
             alpha_spv.find("3 3 3 3") != std::string::npos &&
             alpha_spv.find("3 2 1 0") != std::string::npos,
         "alpha remap emits separate blend source and reversed physical color mapping");
    profile["alpha_blend_source_remap"] = false;
    profile["target_export_mapping"][1] = 0x1b;
    auto [dual_details, dual_spv] = compile("guest-dual-source", exports, profile);
    test(dual_spv.find("OpDecorate %out_mrt_1 Index 1") != std::string::npos &&
             dual_spv != alpha_spv,
         "guest dual-source and synthetic alpha remap produce distinct real modules");
    profile = basic_pixel();
    profile["ps_early_z"] = true;
    auto [early_details, early_spv] = compile("early-depth", {0xbf810000u}, profile);
    test(early_spv.find("EarlyFragmentTests") != std::string::npos,
         "explicit early-Z state reaches the real execution mode");
    profile["ps_sample_mask_export_enable"] = true;
    const std::vector<uint32_t> mask = {0x7e0002ffu, 0xffffffffu, exp0(8, true), exp1(0, 0, 0, 0),
                                        0xbf810000u};
    auto [mask_details, mask_spv] = compile("sample-mask", mask, profile);
    test(mask_spv.find("BuiltIn SampleMask") != std::string::npos &&
             mask_spv.find("EarlyFragmentTests") == std::string::npos,
         "sample-mask export reaches the interface and inhibits early tests");
    return checks;
}
