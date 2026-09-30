#include "shader_lab/host_profile.hpp"
#include <algorithm>
#include <array>
#include <set>

namespace sl {
namespace {
void keys(const json &j, const std::set<std::string> &allowed) {
    if (!j.is_object())
        throw std::runtime_error("host profile field must be an object");
    for (auto it = j.begin(); it != j.end(); ++it)
        if (!allowed.contains(it.key()))
            throw std::runtime_error("unknown host field: " + it.key());
}
uint32_t u32(const json &j) {
    if (!j.is_number_integer() || (!j.is_number_unsigned() && j.get<int64_t>() < 0) ||
        j.get<uint64_t>() > UINT32_MAX)
        throw std::runtime_error("host value must be u32");
    return j.get<uint32_t>();
}
const std::set<std::string> features = {"geometryShader",
                                        "tessellationShader",
                                        "shaderFloat64",
                                        "shaderInt64",
                                        "shaderBufferInt64Atomics",
                                        "shaderSharedInt64Atomics",
                                        "shaderImageInt64Atomics",
                                        "shaderImageGatherExtended",
                                        "shaderClipDistance",
                                        "shaderCullDistance",
                                        "sampleRateShading",
                                        "shaderStorageImageReadWithoutFormat",
                                        "shaderStorageImageWriteWithoutFormat",
                                        "shaderOutputLayer",
                                        "shaderOutputViewportIndex",
                                        "bufferDeviceAddress",
                                        "workgroupMemoryExplicitLayout",
                                        "meshShader",
                                        "fragmentShaderBarycentric",
                                        "computeDerivativeGroupQuads",
                                        "computeDerivativeGroupLinear"};
const std::map<uint32_t, std::string> float_modes = {{4459, "shaderDenormPreserveFloat"},
                                                     {4460, "shaderDenormFlushToZeroFloat"},
                                                     {4461, "shaderSignedZeroInfNanPreserveFloat"},
                                                     {4462, "shaderRoundingModeRTEFloat"},
                                                     {4463, "shaderRoundingModeRTZFloat"}};
const std::set<std::string> stages = {"VS", "TCS", "TES", "GS", "PS", "CS", "Task", "Mesh"};
const std::set<std::string> operations = {
    "basic", "vote", "arithmetic", "ballot", "shuffle", "shuffle_relative", "clustered", "quad"};
void strings(const json &j, const std::set<std::string> &allowed, bool extension = false) {
    if (!j.is_array() || j.size() > 128)
        throw std::runtime_error("host list must be bounded array");
    std::set<std::string> seen;
    for (const auto &v : j) {
        if (!v.is_string())
            throw std::runtime_error("host list entry must be string");
        auto s = v.get<std::string>();
        if (extension
                ? (s.size() > 128 || !s.starts_with("VK_") ||
                   s.find_first_not_of(
                       "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != s.npos)
                : !allowed.contains(s))
            throw std::runtime_error("unknown host list value: " + s);
        if (!seen.insert(s).second)
            throw std::runtime_error("duplicate host list entry");
    }
}
void booleans(const json &j, const std::set<std::string> &allowed) {
    keys(j, allowed);
    for (const auto &v : j)
        if (!v.is_boolean())
            throw std::runtime_error("host feature/property must be boolean");
}
std::string stage(uint32_t model) {
    switch (model) {
    case 0:
        return "VS";
    case 1:
        return "TCS";
    case 2:
        return "TES";
    case 3:
        return "GS";
    case 4:
        return "PS";
    case 5:
        return "CS";
    case 5364:
        return "Task";
    case 5365:
        return "Mesh";
    default:
        return "unknown:" + std::to_string(model);
    }
}
} // namespace

json normalize_host_profile(const json &host) {
    if (host.is_null())
        return nullptr;
    keys(host, {"schema", "api_version", "subgroup_size", "enabled_features", "enabled_extensions",
                "properties", "subgroup_operations", "subgroup_stages", "limits"});
    if (u32(host.at("schema")) != 1 || host.at("api_version") != "1.3")
        throw std::runtime_error("host profile requires schema 1 and Vulkan api_version 1.3");
    const auto subgroup = u32(host.at("subgroup_size"));
    if (subgroup != 32 && subgroup != 64)
        throw std::runtime_error("host subgroup_size must be 32 or 64");
    if (host.contains("enabled_features"))
        booleans(host.at("enabled_features"), features);
    std::set<std::string> properties;
    for (const auto &[mode, prefix] : float_modes)
        for (auto width : {16, 32, 64})
            properties.insert(prefix + std::to_string(width));
    if (host.contains("properties"))
        booleans(host.at("properties"), properties);
    if (host.contains("enabled_extensions"))
        strings(host.at("enabled_extensions"), {}, true);
    if (host.contains("subgroup_operations"))
        strings(host.at("subgroup_operations"), operations);
    if (host.contains("subgroup_stages"))
        strings(host.at("subgroup_stages"), stages);
    if (host.contains("limits")) {
        const auto &limits = host.at("limits");
        keys(limits, {"maxComputeWorkGroupSize", "maxComputeWorkGroupInvocations"});
        if (limits.contains("maxComputeWorkGroupInvocations") &&
            !u32(limits.at("maxComputeWorkGroupInvocations")))
            throw std::runtime_error("host workgroup invocation limit must be nonzero");
        if (limits.contains("maxComputeWorkGroupSize")) {
            const auto &dims = limits.at("maxComputeWorkGroupSize");
            if (!dims.is_array() || dims.size() != 3)
                throw std::runtime_error("host workgroup size needs three axes");
            for (const auto &v : dims)
                if (!u32(v))
                    throw std::runtime_error("host axis limit must be nonzero");
        }
    }
    return host; // Missing values stay missing: never manufacture a device capability.
}

uint32_t compiler_host_subgroup(const json &profile) {
    const auto host = normalize_host_profile(profile.value("host", json(nullptr)));
    const auto size =
        host.is_null()
            ? (profile.contains("host_subgroup_size") ? u32(profile.at("host_subgroup_size")) : 32)
            : u32(host.at("subgroup_size"));
    if (size != 32 && size != 64)
        throw std::runtime_error("host_subgroup_size must be 32 or 64");
    if (profile.contains("host_subgroup_size") && u32(profile.at("host_subgroup_size")) != size)
        throw std::runtime_error("host subgroup declarations disagree");
    return size;
}

json assess_spirv_host(Bytes module, const json &input_host) {
    const auto host = normalize_host_profile(input_host);
    if (module.size() < 20 || module.size() > 64 * 1024 * 1024 || module.size() % 4 ||
        integer(module, 0, 4) != 0x07230203)
        throw std::runtime_error("invalid SPIR-V inventory envelope");
    auto word = [&](size_t i) { return uint32_t(integer(module, i * 4, 4)); };
    const auto version = word(1);
    std::set<uint32_t> caps;
    std::set<std::string> extensions, entry_stages;
    struct Mode {
        uint32_t entry, mode;
        std::vector<uint32_t> args;
    };
    std::vector<Mode> modes;
    unsigned entries = 0;
    for (size_t i = 5; i < module.size() / 4;) {
        const auto count = word(i) >> 16, op = word(i) & 65535;
        if (!count || count > module.size() / 4 - i)
            throw std::runtime_error("truncated SPIR-V instruction");
        if (op == 17) {
            if (count != 2 || caps.size() >= 256)
                throw std::runtime_error("invalid capability inventory");
            caps.insert(word(i + 1));
        } else if (op == 10) {
            std::string name;
            bool end = false;
            for (size_t b = (i + 1) * 4; b < (i + count) * 4; ++b) {
                if (!module[b]) {
                    end = true;
                    break;
                }
                if (module[b] < 32 || module[b] > 126 || name.size() >= 128)
                    throw std::runtime_error("invalid SPIR-V extension string");
                name += char(module[b]);
            }
            if (!end || name.empty() || extensions.size() >= 128)
                throw std::runtime_error("invalid extension inventory");
            extensions.insert(name);
        } else if (op == 15) {
            if (count < 4 || ++entries > 128)
                throw std::runtime_error("invalid entry point inventory");
            entry_stages.insert(stage(word(i + 1)));
        } else if (op == 16 || op == 331) {
            if (count < 3 || modes.size() >= 256)
                throw std::runtime_error("invalid execution mode inventory");
            Mode m{word(i + 1), op == 331 ? UINT32_MAX : word(i + 2), {}};
            for (size_t a = i + 3; a < i + count; ++a)
                m.args.push_back(word(a));
            modes.push_back(std::move(m));
        }
        i += count;
    }
    json checks = json::array();
    auto add = [&](const std::string &requirement, const std::string &state,
                   const json &evidence = nullptr) {
        checks.push_back({{"requirement", requirement}, {"state", state}, {"declared", evidence}});
    };
    auto boolean = [&](const std::string &category, const std::string &name) {
        if (host.is_null() || !host.contains(category) || !host.at(category).contains(name)) {
            add(category + "." + name, "unknown");
            return;
        }
        const auto &v = host.at(category).at(name);
        add(category + "." + name, v.get<bool>() ? "satisfied" : "unsupported", v);
    };
    auto member = [&](const std::string &category, const std::string &name) {
        if (host.is_null() || !host.contains(category)) {
            add(category + "." + name, "unknown");
            return;
        }
        const auto &values = host.at(category);
        const bool found = std::find(values.begin(), values.end(), json(name)) != values.end();
        add(category + "." + name, found ? "satisfied" : "unsupported", values);
    };
    add("spirv_version_at_most_1.6",
        version >= 0x10000 && version <= 0x10600 && !(version & 255) ? "satisfied" : "unsupported",
        version);
    if (entries != 1)
        add("single_entry_point", "unknown");
    for (const auto &s : entry_stages)
        if (!stages.contains(s))
            add("unmodeled_entry_stage:" + s, "unknown");
    const std::map<uint32_t, std::string> feature_caps = {{2, "geometryShader"},
                                                          {3, "tessellationShader"},
                                                          {10, "shaderFloat64"},
                                                          {11, "shaderInt64"},
                                                          {25, "shaderImageGatherExtended"},
                                                          {32, "shaderClipDistance"},
                                                          {33, "shaderCullDistance"},
                                                          {35, "sampleRateShading"},
                                                          {52, "sampleRateShading"},
                                                          {69, "shaderOutputLayer"},
                                                          {70, "shaderOutputViewportIndex"},
                                                          {4428, "workgroupMemoryExplicitLayout"},
                                                          {5283, "meshShader"},
                                                          {5284, "fragmentShaderBarycentric"},
                                                          {5288, "computeDerivativeGroupQuads"},
                                                          {5347, "bufferDeviceAddress"},
                                                          {5350, "computeDerivativeGroupLinear"}};
    const std::array<const char *, 8> subgroup_ops = {"basic",     "vote",    "arithmetic",
                                                      "ballot",    "shuffle", "shuffle_relative",
                                                      "clustered", "quad"};
    for (const auto cap : caps) {
        if (feature_caps.contains(cap))
            boolean("enabled_features", feature_caps.at(cap));
        else if (cap >= 61 && cap <= 68) {
            member("subgroup_operations", subgroup_ops[cap - 61]);
            for (const auto &s : entry_stages)
                member("subgroup_stages", s);
            if (cap == 68 && entry_stages != std::set<std::string>{"CS"} &&
                entry_stages != std::set<std::string>{"PS"})
                add("quadOperationsInAllStages", "unknown");
        } else if (cap >= 4464 && cap <= 4468) {
            // The declaration needs at least one supported width; execution modes below
            // impose the actual widths. Omitted alternatives remain unknown, not false.
            bool supported = false, missing = false;
            const auto &prefix = float_modes.at(cap - 5);
            for (auto width : {16, 32, 64}) {
                const auto name = prefix + std::to_string(width);
                if (host.is_null() || !host.contains("properties") ||
                    !host.at("properties").contains(name))
                    missing = true;
                else
                    supported |= host.at("properties").at(name).get<bool>();
            }
            add("float_control_capability:" + std::to_string(cap), supported ? "satisfied"
                                                                   : missing ? "unknown"
                                                                             : "unsupported");
        } else if (cap == 12) {
            // Capability alone cannot tell which atomic storage class must be enabled.
            add("Int64Atomics_storage_class_usage", "unknown");
        } else if (cap == 55 || cap == 56) {
            const auto name = cap == 55 ? "shaderStorageImageReadWithoutFormat"
                                        : "shaderStorageImageWriteWithoutFormat";
            if (!host.is_null() && host.contains("enabled_features") &&
                host.at("enabled_features").value(name, false))
                boolean("enabled_features", name);
            else
                add("storage_image_format_features:" + std::string(name), "unknown");
        } else if (!std::set<uint32_t>{0, 1, 40, 43, 44, 46, 47, 49, 50, 51}.contains(cap))
            add("unmodeled_capability:" + std::to_string(cap), "unknown");
    }
    const std::map<std::string, std::string> extension_map = {
        {"SPV_KHR_workgroup_memory_explicit_layout", "VK_KHR_workgroup_memory_explicit_layout"},
        {"SPV_EXT_mesh_shader", "VK_EXT_mesh_shader"},
        {"SPV_KHR_fragment_shader_barycentric", "VK_KHR_fragment_shader_barycentric"},
        {"SPV_KHR_compute_shader_derivatives", "VK_KHR_compute_shader_derivatives"}};
    for (const auto &ext : extensions) {
        if (extension_map.contains(ext))
            member("enabled_extensions", extension_map.at(ext));
        else if (ext != "SPV_KHR_float_controls" && ext != "SPV_KHR_physical_storage_buffer" &&
                 ext != "SPV_KHR_storage_buffer_storage_class")
            add("unmodeled_spirv_extension:" + ext, "unknown");
    }
    if (!host.is_null() && host.contains("enabled_extensions"))
        for (const auto &ext : host.at("enabled_extensions"))
            if (ext == "VK_KHR_portability_subset")
                add("portability_subset_restrictions", "unknown");
    bool local_size = false;
    for (const auto &m : modes) {
        if (float_modes.contains(m.mode)) {
            if (m.args.size() != 1 || (m.args[0] != 16 && m.args[0] != 32 && m.args[0] != 64))
                throw std::runtime_error("invalid float-control execution mode");
            boolean("properties", float_modes.at(m.mode) + std::to_string(m.args[0]));
        } else if (m.mode == 17) {
            if (m.args.size() != 3 || !m.args[0] || !m.args[1] || !m.args[2])
                throw std::runtime_error("invalid LocalSize");
            local_size = true;
            const auto limits =
                host.is_null() ? json::object() : host.value("limits", json::object());
            if (entry_stages != std::set<std::string>{"CS"}) {
                add("noncompute_workgroup_limits", "unknown");
                continue;
            }
            if (!limits.contains("maxComputeWorkGroupSize"))
                add("maxComputeWorkGroupSize", "unknown");
            else {
                bool fits = true;
                for (size_t axis = 0; axis < 3; ++axis)
                    fits &= m.args[axis] <= u32(limits.at("maxComputeWorkGroupSize")[axis]);
                add("maxComputeWorkGroupSize", fits ? "satisfied" : "unsupported",
                    limits.at("maxComputeWorkGroupSize"));
            }
            if (!limits.contains("maxComputeWorkGroupInvocations"))
                add("maxComputeWorkGroupInvocations", "unknown");
            else {
                uint64_t remaining = u32(limits.at("maxComputeWorkGroupInvocations"));
                for (auto dim : m.args)
                    remaining /= dim;
                add("maxComputeWorkGroupInvocations", remaining ? "satisfied" : "unsupported",
                    limits.at("maxComputeWorkGroupInvocations"));
            }
        } else if (!std::set<uint32_t>{1, 7, 9, 12, 21, 22, 26, 27, 28, 29, 5269, 5270, 5289, 5298}
                        .contains(m.mode))
            add("unmodeled_execution_mode:" + std::to_string(m.mode), "unknown");
    }
    if (entry_stages.contains("CS") && !local_size)
        add("resolved_compute_local_size", "unknown");
    bool unsupported = false, unknown = false;
    for (const auto &check : checks) {
        unsupported |= check.at("state") == "unsupported";
        unknown |= check.at("state") == "unknown";
    }
    return {{"schema", 1},
            {"scope", "declared_spirv_requirements_only"},
            {"status", host.is_null() ? "not_supplied"
                       : unsupported  ? "unsupported"
                       : unknown      ? "unknown"
                                      : "satisfied"},
            {"runtime_compatibility", "not_established"},
            {"profile_provenance", "user_declared_not_queried"},
            {"spirv_sha256", sha256(module)},
            {"profile", host},
            {"capabilities", caps},
            {"spirv_extensions", extensions},
            {"entry_stages", entry_stages},
            {"checks", checks},
            {"not_assessed",
             {"resource formats and layouts", "atomic storage-class usage",
              "pipeline state and limits", "float-control independence rules",
              "actual subgroup-size enforcement", "driver behavior and shader semantics"}}};
}
} // namespace sl
