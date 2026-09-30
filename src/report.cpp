#include "report_ui.hpp"
#include "shader_lab/lab.hpp"
#include <map>
#include <set>

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
    json data = {
        {"root", manifest.value("root", "")},
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
        {"semantic_issues", run.value("semantics", json::object()).value("issues", json::array())},
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
                      {"semantic", run.value("semantics", json::object())
                                       .value("cases", json::object())
                                       .value(it.key(), json::object())},
                      {"artifacts", json::array()}};
        const auto artifact_dir = entry["result"].value("artifacts", "");
        if (!results.empty() && !artifact_dir.empty()) {
            // Never turn attacker-supplied paths into links to arbitrary local files.
            const auto dir = fs::weakly_canonical(run_root / path_from(artifact_dir));
            if (is_within(dir, allowed_artifacts)) {
                for (const auto *name : {"request.json",
                                         "response.json",
                                         "result.json",
                                         "phase.json",
                                         "worker.log",
                                         "guest.asm",
                                         "instructions.json",
                                         "cfg.json",
                                         "cfg.dot",
                                         "native-cfg.txt",
                                         "cfg.txt",
                                         "translated.ir",
                                         "header-registers.json",
                                         "memory-reads.json",
                                         "final.ir",
                                         "shader.spv",
                                         "shader.spvasm",
                                         "pass-trace.json",
                                         "pass-stop.ir",
                                         "captured-state.json",
                                         "host-assessment.json",
                                         "compiler-layout.json"}) {
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
        if (entry["semantic"].contains("tests") && !results.empty()) {
            const auto semantic_root = fs::weakly_canonical(run_root.parent_path() / "semantic");
            entry["semantic"]["generation_links"] = json::array();
            if (entry["semantic"].contains("generation_artifacts")) {
                const auto dir = fs::weakly_canonical(
                    semantic_root /
                    path_from(entry["semantic"]["generation_artifacts"].get<std::string>()));
                for (const auto *name :
                     {"generation.json", "generator.log", "generate-request.json"}) {
                    const auto file = dir / name;
                    if (is_within(file, semantic_root) && fs::is_regular_file(file))
                        entry["semantic"]["generation_links"].push_back(
                            {{"name", name}, {"href", artifact_url(file, output)}});
                }
            }
            for (auto &test : entry["semantic"]["tests"]) {
                test["links"] = json::array();
                if (test.contains("generated_fixture")) {
                    const auto dir = fs::weakly_canonical(
                        semantic_root / path_from(test["generated_fixture"].get<std::string>()));
                    for (const auto *name :
                         {"fixture.json", "reference.json", "model-trace.json"}) {
                        const auto file = dir / name;
                        if (is_within(file, semantic_root) && fs::is_regular_file(file))
                            test["links"].push_back(
                                {{"name", name}, {"href", artifact_url(file, output)}});
                    }
                }
                if (!test.contains("artifacts"))
                    continue;
                const auto dir = fs::weakly_canonical(
                    semantic_root / path_from(test["artifacts"].get<std::string>()));
                if (!is_within(dir, semantic_root))
                    continue;
                for (const auto *name : {"execution.json", "comparison.json", "backend/worker.log",
                                         "backend/replay-trace.json", "reference/record.json",
                                         "observed/record.json"}) {
                    const auto file = dir / name;
                    if (fs::is_regular_file(file) && is_within(file, semantic_root))
                        test["links"].push_back(
                            {{"name", name}, {"href", artifact_url(file, output)}});
                }
            }
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
    // New scans carry an eboot-validated metadata snapshot. Legacy folder labels are not
    // evidence of a game: re-identify only their recorded source paths against actual eboots.
    json metadata, owners = json::object();
    if (manifest.value("game_identification", "") == "eboot-param-v1" &&
        manifest.contains("games") && manifest.at("games").is_object()) {
        metadata = manifest.at("games");
        for (auto it = manifest["files"].begin(); it != manifest["files"].end(); ++it) {
            const auto key = it.value().value("game", "");
            owners[it.key()] = metadata.contains(key) ? key : "";
        }
        data["game_identification_source"] = "scan_snapshot";
    } else {
        std::vector<fs::path> sources;
        const auto root = fs::weakly_canonical(path_from(manifest.value("root", "")));
        for (auto it = manifest["files"].begin(); it != manifest["files"].end(); ++it)
            sources.push_back(root / path_from(it.key()));
        const auto catalog = identify_games(root, sources);
        metadata = catalog.at("games");
        owners = catalog.at("owners");
        data["game_identification_source"] = "source_eboot_and_param_refresh";
    }
    std::map<std::string, json> games;
    for (auto it = metadata.begin(); it != metadata.end(); ++it) {
        auto game = it.value();
        game["key"] = it.key();
        game["file_count"] = 0;
        game["case_ids"] = json::array();
        game["gap_indices"] = json::array();
        game["scan_limited"] = false;
        game["traversal_errors"] = json::array();
        games[it.key()] = std::move(game);
    }
    data["non_game_files"] = 0;
    data["unassigned_shader_cases"] = 0;
    for (auto it = manifest["files"].begin(); it != manifest["files"].end(); ++it) {
        const auto owner = owners.value(it.key(), "");
        if (!games.contains(owner)) {
            data["non_game_files"] = data["non_game_files"].get<size_t>() + 1;
            continue;
        }
        const auto &file = it.value();
        auto &game = games.at(owner);
        game["file_count"] = game["file_count"].get<size_t>() + 1;
        if (file.value("status", "") == "size_limit" ||
            file.value("status", "") == "candidate_limit" ||
            file.value("status", "") == "extraction_limit")
            game["scan_limited"] = true;
    }
    for (auto &entry : data["cases"]) {
        std::set<std::string> case_owners;
        if (entry["shader"].contains("origins"))
            for (auto &origin : entry["shader"]["origins"]) {
                const auto owner = owners.value(origin.value("file", ""), "");
                origin["game"] = owner;
                if (games.contains(owner))
                    case_owners.insert(owner);
            }
        if (case_owners.empty())
            data["unassigned_shader_cases"] = data["unassigned_shader_cases"].get<size_t>() + 1;
        for (const auto &owner : case_owners)
            games.at(owner)["case_ids"].push_back(entry["id"]);
    }
    for (size_t i = 0; i < data["gaps"].size(); ++i) {
        const auto owner = owners.value(data["gaps"][i].value("file", ""), "");
        if (games.contains(owner))
            games.at(owner)["gap_indices"].push_back(i);
    }
    data["unassigned_traversal_errors"] = json::array();
    for (const auto &error : data["traversal_errors"]) {
        std::string owner;
        if (error.contains("path")) {
            const auto relative =
                path_text(path_from(error["path"].get<std::string>())
                              .lexically_relative(path_from(manifest.value("root", ""))));
            for (const auto &[name, _] : games)
                if ((name == "." || relative == name || relative.starts_with(name + "/")) &&
                    name.size() > owner.size())
                    owner = name;
        }
        if (owner.empty())
            data["unassigned_traversal_errors"].push_back(error);
        else
            games.at(owner)["traversal_errors"].push_back(error);
    }
    data["games"] = json::array();
    for (const auto &[_, game] : games)
        data["games"].push_back(game);
    std::string html(report_head);
    html += "<script id=report-data type=application/json>" + embedded_json(data) + "</script>";
    html += report_script;
    write_text(output, html);
}
} // namespace sl
