#pragma once
#include "shader_lab/lab.hpp"

namespace sl {
struct CpuReferenceResult {
    std::map<std::string, std::vector<uint8_t>> outputs;
    json trace;
};
// Independent, bounded RDNA2 integer model. Unsupported state/instructions throw;
// partially modified resources are never returned as completed execution.
CpuReferenceResult cpu_reference(ExecutionInputs inputs);
int cpu_reference_worker(const fs::path &request);
} // namespace sl
