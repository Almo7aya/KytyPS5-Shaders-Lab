#include "shader_lab/compiler_trace.hpp"
#include <array>

namespace sl {
json compiler_pass_catalog() {
    constexpr std::array names = {"translation_input",
                                  "frontend_translate",
                                  "rewrite_ssa",
                                  "constant_propagation",
                                  "resolve_control_flow_identities",
                                  "remove_identities",
                                  "dead_code_elimination",
                                  "read_lane_elimination",
                                  "conditional_read_lane_cleanup",
                                  "lower_tessellation_memory",
                                  "track_resources",
                                  "resource_tracking_dead_code_elimination",
                                  "extract_resource_plan",
                                  "materialize_resources",
                                  "apply_resource_specialization",
                                  "prune_descriptor_dependencies",
                                  "specialization_remove_identities",
                                  "specialization_dead_code_elimination",
                                  "collect_shader_info",
                                  "allocate_bindings",
                                  "emit_spirv"};
    json passes = json::array();
    for (size_t i = 0; i < names.size(); ++i)
        passes.push_back({{"index", i}, {"name", names[i]}});
    return {{"schema", 1},
            {"passes", passes},
            {"predicate", "checkpoint_reached_not_semantic_correctness"},
            {"note", "Conditional read-lane cleanup is one grouped boundary. Decode/CFG setup "
                     "precede frontend translation; SPIR-V validation follows emission."}};
}
} // namespace sl
