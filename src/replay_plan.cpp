#include "shader_lab/replay_plan.hpp"
#include <stdexcept>
namespace sl {
namespace {
void require(bool condition, const char *reason) {
    if (!condition)
        throw std::runtime_error(reason);
}
uint64_t address(const json &value) {
    const auto text = value.get<std::string>();
    size_t end = 0;
    const auto result = std::stoull(text, &end, 16);
    require(end == text.size(), "invalid replay resource address");
    return result;
}
} // namespace
ReplayBufferPlan plan_replay_buffers(const ExecutionInputs &inputs, const json &layout,
                                     uint64_t alignment, uint64_t max_range) {
    require(alignment && !(alignment & (alignment - 1)) && max_range,
            "invalid storage-buffer alignment or range limit");
    ReplayBufferPlan result;
    const auto &data = layout.at("shader_data");
    const auto &words = data.at("words");
    const auto offset_word = data.at("memory_offset_dword").get<uint32_t>();
    const auto offset_count = data.at("memory_offset_count").get<uint32_t>();
    require(words.is_array() && words.size() <= 272 && offset_count <= 64 &&
                offset_word <= words.size() &&
                words.size() == uint64_t(offset_word) + (offset_count + 3u) / 4u &&
                data.at("dwords") == words.size(),
            "invalid replay shader-data extent");
    for (size_t i = 0; i < words.size(); ++i) {
        require(i < offset_word ? words[i].is_number_unsigned() || words[i].is_number_integer()
                                : words[i].is_null(),
                "unexpected replay shader-data template word");
        if (i < offset_word) {
            require(words[i].get<int64_t>() >= 0 && words[i].get<uint64_t>() <= UINT32_MAX,
                    "user-data word is outside u32 range");
            result.shader_data.push_back(words[i].get<uint32_t>());
        } else
            result.shader_data.push_back(0);
    }
    require(layout.at("buffer_offsets").size() == offset_count, "buffer offset count mismatch");
    for (const auto &binding : layout.at("descriptors")) {
        if (binding.at("kind") != 0)
            continue;
        require(result.bindings.empty() && binding.at("resource_indices").size() == offset_count &&
                    binding.at("descriptor_count") == offset_count && offset_count != 0,
                "invalid live buffer binding array");
        for (const auto &index : binding.at("resource_indices")) {
            ReplayBufferBinding next;
            next.resource = index.get<uint32_t>();
            next.array_element = static_cast<uint32_t>(result.bindings.size());
            const auto &buffer = layout.at("buffers").at(next.resource);
            require(buffer.at("image_alias").is_null(),
                    "image-buffer aliases require image replay");
            const auto start = address(buffer.at("guest_address"));
            const auto extent = buffer.at("descriptor_size_bytes").get<uint64_t>();
            require(extent && extent <= UINT64_MAX - start, "invalid replay descriptor extent");
            bool found = false;
            for (auto it = inputs.resources.begin(); it != inputs.resources.end(); ++it) {
                if (it.value().at("kind") != "buffer")
                    continue;
                const auto base = address(it.value().at("guest_address"));
                const auto captured = inputs.resource_bytes.at(it.key()).size();
                if (start < base || start - base > captured || extent > captured - (start - base))
                    continue;
                require(!found, "ambiguous captured buffer range");
                require(
                    (!buffer.at("written").get<bool>() || it.value().at("access") != "read_only") &&
                        (!buffer.at("read").get<bool>() || it.value().at("access") != "write_only"),
                    "compiler access disagrees with fixture permissions");
                const auto delta = start - base;
                next.descriptor_offset = delta & ~(alignment - 1);
                const auto adjustment = delta - next.descriptor_offset;
                require(adjustment <= 255 && extent <= max_range &&
                            adjustment <= max_range - extent,
                        "captured subrange exceeds packed offset or storage-buffer range limits");
                next.byte_adjustment = static_cast<uint32_t>(adjustment);
                next.descriptor_range = extent + adjustment;
                next.fixture_resource = it.key();
                found = true;
            }
            require(found, "live descriptor range is not wholly backed by one captured buffer");
            const auto &slot = layout.at("buffer_offsets").at(next.array_element);
            const auto word = offset_word + next.array_element / 4;
            const auto shift = (next.array_element % 4) * 8;
            require(slot.at("array_element") == next.array_element &&
                        slot.at("resource") == next.resource &&
                        slot.at("shader_data_dword") == word && slot.at("bit_offset") == shift &&
                        slot.at("bit_width") == 8 && slot.at("value").is_null(),
                    "compiler buffer-offset slot mismatch");
            result.shader_data.at(word) |= next.byte_adjustment << shift;
            result.bindings.push_back(std::move(next));
        }
    }
    require(result.bindings.size() == offset_count, "missing live buffer binding array");
    return result;
}
} // namespace sl
