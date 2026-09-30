#pragma once
#include "shader_lab/lab.hpp"
#include <stdexcept>
namespace sl {
struct VulkanReplayFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct VulkanReplayResult {
    std::map<std::string, std::vector<uint8_t>> outputs;
    json trace;
};
// Must only be called after explicit GPU opt-in in an isolated worker process.
VulkanReplayResult vulkan_replay(const ExecutionInputs &inputs, Bytes spirv, const json &layout);
} // namespace sl
