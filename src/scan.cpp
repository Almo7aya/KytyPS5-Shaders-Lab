#include "shader_lab/lab.hpp"
#include <algorithm>
#include <bit>
#include <cctype>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
namespace sl {
namespace {
#ifdef SL_HAVE_ZSTD
const std::string extractor_identity = std::string(SL_EXTRACTOR_ID) + "-zstd";
#else
const std::string extractor_identity = std::string(SL_EXTRACTOR_ID) + "-raw";
#endif
constexpr uint64_t max_blob = 32 * 1024 * 1024;
constexpr uint64_t extraction_budget = 256 * 1024 * 1024;
size_t find(Bytes b, std::string_view needle, size_t start) {
    while (start <= b.size() && needle.size() <= b.size() - start) {
        auto p = static_cast<const uint8_t *>(std::memchr(b.data() + start, uint8_t(needle[0]),
                                                          b.size() - start - needle.size() + 1));
        if (!p)
            break;
        start = size_t(p - b.data());
        if (!std::memcmp(p, needle.data(), needle.size()))
            return start;
        ++start;
    }
    return b.size();
}
bool relative_range(Bytes h, uint64_t field, uint64_t count, uint64_t stride) {
    if (!count)
        return true;
    auto raw = integer(h, field, 8);
    auto delta = std::bit_cast<int64_t>(raw);
    if (delta < 0 && uint64_t(-(delta + 1)) + 1 > field)
        return false;
    if (delta > 0 && uint64_t(delta) > UINT64_MAX - field)
        return false;
    auto start = field + raw;
    return count <= max_blob / stride && contains(h, start, count * stride);
}
std::vector<std::pair<uint32_t, uint32_t>> registers(Bytes h) {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    for (auto [field, count] :
         {std::pair{0x18u, unsigned(h[0x5b])}, std::pair{0x20u, unsigned(h[0x5c])}}) {
        if (!count)
            continue;
        auto base = uint64_t(field) + integer(h, field, 8);
        for (unsigned i = 0; i < count; ++i)
            out.emplace_back(uint32_t(integer(h, base + i * 8, 4)),
                             uint32_t(integer(h, base + i * 8 + 4, 4)));
    }
    return out;
}
std::string stage(uint8_t t) {
    static constexpr const char *names[] = {"CS",       "PS",      "GS",      "HS", "GS-front",
                                            "HS-front", "GS-back", "HS-back", "FS"};
    return t < 9 ? names[t] : "unknown";
}
std::string game_name(const fs::path &file, const fs::path &root) {
    auto parent = file.parent_path();
    while (is_within(parent, root)) {
        if (fs::exists(parent / "eboot.bin") || fs::exists(parent / "sce_sys/param.json"))
            return path_text(fs::relative(parent, root));
        if (parent == root || parent == parent.parent_path())
            break;
        parent = parent.parent_path();
    }
    auto rel = fs::relative(file, root);
    auto it = rel.begin();
    return it == rel.end() ? "." : path_text(*it);
}
} // namespace
bool valid_header(Bytes h, std::string &reason) {
    auto fail = [&](std::string s) {
        reason = std::move(s);
        return false;
    };
    if (h.size() < 0x60)
        return fail("truncated AGC header");
    if (integer(h, 0, 4) != 0x34333231 || integer(h, 4, 4) != 0x18)
        return fail("unsupported AGC magic/version");
    auto hs = integer(h, 0x40, 4), cs = integer(h, 0x44, 4);
    if (hs != h.size() || hs > max_blob || !cs || cs > max_blob || cs % 4)
        return fail("invalid header/code extent");
    if (h[0x5a] > 8)
        return fail("unknown stage type");
    if (!relative_range(h, 0x18, h[0x5b], 8) || !relative_range(h, 0x20, h[0x5c], 8) ||
        !relative_range(h, 0x30, integer(h, 0x50, 4), 4) ||
        !relative_range(h, 0x38, integer(h, 0x56, 2), 4))
        return fail("self-relative register/semantic table outside header");
    reason.clear();
    return true;
}
Extraction extract(Bytes b, size_t limit) {
    Extraction out;
    uint64_t retained = 0;
    std::vector<std::pair<uint64_t, uint64_t>> extents;
    auto note = [&](uint64_t offset, std::string kind, std::string detail) {
        if (out.findings.size() < 10000)
            out.findings.push_back({{"offset", offset}, {"kind", kind}, {"detail", detail}});
        else
            out.limited = true;
    };
    auto add = [&](uint64_t hp, uint64_t cp, Bytes h, Bytes code, const char *method,
                   json evidence) {
        if (out.candidates.size() >= limit ||
            retained + h.size() + code.size() > extraction_budget) {
            out.limited = true;
            return;
        }
        retained += h.size() + code.size();
        out.candidates.push_back({hp,
                                  cp,
                                  {h.begin(), h.end()},
                                  {code.begin(), code.end()},
                                  method,
                                  std::move(evidence)});
    };
    // Parse every ELF candidate independently. Offsets are relative to its own base,
    // never host pointers. All additions and multiplications are range checked.
    for (size_t p = find(b,
                         "\x7f"
                         "ELF",
                         0);
         p < b.size(); p = find(b,
                                "\x7f"
                                "ELF",
                                p + 4)) {
        if (out.limited)
            break;
        if (!contains(b, p, 64)) {
            note(p, "truncated_elf", "header");
            continue;
        }
        auto e = b.subspan(p);
        if (e[4] != 2 || e[5] != 1 || integer(e, 18, 2) != 0xe0)
            continue;
        try {
            auto shoff = integer(e, 40, 8), stride = integer(e, 58, 2), count = integer(e, 60, 2),
                 names = integer(e, 62, 2);
            if (stride < 64 || !count || count > 8192 || names >= count ||
                !contains(e, shoff, count * stride))
                throw std::runtime_error("invalid section table");
            auto section = [&](uint64_t n) {
                auto h = e.subspan(size_t(shoff + n * stride), 64);
                auto o = integer(h, 24, 8), z = integer(h, 32, 8);
                if (!contains(e, o, z))
                    throw std::runtime_error("section outside file");
                return std::pair{o, z};
            };
            auto [no, ns] = section(names);
            if (ns > 4 * 1024 * 1024)
                throw std::runtime_error("oversized string table");
            auto strings = e.subspan(size_t(no), size_t(ns));
            uint64_t hp = 0, hs = 0, cp = 0, cs = 0, extent = shoff + count * stride;
            for (uint64_t i = 1; i < count; ++i) {
                auto h = e.subspan(size_t(shoff + i * stride), 64);
                auto name = integer(h, 0, 4);
                if (name >= strings.size())
                    throw std::runtime_error("section name out of bounds");
                auto end = static_cast<const uint8_t *>(
                    std::memchr(strings.data() + name, 0, strings.size() - size_t(name)));
                if (!end)
                    throw std::runtime_error("unterminated section name");
                std::string_view n(reinterpret_cast<const char *>(strings.data() + name),
                                   size_t(end - (strings.data() + name)));
                if (integer(h, 4, 4) == 8)
                    continue; // SHT_NOBITS has no file bytes.
                auto [o, z] = section(i);
                extent = std::max(extent, o + z);
                if (n == ".shader_header") {
                    if (hs)
                        throw std::runtime_error("duplicate header section");
                    hp = o;
                    hs = z;
                }
                if (n == ".shader_text") {
                    if (cs)
                        throw std::runtime_error("duplicate text section");
                    cp = o;
                    cs = z;
                }
            }
            if (!hs || !cs)
                continue;
            auto h = e.subspan(size_t(hp), size_t(hs));
            std::string why;
            if (!valid_header(h, why))
                throw std::runtime_error(why);
            auto size = integer(h, 0x44, 4);
            if (size > cs)
                throw std::runtime_error("declared shader code exceeds section");
            extents.emplace_back(p, p + extent);
            add(p + hp, p + cp, h, e.subspan(size_t(cp), size_t(size)), "amdgpu_elf",
                json::array({"ELF64 little-endian e_machine=0xe0", "named shader sections",
                             "validated AGC extents"}));
        } catch (const std::exception &ex) {
            note(p, "rejected_shader_elf", ex.what());
        }
    }
    auto in_elf = [&](uint64_t p) {
        return std::any_of(extents.begin(), extents.end(),
                           [&](auto e) { return p >= e.first && p < e.second; });
    };
    std::map<uint32_t, std::vector<uint64_t>> ends;
    size_t markers = 0;
    for (size_t p = find(b, "barefoot", 0); p < b.size(); p = find(b, "barefoot", p + 8)) {
        if (++markers > 1000000) {
            out.limited = true;
            break;
        }
        if (!in_elf(p) && contains(b, p, 0x30))
            ends[uint32_t(integer(b, p + 8, 4))].push_back(p + 0x30);
    }
    const std::string_view magic("1234\x18\0\0\0", 8);
    size_t headers = 0, pair_work = 0;
    for (size_t p = find(b, magic, 0); p < b.size(); p = find(b, magic, p + 8)) {
        if (out.limited || ++headers > 1000000) {
            out.limited = true;
            break;
        }
        if (in_elf(p))
            continue;
        if (!contains(b, p, 0x60)) {
            note(p, "rejected_header", "truncated header");
            continue;
        }
        auto hs = integer(b, p + 0x40, 4), cs = integer(b, p + 0x44, 4);
        if (hs > max_blob || !contains(b, p, hs)) {
            note(p, "rejected_header", "header extent outside file/limit");
            continue;
        }
        auto h = b.subspan(p, size_t(hs));
        std::string why;
        if (!valid_header(h, why)) {
            note(p, "rejected_header", why);
            continue;
        }
        std::map<std::string, uint64_t> matches;
        for (auto [reg, value] : registers(h)) {
            (void)reg;
            auto it = ends.find(value);
            if (it == ends.end())
                continue;
            for (auto end : it->second) {
                if (++pair_work > 1000000) {
                    out.limited = true;
                    break;
                }
                if (end >= cs && (end - cs) % 256 == 0 && contains(b, end - cs, cs)) {
                    auto start = end - cs;
                    if (start < p + hs && p < end)
                        continue;
                    matches.emplace(sha256(b.subspan(size_t(start), size_t(cs))), start);
                }
            }
        }
        if (matches.size() == 1)
            add(p, matches.begin()->second, h,
                b.subspan(size_t(matches.begin()->second), size_t(cs)), "bare_header_checksum",
                json::array({"register value matches barefoot trailer checksum",
                             "256-byte aligned code start",
                             "unique code content in file; heuristic pairing, not runtime proof"}));
        else
            note(p, matches.empty() ? "unpaired_header" : "ambiguous_header",
                 "candidate code contents=" + std::to_string(matches.size()));
    }
    return out;
}
json scan(const ScanOptions &opt) {
    auto input = fs::weakly_canonical(opt.input), output = fs::weakly_canonical(opt.output);
    if (!fs::exists(input))
        throw std::runtime_error("input does not exist");
    auto root = fs::is_directory(input) ? input : input.parent_path();
    if (is_within(output, root) || is_within(root, output))
        throw std::runtime_error("dataset and game input must be disjoint directories");
    fs::create_directories(output);
    // Directory lock is intentionally recoverable after a crash; do not auto-steal.
    if (!fs::create_directory(output / ".scan-lock"))
        throw std::runtime_error(
            "dataset locked; confirm no scan is running before removing .scan-lock");
    struct Lock {
        fs::path p;
        ~Lock() {
            std::error_code e;
            fs::remove(p, e);
        }
    } lock{output / ".scan-lock"};
    json old;
    if (opt.resume && fs::exists(output / "manifest.json"))
        old = read_json(output / "manifest.json");
    if (!old.is_null() &&
        (old.value("schema", 0) != schema_version || old.value("root", "") != path_text(root)))
        throw std::runtime_error("dataset schema/root mismatch; use a different output directory");
    json doc = {{"schema", schema_version},
                {"tool", "ps5-shader-lab/0.1.0"},
                {"extractor_id", extractor_identity},
                {"root", path_text(root)},
                {"coverage", "recognized clear/raw, SELF load segments, and enabled compression "
                             "formats only; no all-shaders guarantee"},
                {"files", json::object()},
                {"shaders", json::object()},
                {"traversal_errors", json::array()},
                {"limited", false}};
    std::vector<fs::path> files;
    if (fs::is_regular_file(input))
        files.push_back(input);
    else {
        std::error_code ec;
        fs::recursive_directory_iterator it(input, fs::directory_options::none, ec), end;
        if (ec)
            throw std::runtime_error("cannot enumerate input: " + ec.message());
        while (it != end) {
            auto entry = *it;
            auto st = entry.symlink_status(ec);
            if (ec)
                doc["traversal_errors"].push_back(
                    {{"path", path_text(entry.path())}, {"error", ec.message()}});
            else if (fs::is_symlink(st)) {
                it.disable_recursion_pending();
                doc["traversal_errors"].push_back(
                    {{"path", path_text(entry.path())}, {"error", "symlink not followed"}});
            } else if (fs::is_regular_file(st))
                files.push_back(entry.path());
            it.increment(ec);
            if (ec) {
                doc["traversal_errors"].push_back({{"error", ec.message()}});
                ec.clear();
            }
        }
    }
    std::sort(files.begin(), files.end());
    if (opt.max_files && files.size() > opt.max_files) {
        files.resize(size_t(opt.max_files));
        doc["limited"] = true;
    }
    size_t index = 0;
    for (const auto &file : files) {
        auto relative = path_text(fs::relative(file, root));
        json rec = {{"game", game_name(file, root)},
                    {"candidates", json::array()},
                    {"findings", json::array()}};
        try {
            auto size = fs::file_size(file);
            rec["bytes"] = size;
            if (opt.max_file_bytes && size > opt.max_file_bytes) {
                rec["status"] = "size_limit";
                doc["limited"] = true;
            } else {
                MappedFile mapped(file);
                auto b = mapped.bytes();
                auto digest = sha256(b);
                rec["sha256"] = digest;
                bool cached = !old.is_null() &&
                              old.value("extractor_id", "") == extractor_identity &&
                              old["files"].contains(relative) &&
                              old["files"][relative].value("sha256", "") == digest &&
                              old["files"][relative].value("status", "") == "scanned";
                if (cached && file.extension() == ".header") {
                    auto cp = file;
                    cp.replace_extension(".code");
                    cached = fs::exists(cp) && old["files"][relative].value("paired_code_sha256",
                                                                            "") == hash_file(cp);
                }
                if (cached)
                    for (const auto &id : old["files"][relative]["candidates"])
                        if (!fs::exists(output / "objects" / id.get<std::string>() /
                                        "header.bin") ||
                            !fs::exists(output / "objects" / id.get<std::string>() / "code.bin")) {
                            cached = false;
                            break;
                        }
                if (cached) {
                    rec = old["files"][relative];
                    rec["cache_hit"] = true;
                    for (const auto &id : rec["candidates"]) {
                        auto key = id.get<std::string>();
                        if (!doc["shaders"].contains(key)) {
                            doc["shaders"][key] = old["shaders"][key];
                            doc["shaders"][key]["origins"] = json::array();
                        }
                        for (const auto &o : old["shaders"][key]["origins"])
                            if (o["file"] == relative)
                                doc["shaders"][key]["origins"].push_back(o);
                    }
                } else {
                    Extraction found;
                    if (file.extension() == ".header") {
                        auto cp = file;
                        cp.replace_extension(".code");
                        std::string why;
                        if (fs::exists(cp) && valid_header(b, why)) {
                            auto c = read_bytes(cp);
                            rec["paired_code_sha256"] = sha256(c);
                            if (c.size() == integer(b, 0x44, 4))
                                found.candidates.push_back(
                                    {0,
                                     0,
                                     {b.begin(), b.end()},
                                     std::move(c),
                                     "paired_files",
                                     json::array({"matching header/code filename and declared code "
                                                  "size"})});
                            else
                                found.findings.push_back({{"kind", "pair_size_mismatch"}});
                        }
                    } else if (file.extension() != ".code")
                        found = extract_containers(b, opt.max_candidates);
                    rec["status"] = found.limited ? "candidate_limit" : "scanned";
                    rec["findings"] = std::move(found.findings);
                    rec["cache_hit"] = false;
                    if (found.limited)
                        doc["limited"] = true;
                    auto ext = path_text(file.extension());
                    std::transform(ext.begin(), ext.end(), ext.begin(),
                                   [](unsigned char c) { return char(std::tolower(c)); });
                    if (ext == ".pkg" || ext == ".pak" || ext == ".ucas" || ext == ".zip" ||
                        ext == ".gz" || ext == ".zst" || ext == ".zar" || ext == ".7z" ||
                        ext == ".psarc")
                        rec["findings"].push_back(
                            {{"kind", "container_coverage_gap"},
                             {"detail", "raw bytes scanned; format-aware decompression/decryption "
                                        "not implemented"}});
                    for (const auto &c : found.candidates) {
                        auto hs = sha256(c.header), cs = sha256(c.code);
                        auto key = hs + "-" + cs;
                        auto obj = output / "objects" / key;
                        if (!fs::exists(obj / "header.bin"))
                            write_bytes(obj / "header.bin", c.header);
                        if (!fs::exists(obj / "code.bin"))
                            write_bytes(obj / "code.bin", c.code);
                        if (!doc["shaders"].contains(key))
                            doc["shaders"][key] = {{"header_sha256", hs},
                                                   {"code_sha256", cs},
                                                   {"kyty_hash", xxh3(c.code)},
                                                   {"type", stage(c.header[0x5a])},
                                                   {"header_bytes", c.header.size()},
                                                   {"code_bytes", c.code.size()},
                                                   {"origins", json::array()}};
                        doc["shaders"][key]["origins"].push_back(
                            {{"file", relative},
                             {"game", rec["game"]},
                             {"header_offset", c.header_offset},
                             {"code_offset", c.code_offset},
                             {"offset_space", c.offset_space},
                             {"method", c.method},
                             {"evidence", c.evidence}});
                        if (std::find(rec["candidates"].begin(), rec["candidates"].end(),
                                      json(key)) == rec["candidates"].end())
                            rec["candidates"].push_back(key);
                    }
                }
            }
        } catch (const std::exception &ex) {
            rec["status"] = "read_error";
            rec["error"] = ex.what();
        }
        doc["files"][relative] = std::move(rec);
        ++index;
        if (index % 100 == 0 || !doc["files"][relative]["candidates"].empty() ||
            index == files.size())
            std::cout << "[" << index << "/" << files.size() << "] " << relative
                      << " shaders=" << doc["files"][relative]["candidates"].size() << "\n"
                      << std::flush;
        if (index % 100 == 0) {
            doc["in_progress"] = true;
            atomic_json(output / "manifest.json", doc);
        }
    }
    doc["in_progress"] = false;
    doc["shader_count"] = doc["shaders"].size();
    doc["file_count"] = doc["files"].size();
    atomic_json(output / "manifest.json", doc);
    return doc;
}
} // namespace sl
