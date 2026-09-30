#include "shader_lab/lab.hpp"
#include <algorithm>
#include <cstring>
#include <set>
#ifdef SL_HAVE_ZSTD
#include <zstd.h>
#endif
namespace sl {
namespace {
// Shared across the entire input tree, not reset for each nested frame. Charge
// every expanded layer, including intermediate containers, before allocating.
struct ContainerBudget {
    uint64_t expanded = 0;
    size_t frames = 0;
};
Extraction extract_layer(Bytes bytes, size_t limit, [[maybe_unused]] ContainerBudget &budget,
                         [[maybe_unused]] unsigned depth) {
    auto out = extract(bytes, limit);
#ifdef SL_HAVE_ZSTD
    // Scan independently framed Zstandard data. Unknown-size/dictionary streams
    // remain coverage gaps; never allocate from an unchecked declared size.
    for (size_t pos = 0; pos + 4 <= bytes.size();) {
        auto p = static_cast<const uint8_t *>(
            std::memchr(bytes.data() + pos, 0x28, bytes.size() - pos - 3));
        if (!p)
            break;
        pos = size_t(p - bytes.data());
        if (integer(bytes, pos, 4) != 0xfd2fb528) {
            ++pos;
            continue;
        }
        if (++budget.frames > 4096) {
            out.findings.push_back({{"kind", "container_member_budget"}, {"offset", pos},
                                    {"max_members", 4096}});
            out.limited = true;
            break;
        }
        auto frame = bytes.subspan(pos);
        auto packed = ZSTD_findFrameCompressedSize(frame.data(), frame.size());
        auto size = ZSTD_getFrameContentSize(frame.data(), frame.size());
        if (ZSTD_isError(packed)) {
            ++pos;
            continue;
        }
        if (depth >= 4) {
            out.findings.push_back({{"kind", "container_depth_budget"}, {"offset", pos},
                                    {"max_depth", 4}, {"adapter", "zstd/1"}});
            out.limited = true;
            pos += packed;
            continue;
        }
        if (size == ZSTD_CONTENTSIZE_UNKNOWN || size == ZSTD_CONTENTSIZE_ERROR ||
            size > 256 * 1024 * 1024 || size > 512 * 1024 * 1024 - budget.expanded) {
            out.findings.push_back({{"kind", "zstd_size_limit_or_unknown"}, {"offset", pos}});
            out.limited = true;
            pos += packed;
            continue;
        }
        budget.expanded += size;
        std::vector<uint8_t> decoded(size_t(size), 0);
        auto actual = ZSTD_decompress(decoded.data(), decoded.size(), frame.data(), packed);
        if (ZSTD_isError(actual) || actual != size) {
            out.findings.push_back(
                {{"kind", "zstd_decompression_failed"},
                 {"offset", pos},
                 {"detail", ZSTD_isError(actual) ? ZSTD_getErrorName(actual) : "size mismatch"}});
            pos += packed;
            continue;
        }
        auto inner =
            extract_layer(decoded, limit > out.candidates.size() ? limit - out.candidates.size() : 0,
                          budget, depth + 1);
        const auto frame_space = "zstd_frame@0x" + hex(pos);
        for (auto &c : inner.candidates) {
            c.offset_space = frame_space + "/" + c.offset_space;
            c.method = "zstd/" + c.method;
            c.evidence.push_back(
                {{"adapter", "zstd/1"}, {"frame_offset", pos}, {"depth", depth},
                 {"compressed_bytes", packed}, {"expanded_bytes", size}});
            out.candidates.push_back(std::move(c));
        }
        for (auto &f : inner.findings) {
            f["offset_space"] = frame_space + "/" + f.value("offset_space", "file");
            out.findings.push_back(std::move(f));
        }
        out.limited = out.limited || inner.limited;
        out.findings.push_back(
            {{"kind", "zstd_frame_scanned"}, {"adapter", "zstd/1"}, {"offset", pos},
             {"expanded_bytes", size}, {"depth", depth}});
        pos += packed;
    }
#endif
    if (bytes.size() < 32)
        return out;
    auto magic = integer(bytes, 0, 4);
    if (magic != 0x1d3d154f && magic != 0xeef51454)
        return out;
    // This is segment normalization, not decryption or executable loading.
    // Source: KytyPS5 loader/elf.{h,cpp}; corroborated by ps5rs SELF constants.
    try {
        const auto count = integer(bytes, 24, 2), elf = 32 + count * 32;
        if (count > 4096 || !contains(bytes, 32, count * 32) || !contains(bytes, elf, 64))
            throw std::runtime_error("truncated SELF table");
        auto e = bytes.subspan(size_t(elf));
        if (integer(e, 0, 4) != 0x464c457f || e[4] != 2 || e[5] != 1)
            throw std::runtime_error("unsupported embedded ELF encoding");
        auto phoff = integer(e, 32, 8), stride = integer(e, 54, 2), phnum = integer(e, 56, 2);
        if (stride < 56 || phnum > 4096 || !contains(e, phoff, phnum * stride))
            throw std::runtime_error("invalid embedded program-header table");
        struct Segment {
            uint64_t src, dst, size, vaddr;
        };
        std::vector<Segment> segments;
        std::set<uint64_t> seen;
        uint64_t extent = 0;
        for (uint64_t i = 0; i < count; ++i) {
            auto s = bytes.subspan(size_t(32 + i * 32), 32);
            auto flags = integer(s, 0, 8);
            if (!(flags & 0x800))
                continue;
            auto idx = (flags >> 20) & 0xfff;
            if (idx >= phnum)
                throw std::runtime_error("SELF segment references absent program header");
            auto ph = e.subspan(size_t(phoff + idx * stride), 56);
            if (integer(ph, 0, 4) != 1)
                continue;
            if (flags & 2) {
                out.findings.push_back({{"kind", "encrypted_self_segment"}, {"segment", i}});
                continue;
            }
            if (flags & 8) {
                out.findings.push_back(
                    {{"kind", "compressed_self_segment_unsupported"}, {"segment", i}});
                continue;
            }
            auto src = integer(s, 8, 8), packed = integer(s, 16, 8), unpacked = integer(s, 24, 8),
                 dst = integer(ph, 8, 8), size = integer(ph, 32, 8), va = integer(ph, 16, 8);
            if (!seen.insert(idx).second)
                throw std::runtime_error("duplicate SELF segment mapping");
            if (size > 256 * 1024 * 1024 || dst > 256 * 1024 * 1024 - size || packed != unpacked ||
                packed != size || !contains(bytes, src, size))
                throw std::runtime_error("invalid/unbounded SELF load segment extent");
            for (const auto &previous : segments)
                if (dst < previous.dst + previous.size && previous.dst < dst + size)
                    throw std::runtime_error("overlapping SELF load segments");
            segments.push_back({src, dst, size, va});
            extent = std::max(extent, dst + size);
        }
        if (!extent)
            return out;
        if (extent > 512 * 1024 * 1024 - budget.expanded) {
            out.findings.push_back({{"kind", "container_expansion_budget"},
                                    {"adapter", "self/1"}, {"expanded_bytes", extent}});
            out.limited = true;
            return out;
        }
        budget.expanded += extent;
        std::vector<uint8_t> image(size_t(extent), 0);
        for (const auto &s : segments)
            std::copy_n(bytes.begin() + std::ptrdiff_t(s.src), size_t(s.size),
                        image.begin() + std::ptrdiff_t(s.dst));
        auto extracted =
            extract(image, limit > out.candidates.size() ? limit - out.candidates.size() : 0);
        auto backed = [&](uint64_t start, uint64_t size) {
            for (const auto &s : segments)
                if (start >= s.dst && start - s.dst <= s.size && size <= s.size - (start - s.dst))
                    return true;
            return false;
        };
        for (auto &c : extracted.candidates) {
            if (!backed(c.header_offset, c.header.size()) || !backed(c.code_offset, c.code.size()))
                continue;
            c.offset_space = "reconstructed_elf_file";
            c.method = "self/" + c.method;
            sl::json map = sl::json::array();
            for (const auto &s : segments)
                if ((c.header_offset >= s.dst && c.header_offset < s.dst + s.size) ||
                    (c.code_offset >= s.dst && c.code_offset < s.dst + s.size))
                    map.push_back({{"source_offset", s.src},
                                   {"elf_offset", s.dst},
                                   {"virtual_address", s.vaddr},
                                   {"bytes", s.size}});
            c.evidence.push_back({{"self_segment_map", map}});
            out.candidates.push_back(std::move(c));
        }
        for (auto &finding : extracted.findings) {
            finding["offset_space"] = "reconstructed_elf_file";
            out.findings.push_back(std::move(finding));
        }
        out.limited = out.limited || extracted.limited;
        out.findings.push_back({{"kind", "self_normalization"},
                                {"clear_load_segments", segments.size()},
                                {"note", "only fully backed candidates retained; "
                                         "encrypted/compressed segments are not reconstructed"}});
    } catch (const std::exception &ex) {
        out.findings.push_back({{"kind", "rejected_self"}, {"detail", ex.what()}});
    }
    return out;
}
} // namespace
Extraction extract_containers(Bytes bytes, size_t limit) {
    ContainerBudget budget;
    return extract_layer(bytes, limit, budget, 0);
}
} // namespace sl
