#include "shader_lab/lab.hpp"
#include <array>
#include <set>

namespace sl {
json normalize_pixel_profile(Bytes header, const json &pixel, uint32_t wave_size) {
    std::string reason;
    if (!valid_header(header, reason) || header[0x5a] != 1)
        throw std::runtime_error("pixel profile requires a valid pixel AGC header");
    if (!pixel.is_object() || (wave_size != 32 && wave_size != 64))
        throw std::runtime_error("invalid pixel profile object or wave size");
    constexpr std::array booleans = {"ps_pos_x",
                                     "ps_pos_y",
                                     "ps_pos_z",
                                     "ps_pos_w",
                                     "ps_front_face",
                                     "ps_ancillary",
                                     "ps_no_perspective",
                                     "ps_pixel_kill_enable",
                                     "ps_depth_export_enable",
                                     "ps_sample_mask_export_enable",
                                     "ps_sample_shading",
                                     "dual_source_blending",
                                     "alpha_blend_source_remap",
                                     "ps_early_z",
                                     "ps_execute_on_noop"};
    std::set<std::string> allowed = {"input_num",
                                     "ps_system_input_base",
                                     "custom_interpolation_mask",
                                     "ps_perspective_center_vgpr",
                                     "ps_perspective_centroid_vgpr",
                                     "target_output_mode",
                                     "target_export_mapping",
                                     "scratch_size_dwords",
                                     "interpolator_settings"};
    allowed.insert(booleans.begin(), booleans.end());
    for (auto it = pixel.begin(); it != pixel.end(); ++it)
        if (!allowed.contains(it.key()))
            throw std::runtime_error("unknown pixel profile field: " + it.key());
    json defaults = json::array(), effective = json::object();
    auto select = [&](const char *name, json fallback) {
        if (pixel.contains(name))
            return pixel.at(name);
        defaults.push_back(name);
        return fallback;
    };
    auto u32 = [](const json &word) {
        if (!word.is_number_integer() || (!word.is_number_unsigned() && word.get<int64_t>() < 0) ||
            word.get<uint64_t>() > UINT32_MAX)
            throw std::runtime_error("pixel words must be unsigned 32-bit integers");
        return word.get<uint32_t>();
    };
    auto number = [&](const char *name, uint32_t fallback) {
        auto n = u32(select(name, fallback));
        effective[name] = n;
        return n;
    };
    const auto inputs = number("input_num", uint32_t(integer(header, 0x50, 4)));
    if (inputs > 32)
        throw std::runtime_error("pixel inputs exceed 32");
    const auto system_base = number("ps_system_input_base", 0);
    const auto custom = number("custom_interpolation_mask", 0);
    if (inputs < 32 && (custom >> inputs) != 0)
        throw std::runtime_error("custom interpolation mask exceeds input_num");
    const auto center = number("ps_perspective_center_vgpr", UINT32_MAX);
    const auto centroid = number("ps_perspective_centroid_vgpr", UINT32_MAX);
    number("scratch_size_dwords", uint32_t(integer(header, 0x54, 2)));
    for (const char *name : booleans) {
        auto flag = select(name, false);
        if (!flag.is_boolean())
            throw std::runtime_error(std::string(name) + " requires a boolean");
        effective[name] = flag;
    }
    auto array = [&](const char *name, size_t size, uint32_t fallback, uint32_t max) {
        auto words = select(name, std::vector<uint32_t>(size, fallback));
        if (!words.is_array() || words.size() != size)
            throw std::runtime_error(std::string(name) + " has the wrong number of entries");
        for (const auto &word : words)
            if (u32(word) > max)
                throw std::runtime_error(std::string(name) + " entry exceeds its encoded range");
        effective[name] = words;
    };
    array("interpolator_settings", inputs, 0, UINT32_MAX);
    array("target_output_mode", 8, 9, 15);
    // Packed physical-to-logical channel selectors: two bits per channel, RGBA=0xe4.
    array("target_export_mapping", 8, 0xe4, 255);
    std::array<bool, 256> vgprs{};
    auto reserve = [&](uint32_t start, uint32_t count) {
        if (start >= vgprs.size() || count > vgprs.size() - start)
            throw std::runtime_error("pixel input VGPR extent exceeds 256 registers");
        for (uint32_t i = 0; i < count; ++i) {
            if (vgprs[start + i])
                throw std::runtime_error("pixel system/barycentric input VGPRs overlap");
            vgprs[start + i] = true;
        }
    };
    uint32_t system_count = 0;
    for (const char *name :
         {"ps_pos_x", "ps_pos_y", "ps_pos_z", "ps_pos_w", "ps_front_face", "ps_ancillary"})
        system_count += effective.at(name).get<bool>();
    reserve(system_base, system_count);
    if (center != UINT32_MAX)
        reserve(center, 2);
    if (centroid != UINT32_MAX)
        reserve(centroid, 2);
    const auto &modes = effective.at("target_output_mode");
    const auto &mapping = effective.at("target_export_mapping");
    if (effective.at("alpha_blend_source_remap").get<bool>() &&
        !effective.at("dual_source_blending").get<bool>())
        throw std::runtime_error("alpha_blend_source_remap requires dual_source_blending");
    if (effective.at("dual_source_blending").get<bool>()) {
        if (modes[0] == 0 || modes[1] != modes[0])
            throw std::runtime_error("dual-source outputs require matching active MRT0/MRT1 modes");
        if (effective.at("alpha_blend_source_remap").get<bool>()) {
            if (modes[0] == 7 || mapping[1] != 0xe4)
                throw std::runtime_error(
                    "alpha remap requires a non-integer export and identity MRT1 mapping");
            for (size_t i = 2; i < 8; ++i)
                if (modes[i] != 0)
                    throw std::runtime_error("alpha remap cannot have other active render targets");
        } else if (mapping[1] != mapping[0]) {
            throw std::runtime_error("guest dual-source outputs require matching export mappings");
        }
    }
    effective["wave_size"] = wave_size;
    return {{"schema", 1},
            {"preparation", "supplied_pixel_compiler_inputs"},
            {"provenance_trust", "user_supplied_not_authenticated"},
            {"input", effective},
            {"defaulted_fields", defaults},
            {"draw_state_replayed", false}};
}
} // namespace sl
