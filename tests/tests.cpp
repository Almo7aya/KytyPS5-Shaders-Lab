#include "shader_lab/lab.hpp"
#include <iostream>
#include <thread>
#ifdef SL_HAVE_ZSTD
#include <zstd.h>
#endif
using namespace sl;
namespace {
void check(bool ok, const char *what) {
    if (!ok)
        throw std::runtime_error(what);
}
void put(std::vector<uint8_t> &b, size_t o, uint64_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i)
        b.at(o + i) = uint8_t(v >> (8 * i));
}
std::vector<uint8_t> header(uint32_t size = 4) {
    std::vector<uint8_t> h(0x60);
    put(h, 0, 0x34333231, 4);
    put(h, 4, 0x18, 4);
    put(h, 0x40, h.size(), 4);
    put(h, 0x44, size, 4);
    return h;
}
std::vector<uint8_t> elf() {
    std::vector<uint8_t> b(512);
    b[0] = 0x7f;
    b[1] = 'E';
    b[2] = 'L';
    b[3] = 'F';
    b[4] = 2;
    b[5] = 1;
    b[6] = 1;
    put(b, 18, 0xe0, 2);
    put(b, 40, 64, 8);
    put(b, 58, 64, 2);
    put(b, 60, 4, 2);
    put(b, 62, 1, 2);
    std::string names("\0.shstrtab\0.shader_header\0.shader_text\0", 39);
    std::copy(names.begin(), names.end(), b.begin() + 320);
    put(b, 128, 1, 4);
    put(b, 152, 320, 8);
    put(b, 160, names.size(), 8);
    put(b, 192, 11, 4);
    put(b, 216, 384, 8);
    put(b, 224, 96, 8);
    put(b, 256, 26, 4);
    put(b, 280, 480, 8);
    put(b, 288, 4, 8);
    auto h = header();
    std::copy(h.begin(), h.end(), b.begin() + 384);
    put(b, 480, 0xbf810000, 4);
    return b;
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--request") {
            auto q = read_json(path_from(argv[2]));
            auto out = path_from(q["output"].get<std::string>());
            auto mode = q["profile"].value("test_mode", "");
            if (mode == "timeout")
                std::this_thread::sleep_for(std::chrono::seconds(10));
            if (mode == "error")
                return 17;
            atomic_json(out / "response.json",
                        {{"schema", 1}, {"id", q["id"]}, {"status", "fixture_pass"}});
            return 0;
        }
        unsigned checks = 0;
        auto test = [&](bool ok, const char *name) {
            check(ok, name);
            ++checks;
        };
        test(sha256({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
             "SHA empty");
        std::string abc = "abc";
        test(sha256({reinterpret_cast<const uint8_t *>(abc.data()), abc.size()}) ==
                 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
             "SHA abc");
        std::string longv(1000000, 'a');
        test(sha256({reinterpret_cast<const uint8_t *>(longv.data()), longv.size()}) ==
                 "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
             "SHA million a");
        auto h = header();
        std::string why;
        test(valid_header(h, why), "valid header");
        h[0x5b] = 1;
        put(h, 0x18, 0xffff, 8);
        test(!valid_header(h, why), "bad table");
        h = header();
        h[4] = 23;
        test(!valid_header(h, why), "wrong version");
        h = header();
        put(h, 0x44, 3, 4);
        test(!valid_header(h, why), "misaligned code");
        auto b = elf();
        auto e = extract(b);
        test(e.candidates.size() == 1, "ELF extraction");
        test(e.candidates[0].code_offset == 480, "ELF offset");
        test(e.candidates[0].header.size() == 96, "ELF header size");
        auto embedded = b;
        embedded.insert(embedded.begin(), 17, 0xaa);
        test(extract(embedded).candidates[0].header_offset == 401, "unaligned embedded ELF");
        std::vector<uint8_t> self(2048);
        put(self, 0, 0x1d3d154f, 4);
        put(self, 24, 1, 2);
        put(self, 32, 0x800, 8);
        put(self, 40, 1024, 8);
        put(self, 48, 512, 8);
        put(self, 56, 512, 8);
        put(self, 64, 0x464c457f, 4);
        self[68] = 2;
        self[69] = 1;
        put(self, 96, 64, 8);
        put(self, 118, 56, 2);
        put(self, 120, 1, 2);
        put(self, 128, 1, 4);
        put(self, 136, 256, 8);
        put(self, 144, 0x80000100, 8);
        put(self, 160, 512, 8);
        std::copy(b.begin(), b.end(), self.begin() + 1024);
        auto self_result = extract_containers(self);
        test(std::any_of(self_result.candidates.begin(), self_result.candidates.end(),
                         [](const auto &c) { return c.offset_space == "reconstructed_elf_file"; }),
             "SELF coordinate reconstruction");
        put(self, 32, 0x802, 8);
        self_result = extract_containers(self);
        test(std::none_of(self_result.candidates.begin(), self_result.candidates.end(),
                          [](const auto &c) { return c.offset_space == "reconstructed_elf_file"; }),
             "encrypted SELF segment not reconstructed");
#ifdef SL_HAVE_ZSTD
        std::vector<uint8_t> compressed(ZSTD_compressBound(b.size()));
        auto compressed_size =
            ZSTD_compress(compressed.data(), compressed.size(), b.data(), b.size(), 3);
        test(!ZSTD_isError(compressed_size), "zstd fixture generation");
        compressed.resize(compressed_size);
        auto zs = extract_containers(compressed);
        test(std::any_of(zs.candidates.begin(), zs.candidates.end(),
                         [](const auto &c) { return c.method == "zstd/amdgpu_elf"; }),
             "bounded Zstandard ELF extraction");
#endif
        auto bad = b;
        put(bad, 40, UINT64_MAX - 12, 8);
        test(extract(bad).candidates.empty(), "overflow section table");
        bad = b;
        put(bad, 288, UINT64_MAX, 8);
        test(extract(bad).candidates.empty(), "overflow section size");
        bad = b;
        put(bad, 384 + 0x44, 512, 4);
        test(extract(bad).candidates.empty(), "truncated code section");
        for (size_t n = 0; n < b.size(); ++n) {
            auto x = extract(Bytes(b).first(n));
            test(x.candidates.empty(), "all truncated ELF prefixes rejected");
            if (n == 483)
                break;
        }
        std::vector<uint8_t> bare(1024);
        h = header(256);
        h.resize(104);
        put(h, 0x40, 104, 4);
        h[0x5c] = 1;
        put(h, 0x20, 0x40, 8);
        put(h, 96, 0x22a, 4);
        put(h, 100, 0x12345678, 4);
        std::copy(h.begin(), h.end(), bare.begin());
        std::copy_n("barefoot", 8, bare.begin() + 464);
        put(bare, 472, 0x12345678, 4);
        auto x = extract(bare);
        test(x.candidates.size() == 1 && x.candidates[0].code_offset == 256, "bare header match");
        std::copy_n("barefoot", 8, bare.begin() + 720);
        put(bare, 728, 0x12345678, 4);
        bare[600] = 42;
        test(extract(bare).candidates.empty(), "ambiguous bare pair rejected");
        test(html_escape("<script>\"&") == "&lt;script&gt;&quot;&amp;", "HTML escaping");
        auto root =
            fs::temp_directory_path() /
            path_from("shader-lab-test-" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root);
        struct Cleanup {
            fs::path p;
            ~Cleanup() {
                std::error_code ec;
                fs::remove_all(p, ec);
            }
        } cleanup{root};
        write_bytes(root / "games/Game/eboot.bin", b);
        write_bytes(root / "games/Game/copy.bin", b);
        auto d = scan({root / "games", root / "dataset"});
        test(d["shader_count"] == 1, "content dedup");
        test(d["shaders"].begin().value()["origins"].size() == 2, "retain duplicate origins");
        auto d2 = scan({root / "games", root / "dataset"});
        test(d2["files"]["Game/eboot.bin"]["cache_hit"] == true, "scan resume hash check");
        auto hash = d["shaders"].begin().value()["kyty_hash"].get<std::string>();
        test(inspect(root / "dataset", hash)["cases"].size() == 1, "hash reverse lookup");
        write_text(root / "trace.log", "test hash=0x" + hash + "\ntest hash=0xffffffffffffffff\n");
        auto trace = correlate(root / "dataset", root / "trace.log");
        test(trace["observed"].size() == 2 && trace["runtime_only"].size() == 1,
             "runtime log correlation");
        auto profile = root / "profile.json";
        atomic_json(profile, {{"test_mode", "ok"}});
        auto exe = fs::absolute(path_from(argv[0]));
        RunOptions options{root / "dataset", root / "run", exe, profile, 2, 5000, 0, true};
        auto r = run(options);
        test(r["status_counts"]["fixture_pass"] == 1, "isolated process protocol");
        auto r2 = run(options);
        test(r2["results"].begin().value()["cache_hit"] == true, "run resume");
        atomic_json(profile, {{"test_mode", "timeout"}});
        options.timeout_ms = 150;
        auto t = run(options);
        test(t["status_counts"]["timeout"] == 1, "timeout process termination");
        atomic_json(profile, {{"test_mode", "error"}});
        options.timeout_ms = 5000;
        auto f = run(options);
        test(f["status_counts"]["worker_crash_or_error"] == 1, "failed worker isolation");
        report(root / "dataset", root / "run/results.json", root / "report.html");
        test(fs::file_size(root / "report.html") > 500, "HTML report");
        test(cluster(root / "run/results.json")["groups"].size() == 1, "failure grouping");
        test(compare(root / "run/results.json", root / "run/results.json")["changes"].empty(),
             "identical regression comparison");
        if (argc == 3 && std::string(argv[1]) == "--real-worker") {
            atomic_json(profile, {{"schema", 1}, {"stage", "CS"}, {"mode", "header_probe"}});
            options.worker = fs::absolute(path_from(argv[2]));
            options.timeout_ms = 10000;
            options.output = root / "real-run";
            auto real = run(options);
            test(real["status_counts"].value("spirv_valid_under_profile", 0) == 1,
                 "real Kyty end-program SPIR-V validation");
        }
        std::cout << "PASS: " << checks << " checks (fixtures; no guest/GPU conformance)\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
