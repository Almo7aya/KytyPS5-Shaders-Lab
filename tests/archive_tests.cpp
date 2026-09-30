#include "shader_lab/lab.hpp"
#include <algorithm>
#include <stdexcept>
#include <zlib.h>
using namespace sl;
namespace {
void put(std::vector<uint8_t> &bytes, size_t offset, uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) bytes.at(offset + i) = uint8_t(value >> (i * 8));
}
struct Zip { std::vector<uint8_t> bytes; size_t central, data; };
std::vector<uint8_t> deflated(Bytes input, int window) {
    z_stream stream{};
    if (deflateInit2(&stream, 6, Z_DEFLATED, window, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        throw std::runtime_error("fixture block deflate initialization failed");
    struct End { z_stream *stream; ~End() { deflateEnd(stream); } } guard{&stream};
    std::vector<uint8_t> result(deflateBound(&stream, uLong(input.size())));
    stream.next_in = const_cast<Bytef *>(input.data());
    stream.avail_in = uInt(input.size());
    stream.next_out = result.data();
    stream.avail_out = uInt(result.size());
    if (deflate(&stream, Z_FINISH) != Z_STREAM_END) throw std::runtime_error("fixture block deflate failed");
    result.resize(stream.total_out);
    return result;
}
struct BlockSelf { std::vector<uint8_t> bytes; size_t extents; };
BlockSelf block_self(Bytes shader, bool digests) {
    // Three independently framed blocks: raw 4096, zlib 4096, raw partial 257.
    // The middle block contains an unaligned AMDGPU ELF; the file's outer ELF
    // is only the SELF program-header mapping and is never executed.
    std::vector<uint8_t> clear(2 * 4096 + 257);
    for (size_t i = 0; i < 4096; ++i) clear[i] = uint8_t(i * 17 + 3);
    std::copy(shader.begin(), shader.end(), clear.begin() + 4096 + 17);
    for (size_t i = 8192; i < clear.size(); ++i) clear[i] = uint8_t(i);
    const size_t table_source = 768, data_source = 1024;
    std::vector<uint8_t> bytes(data_source);
    put(bytes, 0, 0xeef51454, 4);
    put(bytes, 24, 2, 2);
    put(bytes, 32, (uint64_t(1) << 20) | 0x20000 | (digests ? 0x10000 : 0), 8);
    put(bytes, 40, table_source, 8);
    put(bytes, 48, digests ? 120 : 24, 8);
    put(bytes, 56, digests ? 120 : 24, 8);
    put(bytes, 64, 0xc08, 8);
    put(bytes, 72, data_source, 8);
    put(bytes, 88, clear.size(), 8);
    put(bytes, 96, 0x464c457f, 4);
    bytes[100] = 2;
    bytes[101] = 1;
    put(bytes, 128, 64, 8);
    put(bytes, 150, 56, 2);
    put(bytes, 152, 1, 2);
    put(bytes, 160, 1, 4);
    put(bytes, 176, 0x80000000, 8);
    put(bytes, 192, clear.size(), 8);
    const size_t extents = table_source + (digests ? 96 : 0);
    for (size_t block = 0; block < 3; ++block) {
        auto input = Bytes(clear).subspan(block * 4096, std::min(size_t(4096), clear.size() - block * 4096));
        auto payload = block == 1 ? deflated(input, 12) : std::vector<uint8_t>(input.begin(), input.end());
        auto aligned = (payload.size() + 15) & ~size_t(15);
        put(bytes, extents + block * 8, bytes.size() - data_source, 4);
        put(bytes, extents + block * 8 + 4, aligned + aligned - payload.size(), 4);
        if (digests) {
            auto hash = sha256(payload);
            for (size_t i = 0; i < 32; ++i)
                bytes[table_source + block * 32 + i] = uint8_t(std::stoul(hash.substr(i * 2, 2), nullptr, 16));
        }
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        bytes.resize(bytes.size() + aligned - payload.size(), 0);
    }
    put(bytes, 80, bytes.size() - data_source, 8);
    return {std::move(bytes), extents};
}
Zip zip(Bytes input, bool compressed, bool descriptor = false, bool signature = true,
        const std::string &name = "shaders/member.bin") {
    std::vector<uint8_t> payload(input.begin(), input.end());
    if (compressed) {
        z_stream stream{};
        if (deflateInit2(&stream, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
            throw std::runtime_error("fixture deflate initialization failed");
        struct End { z_stream *stream; ~End() { deflateEnd(stream); } } guard{&stream};
        payload.resize(deflateBound(&stream, uLong(input.size())));
        stream.next_in = const_cast<Bytef *>(input.data());
        stream.avail_in = uInt(input.size());
        stream.next_out = payload.data();
        stream.avail_out = uInt(payload.size());
        if (deflate(&stream, Z_FINISH) != Z_STREAM_END)
            throw std::runtime_error("fixture deflate failed");
        payload.resize(stream.total_out);
    }
    auto checksum = crc32(0, input.data(), uInt(input.size()));
    size_t data = 30 + name.size(), central = data + payload.size() + (descriptor ? (signature ? 16 : 12) : 0);
    size_t end = central + 46 + name.size();
    std::vector<uint8_t> bytes(end + 22);
    put(bytes, 0, 0x04034b50, 4);
    put(bytes, 4, 20, 2);
    put(bytes, 6, descriptor ? 8 : 0, 2);
    put(bytes, 8, compressed ? 8 : 0, 2);
    if (!descriptor) {
        put(bytes, 14, checksum, 4);
        put(bytes, 18, payload.size(), 4);
        put(bytes, 22, input.size(), 4);
    }
    put(bytes, 26, name.size(), 2);
    std::copy(name.begin(), name.end(), bytes.begin() + 30);
    std::copy(payload.begin(), payload.end(), bytes.begin() + data);
    if (descriptor) {
        auto offset = data + payload.size();
        if (signature) { put(bytes, offset, 0x08074b50, 4); offset += 4; }
        put(bytes, offset, checksum, 4);
        put(bytes, offset + 4, payload.size(), 4);
        put(bytes, offset + 8, input.size(), 4);
    }
    put(bytes, central, 0x02014b50, 4);
    put(bytes, central + 4, 20, 2);
    put(bytes, central + 6, 20, 2);
    put(bytes, central + 8, descriptor ? 8 : 0, 2);
    put(bytes, central + 10, compressed ? 8 : 0, 2);
    put(bytes, central + 16, checksum, 4);
    put(bytes, central + 20, payload.size(), 4);
    put(bytes, central + 24, input.size(), 4);
    put(bytes, central + 28, name.size(), 2);
    std::copy(name.begin(), name.end(), bytes.begin() + central + 46);
    put(bytes, end, 0x06054b50, 4);
    put(bytes, end + 8, 1, 2);
    put(bytes, end + 10, 1, 2);
    put(bytes, end + 12, 46 + name.size(), 4);
    put(bytes, end + 16, central, 4);
    return {std::move(bytes), central, data};
}
} // namespace
unsigned archive_tests(Bytes shader) {
    unsigned checks = 0;
    auto check = [&](bool condition, const char *message) {
        if (!condition) throw std::runtime_error(message);
        ++checks;
    };
    auto finding = [](const Extraction &result, const char *kind) {
        return std::any_of(result.findings.begin(), result.findings.end(),
                           [&](const auto &value) { return value.value("kind", "") == kind; });
    };
    for (bool compressed : {false, true}) {
        auto fixture = zip(shader, compressed);
        auto result = extract_containers(fixture.bytes);
        check(result.candidates.size() == 1 && result.candidates[0].method == "zip/amdgpu_elf",
              "ZIP stored and deflated member extraction");
        check(result.candidates[0].offset_space == "zip_member@0x0/file" &&
                  result.candidates[0].evidence.back()["archive_member"]["member_name"] == "shaders/member.bin",
              "ZIP member provenance and local coordinate space");
        for (bool signature : {false, true}) {
            auto streamed = zip(shader, compressed, true, signature);
            check(extract_containers(streamed.bytes).candidates.size() == 1,
                  "ZIP central directory resolves streamed member descriptors");
        }
        fixture.bytes[fixture.central + 16] ^= 1;
        fixture.bytes[14] ^= 1;
        result = extract_containers(fixture.bytes);
        check(result.candidates.empty() && finding(result, "zip_crc_mismatch"),
              "ZIP bad CRC cannot bypass verification via raw extraction");
    }
    auto encrypted = zip(shader, false);
    put(encrypted.bytes, 6, 1, 2);
    put(encrypted.bytes, encrypted.central + 8, 1, 2);
    auto result = extract_containers(encrypted.bytes);
    check(result.candidates.empty() && finding(result, "encrypted_zip_member_unsupported"),
          "encrypted ZIP members never become raw-scan candidates");
    auto unsupported = zip(shader, false);
    put(unsupported.bytes, 8, 99, 2);
    put(unsupported.bytes, unsupported.central + 10, 99, 2);
    check(finding(extract_containers(unsupported.bytes), "unsupported_zip_method_or_flags"),
          "unsupported ZIP compression is explicit");
    auto mismatch = zip(shader, false);
    mismatch.bytes[30] = 'X';
    check(finding(extract_containers(mismatch.bytes), "rejected_zip"), "ZIP filename disagreement rejected");
    auto truncated = zip(shader, false);
    truncated.bytes.resize(truncated.data + shader.size());
    result = extract_containers(truncated.bytes);
    check(result.candidates.empty() && finding(result, "rejected_zip"), "ZIP missing directory cannot raw-fallback");
    auto huge = zip(shader, true, true);
    put(huge.bytes, huge.central + 24, 300 * 1024 * 1024, 4);
    put(huge.bytes, huge.central - 4, 300 * 1024 * 1024, 4);
    result = extract_containers(huge.bytes);
    check(result.limited && result.candidates.empty(), "ZIP expansion budget checked before allocation");
    auto zip64 = zip(shader, false);
    put(zip64.bytes, zip64.bytes.size() - 12, 0xffff, 2);
    put(zip64.bytes, zip64.bytes.size() - 14, 0xffff, 2);
    check(finding(extract_containers(zip64.bytes), "unsupported_zip64"), "ZIP64 is an explicit coverage gap");
    auto hostile_name = zip(shader, true, false, true, "../../outside.bin");
    check(extract_containers(hostile_name.bytes).candidates.size() == 1,
          "archive path traversal names are labels only");
    auto inner = zip(shader, true);
    auto nested = zip(inner.bytes, true);
    result = extract_containers(nested.bytes);
    check(result.candidates.size() == 1 && result.candidates[0].method == "zip/zip/amdgpu_elf",
          "nested archive member extraction");
    for (unsigned i = 0; i < 4; ++i) nested = zip(nested.bytes, true);
    check(extract_containers(nested.bytes).limited, "archive nesting shares the depth budget");
    check(extract_containers(inner.bytes, 0).limited, "archive candidate budget enforced");
    for (bool digests : {false, true}) {
        auto self = block_self(shader, digests);
        result = extract_containers(self.bytes);
        check(result.candidates.size() == 1 && result.candidates[0].method == "self/amdgpu_elf",
              "compressed SELF extent table reconstructs mixed stored/zlib blocks");
        auto maps = result.candidates[0].evidence.back()["self_segment_map"];
        check(maps[0]["blocks"].size() == 3 && maps[0]["blocks"][0]["encoding"] == "stored" &&
                  maps[0]["blocks"][1]["encoding"] == "zlib" &&
                  maps[0]["blocks"][2]["expanded_bytes"] == 257,
              "SELF provenance retains partial blocks and encoding");
        check(maps[0]["blocks"][1]["digest_checked"] == digests,
              "SELF digest checks accurately disclosed");
        auto compressed_archive = zip(self.bytes, true);
        auto nested_self = extract_containers(compressed_archive.bytes);
        check(nested_self.candidates.size() == 1 && nested_self.candidates[0].method == "zip/self/amdgpu_elf",
              "compressed SELF blocks work inside archive members");
    }
    auto self = block_self(shader, true);
    self.bytes[768] ^= 1;
    check(extract_containers(self.bytes).candidates.empty(), "SELF block digest mismatch rejects candidates");
    self = block_self(shader, false);
    put(self.bytes, self.extents + 8, 0, 4);
    check(extract_containers(self.bytes).candidates.empty(), "overlapping SELF block extents rejected");
    self = block_self(shader, false);
    put(self.bytes, self.extents + 4, 15, 4);
    check(extract_containers(self.bytes).candidates.empty(), "SELF extent padding underflow rejected");
    self = block_self(shader, false);
    put(self.bytes, 64, 0xc0a, 8);
    result = extract_containers(self.bytes);
    check(result.candidates.empty() && finding(result, "encrypted_self_segment"),
          "encrypted SELF has no raw scan fallback");
    self = block_self(shader, false);
    put(self.bytes, 32, (uint64_t(1) << 20) | 0x20002, 8);
    check(extract_containers(self.bytes).candidates.empty(), "encrypted SELF metadata cannot be decoded");
    self = block_self(shader, false);
    put(self.bytes, 64, 0x808, 8);
    check(finding(extract_containers(self.bytes), "rejected_self"), "unknown SELF window encoding rejected");
    self = block_self(shader, false);
    self.bytes.resize(1100);
    check(extract_containers(self.bytes).candidates.empty(), "truncated SELF block payload rejected");
    return checks;
}
