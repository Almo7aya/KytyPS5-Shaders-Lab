#include "shader_lab/lab.hpp"
#include "shader_lab/compiler_trace.hpp"
#include <iostream>
#include <thread>
#ifdef SL_HAVE_ZSTD
#include <zstd.h>
#endif
using namespace sl;
unsigned reference_tests(const fs::path &root);
unsigned archive_tests(Bytes shader);
unsigned repro_tests(const fs::path &root, Bytes shader, const fs::path &worker);
unsigned minimize_tests(const fs::path &root, Bytes header, const fs::path &worker);
unsigned minimize_real_tests(const fs::path &root, Bytes header, const fs::path &worker);
bool minimize_fixture_worker(const json &request);
int pass_fixture_worker(const json &request);
unsigned pass_tests(const fs::path &root, Bytes header, const fs::path &worker);
unsigned pass_real_tests(const fs::path &root, Bytes header, const fs::path &worker);
unsigned capture_tests(Bytes header);
unsigned capture_real_tests(const fs::path &root, Bytes header, const fs::path &worker);
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
        if (argc == 2 && std::string(argv[1]) == "--compiler-info") {
            std::cout << json{{"pass_catalog", compiler_pass_catalog()}}.dump();
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--request") {
            auto q = read_json(path_from(argv[2]));
            if (auto result = pass_fixture_worker(q); result >= 0)
                return result;
            if (minimize_fixture_worker(q))
                return 0;
            auto out = path_from(q["output"].get<std::string>());
            auto mode = q["profile"].value("test_mode", "");
            if (mode == "timeout")
                std::this_thread::sleep_for(std::chrono::seconds(10));
            if (mode == "error")
                return 17;
            if (mode == "cwd") {
                atomic_json(out / "response.json", {{"schema", 1}, {"id", q["id"]},
                            {"status", "fixture_pass"}, {"cwd", path_text(fs::current_path())}});
                return 0;
            }
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
        checks += capture_tests(h);
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
        checks += archive_tests(b);
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
        auto compress = [&](Bytes input) {
            std::vector<uint8_t> result(ZSTD_compressBound(input.size()));
            auto length = ZSTD_compress(result.data(), result.size(), input.data(), input.size(), 3);
            test(!ZSTD_isError(length), "nested frame fixture compression");
            result.resize(length);
            return result;
        };
        auto nested = compress(compressed);
        auto nested_result = extract_containers(nested);
        test(std::any_of(nested_result.candidates.begin(), nested_result.candidates.end(),
                         [](const auto &c) {
                             return c.method == "zstd/zstd/amdgpu_elf" &&
                                    c.offset_space == "zstd_frame@0x0/zstd_frame@0x0/file";
                         }), "nested Zstandard retains every offset coordinate layer");
        auto nested_self = extract_containers(compress([&] {
            auto clear_self = self;
            put(clear_self, 32, 0x800, 8);
            return clear_self;
        }()));
        test(std::any_of(nested_self.candidates.begin(), nested_self.candidates.end(),
                         [](const auto &c) { return c.method == "zstd/self/amdgpu_elf"; }),
             "nested SELF normalization retains container provenance");
        for (unsigned depth = 0; depth < 4; ++depth)
            nested = compress(nested);
        auto deep_result = extract_containers(nested);
        test(deep_result.limited && std::any_of(deep_result.findings.begin(), deep_result.findings.end(),
                                               [](const auto &f) {
                                                   return f.value("kind", "") == "container_depth_budget";
                                               }), "nested payload depth budget is explicit");
        std::vector<uint8_t> many_frames;
        auto empty_frame = compress({});
        for (unsigned frame = 0; frame < 4097; ++frame)
            many_frames.insert(many_frames.end(), empty_frame.begin(), empty_frame.end());
        auto many_result = extract_containers(many_frames);
        test(many_result.limited, "aggregate frame count bounds empty-frame workloads");
        std::vector<uint8_t> child_frames;
        for (unsigned frame = 0; frame < 3000; ++frame)
            child_frames.insert(child_frames.end(), empty_frame.begin(), empty_frame.end());
        auto parent_frame = compress(child_frames);
        auto siblings = parent_frame;
        siblings.insert(siblings.end(), parent_frame.begin(), parent_frame.end());
        test(extract_containers(siblings).limited, "nested siblings share one member budget");
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
        const auto long_file = root / std::string(120, 'a') / std::string(120, 'b') / "fixture.json";
        test(path_text(long_file).size() > 260, "long-path fixture exceeds legacy Windows limit");
        atomic_json(long_file, {{"long_path", true}});
        test(read_json(long_file).at("long_path") == true, "long-path JSON write/read round trip");
        test(hash_file(long_file).size() == 64, "long-path file mapping and hashing");
        checks += reference_tests(root / "reference-fixtures");
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
        auto long_workdir = long_file.parent_path();
        auto long_request = long_workdir / "request.json";
        atomic_json(long_request, {{"id", "long-path-fixture"}, {"profile", {{"test_mode", "cwd"}}},
                                  {"output", path_text(long_workdir)}});
        auto long_process = process(exe, {"--request", path_text(long_request)}, long_workdir,
                                    long_workdir / "worker.log", std::chrono::seconds(5));
        test(long_process.exit_code == 0 && !long_process.timed_out,
             "worker launches with absolute evidence paths deeper than MAX_PATH");
        test(fs::equivalent(path_from(read_json(long_workdir / "response.json").at("cwd").get<std::string>()),
                            long_process.working_directory), "actual worker launch directory is disclosed");
#ifdef _WIN32
        test(long_process.working_directory.native().size() < 259,
             "Windows uses a short ancestor without changing absolute request paths");
#else
        test(fs::equivalent(long_process.working_directory, long_workdir),
             "non-Windows workers retain the requested working directory");
#endif
        checks += repro_tests(root / "repro-fixtures", b, exe);
        checks += minimize_tests(root / "minimize-fixtures", header(), exe);
        checks += pass_tests(root / "pass-fixtures", header(), exe);
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
        auto report_data = [&](const fs::path &file) {
            auto bytes = read_bytes(file);
            std::string html(bytes.begin(), bytes.end());
            const std::string marker = "<script id=report-data type=application/json>";
            auto start = html.find(marker);
            check(start != std::string::npos, "embedded report data exists");
            start += marker.size();
            return std::pair{
                html, json::parse(html.substr(start, html.find("</script>", start) - start))};
        };
        auto [html, report_doc] = report_data(root / "report.html");
        test(report_doc["cases"].size() == 1, "report retains dataset cases");
        test(!report_doc["cases"][0]["artifacts"].empty(), "report links available artifacts");
        test(html.find("Rendering / semantic correctness is unverified for every case") !=
                 std::string::npos,
             "report prominently discloses correctness limit");
        test(html.find("<details") == std::string::npos,
             "report does not hide evidence in disclosures");
        test(html.find("id=\"previous\"") == std::string::npos &&
                 html.find("id=\"next\"") == std::string::npos,
             "report has no pagination controls");
        test(html.find("role=\"listbox\"") != std::string::npos &&
                 html.find("role=\"tablist\"") != std::string::npos,
             "report includes accessible continuous list and evidence tabs");
        report(root / "dataset", {}, root / "extraction-only.html");
        auto extraction_report = report_data(root / "extraction-only.html").second;
        test(!extraction_report["has_run"].get<bool>() &&
                 extraction_report["cases"][0]["result"].empty(),
             "extraction-only reports do not fabricate verdicts");
        auto adversarial = d;
        auto id = adversarial["shaders"].begin().key();
        const std::string hostile = "</script><script>alert('report injection')</script>&\"";
        adversarial["shaders"][id]["origins"][0]["file"] = hostile;
        adversarial["shaders"][id]["origins"][0]["header_offset"] = UINT64_MAX;
        atomic_json(root / "dataset/manifest.json", adversarial);
        auto hostile_result = f;
        hostile_result["results"][id]["log_tail"] = hostile;
        hostile_result["results"][id]["artifacts"] = "../../outside";
        hostile_result["results"]["unrelated-case"] = {{"status", "spirv_valid_under_profile"}};
        atomic_json(root / "run/report-fixture.json", hostile_result);
        report(root / "dataset", root / "run/report-fixture.json", root / "hostile.html");
        auto [safe_html, safe_data] = report_data(root / "hostile.html");
        test(safe_html.find(hostile) == std::string::npos,
             "embedded report data cannot close script element");
        test(safe_data["cases"][0]["result"]["log_tail"] == hostile,
             "escaped diagnostics round-trip");
        test(safe_data["cases"][0]["artifacts"].empty(), "out-of-run artifact paths suppressed");
        test(safe_data["orphan_results"] == 1, "mismatched run cases counted separately");
        test(safe_data["cases"][0]["shader"]["origins"][0]["header_offset"] == "0xffffffffffffffff",
             "report offsets retain uint64 precision");
        write_text(root / "run/cases/space # quote'/worker.log", "fixture");
        hostile_result["results"][id]["artifacts"] = "cases/space # quote'";
        hostile_result["results"][id]["log_tail"] = std::string("invalid utf8: ") + char(0xff);
        atomic_json(root / "run/report-fixture.json", hostile_result);
        report(root / "dataset", root / "run/report-fixture.json", root / "encoded-links.html");
        auto encoded = report_data(root / "encoded-links.html").second;
        auto href = encoded["cases"][0]["artifacts"][0]["href"].get<std::string>();
        test(href.find("space%20%23%20quote%27/worker.log") != std::string::npos,
             "artifact URL encodes spaces and markup delimiters");
        test(encoded["cases"][0]["result"]["log_tail"].get<std::string>().find("\xef\xbf\xbd") !=
                 std::string::npos,
             "non-UTF8 worker diagnostics rendered with replacement");
        auto empty_manifest = d;
        empty_manifest["shaders"] = json::object();
        empty_manifest["files"] = json::object();
        atomic_json(root / "dataset/manifest.json", empty_manifest);
        report(root / "dataset", {}, root / "empty.html");
        test(report_data(root / "empty.html").second["cases"].empty(), "empty report generation");
        atomic_json(root / "dataset/manifest.json", d);
        test(cluster(root / "run/results.json")["groups"].size() == 1, "failure grouping");
        test(compare(root / "run/results.json", root / "run/results.json")["changes"].empty(),
             "identical regression comparison");
        auto campaign_path = root / "campaign-plan.json";
        json campaign_plan = {
            {"schema", 1},
            {"contexts", json::array({
                {{"name", "success"}, {"profile", {{"test_mode", "ok"}}}},
                {{"name", "../failure"}, {"profile", {{"test_mode", "error"}}}}
            })}};
        atomic_json(campaign_path, campaign_plan);
        RunOptions campaign_options{root / "dataset", root / "campaign", exe, {}, 2, 5000, 0, true};
        auto campaign_result = campaign(campaign_options, campaign_path);
        test(campaign_result["state"] == "completed" && campaign_result["contexts"].size() == 2,
             "campaign executes every named context");
        test(campaign_result["contexts"][0]["status_counts"].value("fixture_pass", 0) == 1 &&
                 campaign_result["contexts"][1]["status_counts"].value("worker_crash_or_error", 0) == 1,
             "campaign retains per-context failures without aborting");
        test(campaign_result["context_sensitive_cases"].size() == 1 &&
                 campaign_result["semantic_correctness"] == "not_tested",
             "campaign differences do not claim semantic validation");
        auto campaign_results_path = root / "campaign" /
            path_from(campaign_result["contexts"][1]["results"].get<std::string>());
        test(is_within(campaign_results_path, root / "campaign") && fs::exists(campaign_results_path),
             "untrusted context names never become output paths");
        campaign_result = campaign(campaign_options, campaign_path);
        for (const auto &context : campaign_result["cases"].begin().value())
            test(context["cache_hit"] == true, "campaign resumes each context independently");
        auto original_campaign_key = campaign_result["campaign_key"];
        campaign_options.resume = false;
        campaign_result = campaign(campaign_options, campaign_path);
        for (const auto &context : campaign_result["cases"].begin().value())
            test(context["cache_hit"] == false, "campaign no-resume reruns each context");
        campaign_options.resume = true;
        campaign_plan["contexts"][1]["profile"]["test_mode"] = "ok";
        atomic_json(campaign_path, campaign_plan);
        campaign_result = campaign(campaign_options, campaign_path);
        test(campaign_result["campaign_key"] != original_campaign_key &&
                 campaign_result["context_sensitive_cases"].empty(),
             "changed plan gets fresh identity and no stale differences");
        auto reject_campaign = [&](const json &plan, const RunOptions &selected, const char *name) {
            atomic_json(campaign_path, plan);
            bool rejected = false;
            try { campaign(selected, campaign_path); }
            catch (const std::exception &) { rejected = true; }
            test(rejected, name);
        };
        auto invalid_campaign = campaign_plan;
        invalid_campaign["contexts"][1]["name"] = "success";
        reject_campaign(invalid_campaign, campaign_options, "duplicate campaign context rejected");
        invalid_campaign = campaign_plan;
        invalid_campaign["contexts"][0]["profile"] = "profile.json";
        reject_campaign(invalid_campaign, campaign_options, "non-inline campaign profile rejected");
        invalid_campaign = campaign_plan;
        invalid_campaign["contexts"][0]["profile"] = {{"default", json::object()},
                                                     {"cases", {{"missing", json::object()}}}};
        reject_campaign(invalid_campaign, campaign_options, "unknown campaign case rejected");
        invalid_campaign = campaign_plan;
        invalid_campaign["contexts"] = json::array();
        reject_campaign(invalid_campaign, campaign_options, "empty campaign rejected");
        invalid_campaign = campaign_plan;
        invalid_campaign["unknown"] = 1;
        reject_campaign(invalid_campaign, campaign_options, "unknown campaign option rejected");
        auto invalid_options = campaign_options;
        invalid_options.output = root / "dataset";
        reject_campaign(campaign_plan, invalid_options, "campaign cannot overwrite dataset");
        invalid_options.output = root / "games";
        reject_campaign(campaign_plan, invalid_options, "campaign cannot overwrite game input");
        invalid_options = campaign_options;
        invalid_options.timeout_ms = 0;
        reject_campaign(campaign_plan, invalid_options, "zero campaign timeout rejected");
        fs::create_directory(root / "campaign/.campaign-lock");
        reject_campaign(campaign_plan, campaign_options, "concurrent campaign writer rejected");
        fs::remove(root / "campaign/.campaign-lock");
        if (argc == 3 && std::string(argv[1]) == "--real-worker") {
            atomic_json(profile, {{"schema", 1}, {"stage", "CS"}, {"mode", "header_probe"}});
            options.worker = fs::absolute(path_from(argv[2]));
            auto info_process = process(options.worker, {"--compiler-info"}, root,
                                        root / "compiler-info.json", std::chrono::seconds(10));
            test(info_process.exit_code == 0 && !info_process.timed_out, "compiler library identity query");
            auto compiler_info = read_json(root / "compiler-info.json");
            test(compiler_info.at("compiler_interface") == 1 && compiler_info.at("worker_protocol") == 1 &&
                 compiler_info.at("link_mode") == "compiler_library" &&
                 compiler_info.at("upstream_source_count").get<unsigned>() > 4,
                 "worker exposes the versioned compiler-library boundary");
            test(compiler_info.at("upstream_source_count").get<unsigned>() <
                 compiler_info.at("upstream_full_test_source_count").get<unsigned>(),
                 "compiler-only source set excludes the full emulator test closure");
            options.timeout_ms = 10000;
            options.output = root / "real-run";
            auto real = run(options);
            test(real["status_counts"].value("spirv_valid_under_profile", 0) == 1,
                 "real Kyty end-program SPIR-V validation");
            checks += minimize_real_tests(root / "real-minimization", header(), options.worker);
            checks += pass_real_tests(root / "real-passes", header(), options.worker);
            checks += capture_real_tests(root / "real-capture", header(), options.worker);
        }
        std::cout << "PASS: " << checks << " checks (fixtures; no guest/GPU conformance)\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
