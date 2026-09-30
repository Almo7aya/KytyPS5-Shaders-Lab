#pragma once
#include "shader_lab/lab.hpp"
namespace sl {
struct ReplayBufferBinding {
    uint32_t resource = 0, array_element = 0, byte_adjustment = 0;
    std::string fixture_resource;
    uint64_t descriptor_offset = 0, descriptor_range = 0;
};
struct ReplayBufferPlan {
    std::vector<ReplayBufferBinding> bindings;
    std::vector<uint32_t> shader_data;
};
// Pure planning: no device access. Inputs must be validated execution snapshots.
ReplayBufferPlan plan_replay_buffers(const ExecutionInputs &inputs, const json &layout,
                                     uint64_t alignment, uint64_t max_range);
} // namespace sl
