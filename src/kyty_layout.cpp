// GPL-2.0-only: export actual post-specialization Kyty bindings; never create runtime resources.
#include "kyty_layout.hpp"
#include "graphics/shader/recompiler/ir/ResourceSnapshot.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shaderBindings.h"
#include <algorithm>
namespace sl {
namespace ir = Libs::Graphics::ShaderRecompiler::IR;
namespace {
json descriptor(const ir::DescriptorValue &value, uint32_t expected) {
    if (value.dword_count != expected || expected > value.dwords.size())
        throw std::runtime_error("unexpected materialized descriptor width");
    return std::vector<uint32_t>(value.dwords.begin(), value.dwords.begin() + expected);
}
void bounded(size_t size, size_t limit) {
    if (size > limit)
        throw std::runtime_error("compiler layout exceeds export budget");
}
} // namespace
json compiler_layout(const ir::Program &p, const ir::ResourceSnapshot &s) {
    if (!p.shader_info_complete || !p.binding_layout_complete)
        throw std::runtime_error("compiler layout is not finalized");
    bounded(p.info.buffers.size(), 64);
    bounded(p.info.images.size(), 64);
    bounded(p.info.samplers.size(), 32);
    bounded(s.flattened_srt.size(), 1024 * 1024);
    const auto &layout = p.bindings;
    bounded(layout.descriptors.size(), 64);
    bounded(layout.user_data_registers.size(), 256);
    bounded(layout.memory_offset_count, 64);
    bounded(layout.memory_offset_dword, 256);
    bounded(layout.ShaderDataDwords(), 272);
    if (s.buffers.size() != p.info.buffers.size() || s.images.size() != p.info.images.size() ||
        s.samplers.size() != p.info.samplers.size() ||
        layout.memory_offset_dword != layout.user_data_registers.size())
        throw std::runtime_error("compiler layout and materialized snapshot disagree");
    json result = {{"schema", 1},
                   {"status", "exported"},
                   {"scope", "post_specialization_compiler_layout"},
                   {"runtime_bindings", "not_created"},
                   {"semantic_correctness", "not_tested"},
                   {"wave_size", p.wave_size},
                   {"scratch_dwords", p.scratch_dwords},
                   {"user_data_base", p.user_data_base},
                   {"uses_dma", p.info.uses_dma},
                   {"descriptors", json::array()},
                   {"buffers", json::array()},
                   {"images", json::array()},
                   {"samplers", json::array()},
                   {"flattened_srt", s.flattened_srt}};
    uint32_t buffer_elements = 0;
    for (const auto &b : layout.descriptors) {
        using Kind = ir::DescriptorBindingKind;
        const auto kind = static_cast<uint32_t>(b.kind);
        bounded(b.resources.size(), 4096);
        const bool image =
            kind >= ir::FirstImageBinding && kind < ir::FirstImageBinding + ir::ImageBindingCount;
        const bool array = image || b.kind == Kind::Buffers || b.kind == Kind::Samplers;
        if (kind >= static_cast<uint32_t>(Kind::Count) || (array && b.resources.empty()) ||
            (!array && !b.resources.empty()))
            throw std::runtime_error("unexpected descriptor binding shape");
        const char *type =
            image ? (kind < ir::FirstStorageImageBinding ? "sampled_image" : "storage_image")
            : b.kind == Kind::Samplers ? "sampler"
                                       : "storage_buffer";
        const size_t count = b.kind == Kind::Buffers    ? p.info.buffers.size()
                             : b.kind == Kind::Samplers ? p.info.samplers.size()
                                                        : p.info.images.size();
        for (auto index : b.resources)
            if (index >= count)
                throw std::runtime_error("descriptor resource index out of range");
        result["descriptors"].push_back({{"set", 0},
                                         {"binding", ir::NativeBinding(p.stage, b.kind)},
                                         {"kind", kind},
                                         {"descriptor_type", type},
                                         {"descriptor_count", array ? b.resources.size() : 1},
                                         {"resource_indices", b.resources}});
        if (b.kind == Kind::Buffers) {
            if (buffer_elements || b.resources.size() != layout.memory_offset_count)
                throw std::runtime_error("buffer offset layout mismatch");
            buffer_elements = static_cast<uint32_t>(b.resources.size());
            result["buffer_offsets"] = json::array();
            for (uint32_t i = 0; i < buffer_elements; ++i)
                result["buffer_offsets"].push_back(
                    {{"array_element", i},
                     {"resource", b.resources[i]},
                     {"shader_data_dword", layout.memory_offset_dword + i / 4},
                     {"bit_offset", (i % 4) * 8},
                     {"bit_width", 8},
                     {"value", nullptr}});
        }
    }
    if (buffer_elements != layout.memory_offset_count)
        throw std::runtime_error("missing live buffer descriptor binding");
    if (!result.contains("buffer_offsets"))
        result["buffer_offsets"] = json::array();
    for (size_t i = 0; i < p.info.buffers.size(); ++i) {
        const auto &r = p.info.buffers[i];
        auto raw = descriptor(s.buffers[i], 4);
        Libs::Graphics::ShaderBufferResource native{};
        std::copy_n(s.buffers[i].dwords.begin(), 4, native.fields);
        result["buffers"].push_back(
            {{"resource", i},
             {"source", r.source},
             {"first_use_pc", r.first_use_pc},
             {"descriptor_words", raw},
             {"guest_address", "0x" + hex(native.Base48(), 12)},
             {"descriptor_size_bytes", native.GetSize()},
             {"stride", native.Stride()},
             {"num_records", native.NumRecords()},
             {"packed_stride", r.packed_stride},
             {"descriptor_format", static_cast<uint32_t>(r.descriptor_format)},
             {"descriptor_swizzle", r.descriptor_swizzle},
             {"out_of_bounds", native.OutOfBounds()},
             {"max_byte_extent", r.max_byte_extent},
             {"read", r.read},
             {"written", r.written},
             {"atomic", r.atomic},
             {"formatted", r.formatted},
             {"scalar", r.scalar},
             {"image_alias", r.image_alias == ir::BufferResource::NoImageAlias
                                 ? json(nullptr)
                                 : json(r.image_alias)}});
    }
    for (size_t i = 0; i < p.info.images.size(); ++i) {
        const auto &r = p.info.images[i];
        bounded(r.indirect_resources.size(), 4096);
        result["images"].push_back(
            {{"resource", i},
             {"source", r.source},
             {"first_use_pc", r.first_use_pc},
             {"descriptor_words", descriptor(s.images[i], 8)},
             {"resource_class", static_cast<uint32_t>(r.resource_class)},
             {"numeric_class", static_cast<uint32_t>(r.numeric_class)},
             {"dimension", static_cast<uint32_t>(r.dimension)},
             {"mip_mode", static_cast<uint32_t>(r.mip_mode)},
             {"mip_count", r.mip_count},
             {"conversion_format", static_cast<uint32_t>(r.conversion_format)},
             {"shader_swizzle", r.shader_swizzle},
             {"read", r.read},
             {"written", r.written},
             {"atomic", r.atomic},
             {"depth_compare", r.depth_compare},
             {"cube", r.cube},
             {"r128", r.r128},
             {"indirect_root", r.indirect_root == ir::ImageResource::NoIndirectImage
                                   ? json(nullptr)
                                   : json(r.indirect_root)},
             {"indirect_mapping_offset", r.indirect_mapping_offset},
             {"indirect_search_iterations", r.indirect_search_iterations},
             {"indirect_resources", r.indirect_resources}});
    }
    for (size_t i = 0; i < p.info.samplers.size(); ++i) {
        const auto &r = p.info.samplers[i];
        result["samplers"].push_back({{"resource", i},
                                      {"source", r.source},
                                      {"first_use_pc", r.first_use_pc},
                                      {"descriptor_words", descriptor(s.samplers[i], 4)},
                                      {"force_point_filtering", r.force_point_filtering},
                                      {"depth_compare", r.depth_compare},
                                      {"integer_border", r.integer_border}});
    }
    json words = json::array(), registers = json::array();
    for (auto reg : layout.user_data_registers) {
        if (reg < p.user_data_base || reg - p.user_data_base >= s.user_data.size())
            throw std::runtime_error("live user-data register absent from snapshot");
        registers.push_back({{"register", reg},
                             {"shader_data_dword", words.size()},
                             {"value", s.user_data[reg - p.user_data_base]}});
        words.push_back(s.user_data[reg - p.user_data_base]);
    }
    while (words.size() < layout.ShaderDataDwords())
        words.push_back(
            nullptr); // Runtime-selected buffer offsets must not become fabricated zeros.
    result["shader_data"] = {{"dwords", layout.ShaderDataDwords()},
                             {"words", words},
                             {"user_data_registers", registers},
                             {"memory_offset_dword", layout.memory_offset_dword},
                             {"memory_offset_count", layout.memory_offset_count},
                             {"location", layout.UsesPushData() ? "push_constants"
                                          : words.empty()       ? "none"
                                                                : "storage_buffer"},
                             {"push_data_start_dword", layout.UsesPushData()
                                                           ? json(layout.push_data_start_dword)
                                                           : json(nullptr)},
                             {"native_push_constant_size_bytes", ir::NativePushConstantSize}};
    return result;
}
} // namespace sl
