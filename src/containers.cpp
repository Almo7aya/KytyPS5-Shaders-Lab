#include "shader_lab/lab.hpp"
#include <algorithm>
#include <cstring>
#include <set>
#include <optional>
#include <zlib.h>
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
    bool limited = false;
};
Extraction extract_layer(Bytes bytes, size_t limit, ContainerBudget &budget, unsigned depth);

// ZIP layout: PKWARE APPNOTE 6.3.10, sections 4.3.7, 4.3.12 and 4.3.16.
// Central-directory sizes allow streamed members (bit 3) without guessing where
// compressed bytes end. No archive path is ever used for filesystem writes.
std::optional<size_t> zip_end(Bytes bytes) {
    if (bytes.size() < 22) return {};
    const size_t first = bytes.size() > 65557 ? bytes.size() - 65557 : 0;
    for (size_t pos = bytes.size() - 22;; --pos) {
        if (integer(bytes, pos, 4) == 0x06054b50 &&
            integer(bytes, pos + 20, 2) == bytes.size() - pos - 22)
            return pos;
        if (pos == first) break;
    }
    return {};
}
std::string member_label(Bytes name) {
    std::string result;
    for (auto byte : name) {
        if (byte >= 32 && byte < 127 && byte != '\\') result += char(byte);
        else result += "\\x" + hex(byte, 2);
    }
    return result;
}
Extraction extract_zip(Bytes bytes, size_t end, size_t limit, ContainerBudget &budget, unsigned depth) {
    Extraction out;
    auto note = [&](const char *kind, uint64_t offset, const json &extra = json::object()) {
        json value = {{"kind", kind}, {"adapter", "zip/1"}, {"offset", offset}};
        value.update(extra);
        out.findings.push_back(std::move(value));
    };
    note("zip_archive_detected", end);
    try {
        if (integer(bytes, end + 4, 2) || integer(bytes, end + 6, 2) ||
            integer(bytes, end + 8, 2) != integer(bytes, end + 10, 2)) {
            note("unsupported_zip_multidisk", end);
            return out;
        }
        auto count = integer(bytes, end + 10, 2), length = integer(bytes, end + 12, 4),
             start = integer(bytes, end + 16, 4);
        if (count == 0xffff || length == UINT32_MAX || start == UINT32_MAX ||
            (end >= 20 && integer(bytes, end - 20, 4) == 0x07064b50)) {
            note("unsupported_zip64", end);
            return out;
        }
        if (!contains(bytes, start, length) || start > end || length != end - start)
            throw std::runtime_error("invalid ZIP central-directory extent");
        auto pos = start;
        std::vector<std::pair<uint64_t, uint64_t>> ranges;
        for (uint64_t member = 0; member < count; ++member) {
            if (++budget.frames > 4096) {
                note("container_member_budget", pos);
                out.limited = true;
                return out;
            }
            if (!contains(bytes, pos, 46) || pos + 46 > end || integer(bytes, pos, 4) != 0x02014b50)
                throw std::runtime_error("invalid ZIP central member header");
            auto flags = integer(bytes, pos + 8, 2), method = integer(bytes, pos + 10, 2),
                 crc = integer(bytes, pos + 16, 4), packed = integer(bytes, pos + 20, 4),
                 size = integer(bytes, pos + 24, 4), names = integer(bytes, pos + 28, 2),
                 extra = integer(bytes, pos + 30, 2), comment = integer(bytes, pos + 32, 2),
                 disk = integer(bytes, pos + 34, 2), local = integer(bytes, pos + 42, 4);
            auto next = pos + 46 + names + extra + comment;
            if (next > end) throw std::runtime_error("truncated ZIP central member metadata");
            const auto central = pos;
            pos = next;
            if (names > 4096) {
                note("zip_member_name_limit", central);
                out.limited = true;
                continue;
            }
            auto name_bytes = bytes.subspan(size_t(central + 46), size_t(names));
            json provenance = {{"member_index", member}, {"member_name", member_label(name_bytes)},
                               {"name_encoding", "ASCII with byte escapes"}, {"local_header_offset", local},
                               {"compressed_bytes", packed}, {"expanded_bytes", size}, {"method", method}};
            if (flags & (1 | 0x40 | 0x2000)) {
                note("encrypted_zip_member_unsupported", central, provenance);
                continue;
            }
            if (disk || packed == UINT32_MAX || size == UINT32_MAX || local == UINT32_MAX) {
                note("unsupported_zip64_or_multidisk_member", central, provenance);
                continue;
            }
            if ((flags & ~uint64_t(0x80e)) || (method != 0 && method != 8)) {
                note("unsupported_zip_method_or_flags", central, provenance);
                continue;
            }
            if (!contains(bytes, local, 30) || local + 30 > start || integer(bytes, local, 4) != 0x04034b50)
                throw std::runtime_error("invalid ZIP local member header");
            auto local_names = integer(bytes, local + 26, 2), local_extra = integer(bytes, local + 28, 2);
            auto data = local + 30 + local_names + local_extra;
            if (data > start || packed > start - data || local_names != names ||
                integer(bytes, local + 6, 2) != flags || integer(bytes, local + 8, 2) != method ||
                !std::equal(name_bytes.begin(), name_bytes.end(), bytes.begin() + size_t(local + 30)))
                throw std::runtime_error("ZIP local and central member metadata disagree");
            auto member_end = data + packed;
            if (flags & 8) {
                auto descriptor = member_end;
                // Signature is optional. The CRC itself can equal the signature,
                // so accept the unsigned form only if all three fields agree.
                auto valid = [&](uint64_t offset) {
                    return offset <= start && start - offset >= 12 &&
                           integer(bytes, offset, 4) == crc && integer(bytes, offset + 4, 4) == packed &&
                           integer(bytes, offset + 8, 4) == size;
                };
                if (valid(descriptor)) member_end += 12;
                else if (contains(bytes, descriptor, 4) && integer(bytes, descriptor, 4) == 0x08074b50 && valid(descriptor + 4))
                    member_end += 16;
                else throw std::runtime_error("invalid ZIP data descriptor");
            } else if (integer(bytes, local + 14, 4) != crc || integer(bytes, local + 18, 4) != packed ||
                       integer(bytes, local + 22, 4) != size) {
                throw std::runtime_error("ZIP local sizes or checksum disagree");
            }
            for (auto [begin, finish] : ranges)
                if (local < finish && begin < member_end)
                    throw std::runtime_error("overlapping ZIP members");
            ranges.emplace_back(local, member_end);
            provenance["data_offset"] = data;
            auto unix_type = (integer(bytes, central + 38, 4) >> 16) & 0xf000;
            if ((!name_bytes.empty() && name_bytes.back() == '/') ||
                (unix_type && unix_type != 0x8000)) {
                note("zip_non_regular_member_skipped", central, provenance);
                continue;
            }
            if (depth >= 4 || size > 256 * 1024 * 1024 || size > 512 * 1024 * 1024 - budget.expanded ||
                packed > 256 * 1024 * 1024 || out.candidates.size() >= limit) {
                note("zip_member_budget", central, provenance);
                out.limited = true;
                continue;
            }
            budget.expanded += size;
            std::vector<uint8_t> decoded(size_t(std::max(uint64_t(1), size)), 0);
            if (method == 0) {
                if (size != packed) throw std::runtime_error("stored ZIP member size mismatch");
                std::copy_n(bytes.begin() + size_t(data), size_t(size), decoded.begin());
            } else {
                z_stream stream{};
                if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
                    throw std::runtime_error("cannot initialize ZIP deflate decoder");
                struct End { z_stream *stream; ~End() { inflateEnd(stream); } } guard{&stream};
                stream.next_in = const_cast<Bytef *>(bytes.data() + size_t(data));
                stream.avail_in = uInt(packed);
                stream.next_out = decoded.data();
                stream.avail_out = uInt(decoded.size());
                if (inflate(&stream, Z_FINISH) != Z_STREAM_END || stream.total_in != packed || stream.total_out != size) {
                    note("zip_deflate_failed", central, provenance);
                    continue;
                }
            }
            decoded.resize(size_t(size));
            if (crc32(0, decoded.data(), uInt(decoded.size())) != crc) {
                note("zip_crc_mismatch", central, provenance);
                continue;
            }
            auto inner = extract_layer(decoded, limit - out.candidates.size(), budget, depth + 1);
            auto space = "zip_member@0x" + hex(local);
            for (auto &candidate : inner.candidates) {
                candidate.method = "zip/" + candidate.method;
                candidate.offset_space = space + "/" + candidate.offset_space;
                candidate.evidence.push_back({{"adapter", "zip/1"}, {"archive_member", provenance}});
                out.candidates.push_back(std::move(candidate));
            }
            for (auto &finding : inner.findings) {
                finding["offset_space"] = space + "/" + finding.value("offset_space", "file");
                out.findings.push_back(std::move(finding));
            }
            out.limited = out.limited || inner.limited;
            note("zip_member_scanned", central, provenance);
        }
        if (pos != end) throw std::runtime_error("unparsed ZIP central-directory bytes");
    } catch (const std::exception &error) {
        note("rejected_zip", end, {{"detail", error.what()}});
    }
    return out;
}
// Clear block-table layout and extent padding encoding corroborated against
// flatz/pkg_pfs_tool src/self.{h,c}. This only decompresses already-clear bytes;
// it does not decrypt, authenticate signatures, or modify SELF files.
std::vector<uint8_t> self_blocks(Bytes bytes, uint64_t entry_index, uint64_t count, uint64_t flags,
                                 uint64_t source, uint64_t packed, uint64_t size,
                                 ContainerBudget &budget, json &mapping) {
    if (((flags >> 8) & 7) != 4)
        throw std::runtime_error("unsupported SELF compression window encoding");
    const uint64_t block_size = uint64_t(1) << (12 + ((flags >> 12) & 15));
    const auto blocks = size / block_size + (size % block_size != 0);
    if (blocks > 4096 || blocks > 4096 - std::min(size_t(4096), budget.frames) ||
        size > 512 * 1024 * 1024 - budget.expanded) {
        budget.limited = true;
        throw std::runtime_error("SELF block count or expansion budget exceeded");
    }
    std::optional<uint64_t> table_index;
    for (uint64_t i = 0; i < count; ++i) {
        auto table_flags = integer(bytes, 32 + 32 * i, 8);
        if (!(table_flags & 0x800) && (table_flags & 0x30000) && ((table_flags >> 20) & 0xffff) == entry_index) {
            if (table_index) throw std::runtime_error("ambiguous SELF linked block table");
            table_index = i;
        }
    }
    if (!table_index) throw std::runtime_error("compressed SELF has no linked block table");
    auto table = bytes.subspan(size_t(32 + 32 * *table_index), 32);
    auto table_flags = integer(table, 0, 8), table_source = integer(table, 8, 8),
         table_packed = integer(table, 16, 8), table_size = integer(table, 24, 8);
    if (table_flags & 2) throw std::runtime_error("encrypted SELF block table is unsupported");
    if (table_flags & 8) throw std::runtime_error("compressed SELF block table is unsupported");
    bool has_digests = (table_flags & 0x10000) != 0;
    if (!(table_flags & 0x20000)) throw std::runtime_error("compressed SELF requires explicit block extents");
    if (table_packed != table_size || table_size != blocks * (has_digests ? 40 : 8) ||
        !contains(bytes, table_source, table_size) ||
        (table_source < source + packed && source < table_source + table_size))
        throw std::runtime_error("invalid SELF block-table extent");
    budget.frames += size_t(blocks);
    budget.expanded += size;
    std::vector<uint8_t> decoded(size_t(size), 0);
    uint64_t previous_end = 0;
    const auto extents = table_source + (has_digests ? blocks * 32 : 0);
    for (uint64_t block = 0; block < blocks; ++block) {
        auto offset = integer(bytes, extents + block * 8, 4);
        auto encoded_size = integer(bytes, extents + block * 8 + 4, 4);
        auto aligned_size = encoded_size & ~uint64_t(15), padding = encoded_size & 15;
        if (aligned_size < padding) throw std::runtime_error("invalid SELF block padding");
        const auto payload_size = aligned_size - padding;
        auto expanded_offset = block * block_size, expanded_size = std::min(block_size, size - expanded_offset);
        if ((offset & 15) || offset < previous_end || offset > packed || aligned_size > packed - offset ||
            !payload_size || payload_size > expanded_size)
            throw std::runtime_error("invalid or overlapping SELF block extent");
        previous_end = offset + aligned_size;
        auto payload = bytes.subspan(size_t(source + offset), size_t(payload_size));
        if (has_digests) {
            std::string expected;
            for (auto byte : bytes.subspan(size_t(table_source + block * 32), 32)) expected += hex(byte, 2);
            if (sha256(payload) != expected) throw std::runtime_error("SELF block digest mismatch");
        }
        bool stored = payload_size == expanded_size;
        if (stored) {
            std::copy(payload.begin(), payload.end(), decoded.begin() + size_t(expanded_offset));
        } else {
            z_stream stream{};
            if (inflateInit2(&stream, 12) != Z_OK)
                throw std::runtime_error("cannot initialize SELF block decompressor");
            struct End { z_stream *stream; ~End() { inflateEnd(stream); } } guard{&stream};
            stream.next_in = const_cast<Bytef *>(payload.data());
            stream.avail_in = uInt(payload.size());
            stream.next_out = decoded.data() + size_t(expanded_offset);
            stream.avail_out = uInt(expanded_size);
            if (inflate(&stream, Z_FINISH) != Z_STREAM_END || stream.total_in != payload_size ||
                stream.total_out != expanded_size)
                throw std::runtime_error("SELF block decompression failed or size mismatch");
        }
        mapping.push_back({{"block", block}, {"source_offset", source + offset},
                           {"stored_bytes", payload_size}, {"encoded_extent_size", encoded_size},
                           {"expanded_offset", expanded_offset}, {"expanded_bytes", expanded_size},
                           {"encoding", stored ? "stored" : "zlib"}, {"digest_checked", has_digests}});
    }
    return decoded;
}
Extraction extract_layer(Bytes bytes, size_t limit, [[maybe_unused]] ContainerBudget &budget,
                         [[maybe_unused]] unsigned depth) {
    if (auto end = zip_end(bytes)) return extract_zip(bytes, *end, limit, budget, depth);
    if (contains(bytes, 0, 4) && integer(bytes, 0, 4) == 0x04034b50) {
        Extraction rejected;
        rejected.findings.push_back({{"kind", "rejected_zip"}, {"adapter", "zip/1"},
                                     {"detail", "missing complete central directory; no raw member fallback"}});
        return rejected;
    }
    const bool is_self = contains(bytes, 0, 4) &&
        (integer(bytes, 0, 4) == 0x1d3d154f || integer(bytes, 0, 4) == 0xeef51454);
    // A known SELF must not bypass segment encryption/validation through raw
    // signature scanning of its container bytes.
    auto out = is_self ? Extraction{} : extract(bytes, limit);
#ifdef SL_HAVE_ZSTD
    // Scan independently framed Zstandard data. Unknown-size/dictionary streams
    // remain coverage gaps; never allocate from an unchecked declared size.
    for (size_t pos = 0; !is_self && pos + 4 <= bytes.size();) {
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
    if (bytes.size() < 32) {
        if (is_self) out.findings.push_back({{"kind", "rejected_self"}, {"detail", "truncated SELF header"}});
        return out;
    }
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
            uint64_t packed;
            std::vector<uint8_t> decoded;
            json blocks;
        };
        std::vector<Segment> segments;
        std::set<uint64_t> seen;
        uint64_t extent = 0;
        for (uint64_t i = 0; i < count; ++i) {
            auto s = bytes.subspan(size_t(32 + i * 32), 32);
            auto flags = integer(s, 0, 8);
            if (!(flags & 0x800))
                continue;
            auto idx = (flags >> 20) & 0xffff;
            if (idx >= phnum)
                throw std::runtime_error("SELF segment references absent program header");
            auto ph = e.subspan(size_t(phoff + idx * stride), 56);
            if (integer(ph, 0, 4) != 1)
                continue;
            if (flags & 2) {
                out.findings.push_back({{"kind", "encrypted_self_segment"}, {"adapter", "self/2"}, {"segment", i}});
                continue;
            }
            auto src = integer(s, 8, 8), packed = integer(s, 16, 8), unpacked = integer(s, 24, 8),
                 dst = integer(ph, 8, 8), size = integer(ph, 32, 8), va = integer(ph, 16, 8);
            if (!seen.insert(idx).second)
                throw std::runtime_error("duplicate SELF segment mapping");
            if (size > 256 * 1024 * 1024 || dst > 256 * 1024 * 1024 - size || unpacked != size ||
                packed > 256 * 1024 * 1024 || !contains(bytes, src, packed) ||
                (!(flags & 8) && packed != size))
                throw std::runtime_error("invalid/unbounded SELF load segment extent");
            for (const auto &previous : segments)
                if (dst < previous.dst + previous.size && previous.dst < dst + size)
                    throw std::runtime_error("overlapping SELF load segments");
            Segment segment{src, dst, size, va, packed, {}, json::array()};
            if (flags & 8) {
                if (depth >= 4) {
                    out.findings.push_back({{"kind", "container_depth_budget"}, {"adapter", "self/2"}, {"segment", i}});
                    out.limited = true;
                    continue;
                }
                segment.decoded = self_blocks(bytes, i, count, flags, src, packed, size, budget, segment.blocks);
            }
            segments.push_back(std::move(segment));
            extent = std::max(extent, dst + size);
        }
        if (!extent)
            return out;
        if (extent > 512 * 1024 * 1024 - budget.expanded) {
            out.findings.push_back({{"kind", "container_expansion_budget"},
                                    {"adapter", "self/2"}, {"expanded_bytes", extent}});
            out.limited = true;
            return out;
        }
        budget.expanded += extent;
        std::vector<uint8_t> image(size_t(extent), 0);
        for (const auto &s : segments) {
            auto source = s.decoded.empty() ? bytes.subspan(size_t(s.src), size_t(s.size)) : Bytes(s.decoded);
            std::copy(source.begin(), source.end(), image.begin() + std::ptrdiff_t(s.dst));
        }
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
                                   {"bytes", s.size}, {"stored_bytes", s.packed},
                                   {"blocks", s.blocks}});
            c.evidence.push_back({{"adapter", "self/2"}, {"self_segment_map", map}});
            out.candidates.push_back(std::move(c));
        }
        for (auto &finding : extracted.findings) {
            finding["offset_space"] = "reconstructed_elf_file";
            out.findings.push_back(std::move(finding));
        }
        out.limited = out.limited || extracted.limited;
        out.findings.push_back({{"kind", "self_normalization"},
                                {"adapter", "self/2"},
                                {"clear_load_segments", segments.size()},
                                {"note", "only fully backed candidates retained; "
                                         "encrypted data is not reconstructed; block digests are not signature authentication"}});
    } catch (const std::exception &ex) {
        out.findings.push_back({{"kind", "rejected_self"}, {"adapter", "self/2"}, {"detail", ex.what()}});
    }
    return out;
}
} // namespace
Extraction extract_containers(Bytes bytes, size_t limit) {
    ContainerBudget budget;
    auto out = extract_layer(bytes, limit, budget, 0);
    out.limited = out.limited || budget.limited;
    return out;
}
} // namespace sl
