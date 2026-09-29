#include "shader_lab/lab.hpp"
#include <atomic>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>
namespace sl {
namespace {
std::string digest_text(std::string_view s) {
    return sha256({reinterpret_cast<const uint8_t *>(s.data()), s.size()});
}
void validate_id(std::string_view s) {
    if (s.size() != 129 || s[64] != '-' ||
        s.find_first_not_of("0123456789abcdef-") != std::string_view::npos)
        throw std::runtime_error("invalid object identity");
}
} // namespace
json run(const RunOptions &o) {
    if (!o.jobs || o.jobs > 64)
        throw std::runtime_error("jobs must be 1..64");
    auto dataset = fs::canonical(o.dataset), worker = fs::canonical(o.worker),
         out = fs::weakly_canonical(o.output);
    auto manifest = read_json(dataset / "manifest.json");
    if (manifest.value("schema", 0) != schema_version)
        throw std::runtime_error("unsupported dataset schema");
    if (is_within(out, path_from(manifest.at("root").get<std::string>())))
        throw std::runtime_error("run output must not be inside game input");
    fs::create_directories(out);
    if (!fs::create_directory(out / ".run-lock"))
        throw std::runtime_error(
            "run directory locked; confirm no runner before removing .run-lock");
    struct Lock {
        fs::path p;
        ~Lock() {
            std::error_code e;
            fs::remove(p, e);
        }
    } lock{out / ".run-lock"};
    json profile = o.profile.empty()
                       ? json{{"schema", 1}, {"mode", "header_probe"}, {"host_subgroup_size", 32}}
                       : read_json(o.profile);
    if (profile.contains("cases")) {
        if (!profile["cases"].is_object() || !profile.contains("default"))
            throw std::runtime_error("profile bundle requires default and cases objects");
        for (auto it = profile["cases"].begin(); it != profile["cases"].end(); ++it)
            if (!manifest["shaders"].contains(it.key()))
                throw std::runtime_error("profile bundle contains an unknown case ID");
    }
    auto worker_hash = hash_file(worker), profile_hash = digest_text(profile.dump());
    json summary = {{"schema", 1},
                    {"worker_sha256", worker_hash},
                    {"profile_sha256", profile_hash},
                    {"profile", profile},
                    {"semantic_correctness", "not_tested"},
                    {"results", json::object()}};
    std::vector<std::string> ids;
    for (auto it = manifest["shaders"].begin(); it != manifest["shaders"].end(); ++it) {
        validate_id(it.key());
        ids.push_back(it.key());
    }
    if (o.limit && ids.size() > o.limit)
        ids.resize(size_t(o.limit));
    std::atomic<size_t> next = 0, done = 0;
    std::mutex mutex;
    std::exception_ptr failure;
    auto task = [&] {
        while (true) {
            auto i = next.fetch_add(1);
            if (i >= ids.size())
                break;
            const auto &id = ids[i];
            try {
                const auto &selected_profile =
                    profile.contains("cases")
                        ? (profile["cases"].contains(id) ? profile["cases"][id]
                                                         : profile["default"])
                        : profile;
                auto selected_hash = digest_text(selected_profile.dump());
                auto key = digest_text(id + worker_hash + selected_hash +
                                       std::to_string(o.timeout_ms) + "protocol-1");
                auto dir = out / "cases" / key;
                fs::create_directories(dir);
                json r;
                auto object = dataset / "objects" / id;
                if (hash_file(object / "header.bin") !=
                        manifest["shaders"][id]["header_sha256"].get<std::string>() ||
                    hash_file(object / "code.bin") !=
                        manifest["shaders"][id]["code_sha256"].get<std::string>())
                    throw std::runtime_error("object content hash mismatch");
                if (o.resume && fs::exists(dir / "result.json")) {
                    r = read_json(dir / "result.json");
                    if (r.value("id", "") != id || r.value("cache_key", "") != key)
                        throw std::runtime_error("cached response identity mismatch");
                    r["cache_hit"] = true;
                } else {
                    json request = {{"schema", 1},
                                    {"id", id},
                                    {"header", path_text(object / "header.bin")},
                                    {"code", path_text(object / "code.bin")},
                                    {"profile", selected_profile},
                                    {"output", path_text(dir)}};
                    atomic_json(dir / "request.json", request);
                    // Old partial output must never be mistaken for this attempt's response.
                    auto response = dir / "response.json";
                    if (fs::exists(response)) {
                        auto previous = dir / "previous-response.json";
                        std::error_code ec;
                        fs::remove(previous, ec);
                        fs::rename(response, previous);
                    }
                    auto p = process(worker, {"--request", path_text(dir / "request.json")}, dir,
                                     dir / "worker.log", std::chrono::milliseconds(o.timeout_ms));
                    r = {{"id", id},
                         {"cache_key", key},
                         {"profile_sha256", selected_hash},
                         {"exit_code", p.exit_code},
                         {"elapsed_ms", p.elapsed_ms},
                         {"cache_hit", false},
                         {"semantic_correctness", "not_tested"}};
                    if (p.timed_out)
                        r["status"] = "timeout";
                    else if (p.exit_code != 0)
                        r["status"] = "worker_crash_or_error";
                    else if (!fs::exists(response))
                        r["status"] = "worker_protocol_error";
                    else {
                        auto response_doc = read_json(response);
                        if (response_doc.value("schema", 0) != 1 ||
                            response_doc.value("id", "") != id || !response_doc.contains("status"))
                            r["status"] = "worker_protocol_error";
                        else {
                            r["status"] = response_doc["status"];
                            r["details"] = response_doc;
                        }
                    }
                    if (fs::exists(dir / "phase.json"))
                        r["last_phase"] = read_json(dir / "phase.json");
                    std::ifstream log(dir / "worker.log", std::ios::binary | std::ios::ate);
                    auto length = log.tellg();
                    auto tail_size = length > 8192 ? size_t(8192) : length > 0 ? size_t(length) : 0;
                    std::string tail(tail_size, '\0');
                    log.seekg(-std::streamoff(tail_size), std::ios::end);
                    log.read(tail.data(), std::streamsize(tail_size));
                    r["log_tail"] = tail;
                    atomic_json(dir / "result.json", r);
                }
                r["artifacts"] = path_text(fs::relative(dir, out));
                std::lock_guard guard(mutex);
                summary["results"][id] = r;
                std::cout << "[" << ++done << "/" << ids.size() << "] "
                          << manifest["shaders"][id]["kyty_hash"] << " " << r["status"] << "\n"
                          << std::flush;
                atomic_json(out / "results.json", summary);
            } catch (const std::exception &ex) {
                std::lock_guard guard(mutex);
                summary["results"][id] = {
                    {"id", id}, {"status", "runner_error"}, {"error", ex.what()}};
                std::cerr << "runner error: " << ex.what() << "\n";
            }
        }
    };
    std::vector<std::thread> pool;
    for (unsigned i = 0; i < o.jobs; ++i)
        pool.emplace_back(task);
    for (auto &t : pool)
        t.join();
    summary["status_counts"] = json::object();
    for (const auto &r : summary["results"]) {
        auto status = r.value("status", "unknown");
        summary["status_counts"][status] = summary["status_counts"].value(status, 0) + 1;
    }
    summary["selected_cases"] = ids.size();
    summary["dataset_cases"] = manifest["shaders"].size();
    summary["limited"] = ids.size() != manifest["shaders"].size();
    atomic_json(out / "results.json", summary);
    return summary;
}
json compare(const fs::path &before, const fs::path &after) {
    auto a = read_json(before), b = read_json(after);
    json r = {{"schema", 1},
              {"before_worker", a.value("worker_sha256", "")},
              {"after_worker", b.value("worker_sha256", "")},
              {"profiles_match", a.value("profile_sha256", "") == b.value("profile_sha256", "")},
              {"changes", json::array()}};
    std::set<std::string> keys;
    for (auto it = a["results"].begin(); it != a["results"].end(); ++it)
        keys.insert(it.key());
    for (auto it = b["results"].begin(); it != b["results"].end(); ++it)
        keys.insert(it.key());
    for (const auto &k : keys) {
        auto x = a["results"].contains(k) ? a["results"][k].value("status", "unknown") : "absent";
        auto y = b["results"].contains(k) ? b["results"][k].value("status", "unknown") : "absent";
        auto sx = a["results"].contains(k)
                      ? a["results"][k].value("details", json::object()).value("spirv_sha256", "")
                      : "";
        auto sy = b["results"].contains(k)
                      ? b["results"][k].value("details", json::object()).value("spirv_sha256", "")
                      : "";
        if (x != y || sx != sy)
            r["changes"].push_back(
                {{"id", k}, {"before", x}, {"after", y}, {"spirv_changed", sx != sy}});
    }
    return r;
}
void report(const fs::path &dataset, const fs::path &results, const fs::path &output) {
    auto d = read_json(dataset / "manifest.json");
    json r = results.empty() ? json::object() : read_json(results);
    std::string h =
        "<!doctype html><meta charset=utf-8><title>PS5 Shader Lab</title><style>body{font:15px "
        "system-ui;background:#121823;color:#e0e6f0;margin:2rem}input{padding:.6rem;width:70%}"
        "table{border-collapse:collapse;width:100%}td,th{border:1px solid "
        "#42536a;padding:.5rem;text-align:left;vertical-align:top}code{word-break:break-all}.note{"
        "padding:1rem;background:#253249}</style><h1>PS5 Shader Lab</h1><p class=note>Offline "
        "extraction / compiler regression evidence. SPIR-V validation is not semantic correctness. "
        "Compressed, encrypted and dynamically generated shaders may be absent.</p>";
    h += "<p>Files: " + std::to_string(d["files"].size()) +
         " · Unique header+code cases: " + std::to_string(d["shaders"].size()) +
         " · Scan limited: " + d["limited"].dump() +
         "</p><input id=q placeholder='Filter by game, hash, stage or "
         "outcome'><table><thead><tr><th>Game / source</th><th>Kyty hash / "
         "stage</th><th>Outcome</th><th>Evidence</th></tr></thead><tbody>";
    for (auto it = d["shaders"].begin(); it != d["shaders"].end(); ++it) {
        auto &s = it.value();
        std::string sources, evidence;
        for (const auto &o : s["origins"]) {
            sources += o["game"].get<std::string>() + " / " + o["file"].get<std::string>() +
                       " @0x" + hex(o["header_offset"].get<uint64_t>()) + "\n";
            evidence += o["method"].get<std::string>() + " ";
        }
        auto status = r.contains("results") && r["results"].contains(it.key())
                          ? r["results"][it.key()].value("status", "unknown")
                          : "not tested";
        h += "<tr><td>" + html_escape(sources) + "</td><td><code>" +
             html_escape(s["kyty_hash"].get<std::string>()) + "</code> " +
             html_escape(s["type"].get<std::string>()) + "</td><td>" + html_escape(status) +
             "</td><td>" + html_escape(evidence) + "</td></tr>";
    }
    h += "</tbody></table><h2>Coverage gaps / rejected candidates</h2><pre>";
    for (auto it = d["files"].begin(); it != d["files"].end(); ++it)
        if (it.value().value("status", "") != "scanned" ||
            !it.value().value("findings", json::array()).empty())
            h += html_escape(it.key() + ": " + it.value().dump()) + "\n";
    h += html_escape(d.value("traversal_errors", json::array()).dump(2)) +
         "</pre><script>document.querySelector('#q').addEventListener('input',e=>{const "
         "q=e.target.value.toLowerCase();document.querySelectorAll('tbody "
         "tr').forEach(r=>r.hidden=!r.textContent.toLowerCase().includes(q))})</script>";
    write_text(output, h);
}
} // namespace sl
