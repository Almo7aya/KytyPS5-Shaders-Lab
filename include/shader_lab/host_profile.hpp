#pragma once
#include "shader_lab/lab.hpp"

namespace sl {
// Host declarations are supplied evidence, not a Vulkan device query.
json normalize_host_profile(const json &host);
uint32_t compiler_host_subgroup(const json &profile);
// Inventory/check declared module requirements only. Does not validate SPIR-V,
// resource formats, pipelines, drivers, or semantic execution compatibility.
json assess_spirv_host(Bytes module, const json &host);
} // namespace sl
