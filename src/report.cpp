#include "report_ui.hpp"
#include "shader_lab/lab.hpp"

namespace sl {
namespace {
// JSON in a script element needs HTML-safe escaping even when its type is application/json.
std::string embedded_json(const json &value) {
    auto text = value.dump(-1, ' ', true, json::error_handler_t::replace);
    std::string safe;
    safe.reserve(text.size());
    for (char c : text) {
        if (c == '<')
            safe += "\\u003c";
        else if (c == '>')
            safe += "\\u003e";
        else if (c == '&')
            safe += "\\u0026";
        else
            safe += c;
    }
    return safe;
}
std::string artifact_url(const fs::path &file, const fs::path &output) {
    auto path = fs::absolute(file).lexically_normal();
    auto relative = path.lexically_relative(fs::absolute(output).parent_path());
    std::string text = path_text(relative.empty() ? path : relative), encoded;
    constexpr char digits[] = "0123456789ABCDEF";
    for (unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '/' || c == '-' || c == '_' || c == '.' || c == '~' ||
            (relative.empty() && c == ':'))
            encoded += char(c);
        else {
            encoded += '%';
            encoded += digits[c >> 4];
            encoded += digits[c & 15];
        }
    }
    if (relative.empty())
        return path.has_root_name() ? "file:///" + encoded : "file://" + encoded;
    return encoded;
}
} // namespace

void report(const fs::path &dataset, const fs::path &results, const fs::path &output) {
    const auto manifest = read_json(dataset / "manifest.json");
    const auto run = results.empty() ? json::object() : read_json(results);
    const auto rows = run.value("results", json::object());
    if (!manifest.at("shaders").is_object() || !manifest.at("files").is_object() ||
        !rows.is_object())
        throw std::runtime_error("report requires object-shaped shaders, files and results");
    json data = {{"root", manifest.value("root", "")},
                 {"file_count", manifest["files"].size()},
                 {"scan_limited", manifest.value("limited", false)},
                 {"scan_in_progress", manifest.value("in_progress", false)},
                 {"coverage", manifest.value("coverage", "")},
                 {"extractor_id", manifest.value("extractor_id", "")},
                 {"has_run", !results.empty()},
                 {"run_limited", run.value("limited", false)},
                 {"worker_sha256", run.value("worker_sha256", "")},
                 {"profile_sha256", run.value("profile_sha256", "")},
                 {"profile", run.value("profile", json::object())},
                 {"cases", json::array()},
                 {"gaps", json::array()},
                 {"traversal_errors", manifest.value("traversal_errors", json::array())},
                 {"orphan_results", 0}};
    size_t orphan_count = 0;
    for (auto it = rows.begin(); it != rows.end(); ++it)
        if (!manifest["shaders"].contains(it.key()))
            ++orphan_count;
    data["orphan_results"] = orphan_count;
    const auto run_root =
        results.empty() ? fs::path{} : fs::weakly_canonical(results.parent_path());
    const auto allowed_artifacts = run_root / "cases";
    for (auto it = manifest["shaders"].begin(); it != manifest["shaders"].end(); ++it) {
        json entry = {{"id", it.key()},
                      {"shader", it.value()},
                      {"result", rows.value(it.key(), json::object())},
                      {"artifacts", json::array()}};
        const auto artifact_dir = entry["result"].value("artifacts", "");
        if (!results.empty() && !artifact_dir.empty()) {
            // Never turn attacker-supplied paths into links to arbitrary local files.
            const auto dir = fs::weakly_canonical(run_root / path_from(artifact_dir));
            if (is_within(dir, allowed_artifacts)) {
                for (const auto *name :
                     {"request.json", "response.json", "result.json", "phase.json", "worker.log",
                      "guest.asm", "instructions.json", "cfg.json", "cfg.dot", "native-cfg.txt",
                      "cfg.txt", "translated.ir", "header-registers.json", "memory-reads.json",
                      "final.ir", "shader.spv", "shader.spvasm", "pass-trace.json", "pass-stop.ir"}) {
                    auto file = dir / name;
                    std::error_code ec;
                    if (fs::is_regular_file(file, ec) &&
                        is_within(fs::weakly_canonical(file), allowed_artifacts))
                        entry["artifacts"].push_back(
                            {{"name", name}, {"href", artifact_url(file, output)}});
                }
            } else
                entry["artifact_warning"] =
                    "Artifact path is outside the run's cases directory; links omitted.";
        }
        // Offsets may exceed JavaScript's exact integer range. Preserve them as hexadecimal
        // strings.
        if (entry["shader"].contains("origins"))
            for (auto &origin : entry["shader"]["origins"])
                for (auto key : {"header_offset", "code_offset"})
                    if (origin.contains(key) && origin[key].is_number_unsigned())
                        origin[key] = "0x" + hex(origin[key].get<uint64_t>());
        data["cases"].push_back(std::move(entry));
    }
    for (auto it = manifest["files"].begin(); it != manifest["files"].end(); ++it)
        if (it.value().value("status", "") != "scanned" ||
            !it.value().value("findings", json::array()).empty())
            data["gaps"].push_back({{"file", it.key()}, {"record", it.value()}});
    std::string html(report_head);
    html += "<script id=report-data type=application/json>" + embedded_json(data) + "</script>";
    html += report_script;
    write_text(output, html);
}
} // namespace sl
