#include "shader_lab/replay_plan.hpp"
using namespace sl;
unsigned replay_plan_tests() {
    unsigned checks = 0;
    auto test = [&](bool ok, const char *message) {
        if (!ok)
            throw std::runtime_error(message);
        ++checks;
    };
    ExecutionInputs inputs;
    inputs.resources = {
        {"capture",
         {{"kind", "buffer"}, {"access", "read_write"}, {"guest_address", "0000000000001000"}}}};
    inputs.resource_bytes["capture"] = std::vector<uint8_t>(2048);
    auto buffer = [](const char *base, uint64_t size) {
        return json{{"guest_address", base},
                    {"descriptor_size_bytes", size},
                    {"image_alias", nullptr},
                    {"read", true},
                    {"written", true}};
    };
    json layout = {
        {"shader_data",
         {{"words", {9, nullptr}},
          {"dwords", 2},
          {"memory_offset_dword", 1},
          {"memory_offset_count", 2}}},
        {"buffers", {buffer("0x1004", 32), buffer("0x1108", 64)}},
        {"descriptors", {{{"kind", 0}, {"resource_indices", {1, 0}}, {"descriptor_count", 2}}}},
        {"buffer_offsets",
         {{{"array_element", 0},
           {"resource", 1},
           {"shader_data_dword", 1},
           {"bit_offset", 0},
           {"bit_width", 8},
           {"value", nullptr}},
          {{"array_element", 1},
           {"resource", 0},
           {"shader_data_dword", 1},
           {"bit_offset", 8},
           {"bit_width", 8},
           {"value", nullptr}}}}};
    const auto plan = plan_replay_buffers(inputs, layout, 256, 1024);
    test(plan.bindings.size() == 2 && plan.bindings[0].resource == 1 &&
             plan.bindings[1].resource == 0,
         "compiler buffer array order retained");
    test(plan.bindings[0].fixture_resource == "capture" &&
             plan.bindings[1].fixture_resource == "capture",
         "interior descriptors preserve same-capture aliasing");
    test(plan.bindings[0].descriptor_offset == 256 && plan.bindings[0].byte_adjustment == 8 &&
             plan.bindings[0].descriptor_range == 72,
         "aligned descriptor offset and prefix range correct");
    test(plan.bindings[1].descriptor_offset == 0 && plan.bindings[1].byte_adjustment == 4 &&
             plan.bindings[1].descriptor_range == 36,
         "near-base interior descriptor correct");
    test(plan.shader_data == std::vector<uint32_t>({9, 0x408}),
         "live user data preserved and two offset bytes packed");
    const auto aligned = plan_replay_buffers(inputs, layout, 4, 1024);
    test(aligned.bindings[0].descriptor_offset == 264 && aligned.bindings[0].byte_adjustment == 0 &&
             aligned.shader_data[1] == 0,
         "small host alignment uses native descriptor offset without adjustment");
    auto reject = [&](const ExecutionInputs &in, const json &l, uint64_t alignment = 256,
                      uint64_t range = 1024) {
        bool threw = false;
        try {
            plan_replay_buffers(in, l, alignment, range);
        } catch (const std::exception &) {
            threw = true;
        }
        test(threw, "invalid replay range/offset must be rejected");
    };
    reject(inputs, layout, 3);
    reject(inputs, layout, 512);     // Delta 264 cannot fit in Kyty's eight-bit correction.
    reject(inputs, layout, 256, 64); // Guest range fits 64 but the host prefix makes it 72.
    auto modified = layout;
    modified["buffers"][0]["guest_address"] = "0x17fc";
    reject(inputs, modified);
    modified = layout;
    modified["buffers"][0]["guest_address"] = "0xfffffffffffffff0";
    reject(inputs, modified);
    modified = layout;
    modified["buffer_offsets"][1]["resource"] = 1;
    reject(inputs, modified);
    modified = layout;
    modified["buffer_offsets"][1]["bit_offset"] = 32;
    reject(inputs, modified);
    auto permissions = inputs;
    permissions.resources["capture"]["access"] = "read_only";
    reject(permissions, layout);
    permissions = inputs;
    permissions.resources["capture"]["access"] = "write_only";
    reject(permissions, layout);
    return checks;
}
