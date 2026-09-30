#include "shader_lab/lab.hpp"
#include <array>

namespace sl {
namespace {
constexpr uint64_t file_limit = 64 * 1024 * 1024;
constexpr uint64_t artifact_limit = 128 * 1024 * 1024;
bool digest(std::string_view value) {
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == value.npos;
}
void check_id(const std::string &id) {
    if (id.size() != 129 || id[64] != '-' || !digest(id.substr(0, 64)) || !digest(id.substr(65)))
        throw std::runtime_error("repro requires a complete header-code SHA-256 case ID");
}
json bounded_json(const fs::path &path) {
    auto bytes = read_bytes(path, file_limit);
    return json::parse(bytes.begin(), bytes.end());
}
fs::path contained(const fs::path &root, const fs::path &relative) {
    if (relative.empty() || relative.is_absolute() || relative.has_root_name())
        throw std::runtime_error("repro evidence path must be relative");
    auto path = fs::canonical(root / relative);
    if (!is_within(path, root) || !fs::is_regular_file(path))
        throw std::runtime_error("repro evidence escapes its source directory");
    return path;
}
void disjoint(const fs::path &out, const fs::path &input) {
    if (is_within(out, input) || is_within(input, out))
        throw std::runtime_error("repro output must not overlap its inputs");
}
void fresh_directory(const fs::path &out) {
    fs::create_directories(out.parent_path());
    if (!fs::create_directory(out))
        throw std::runtime_error("repro requires a fresh output directory");
}
std::string json_hash(const json &value) {
    auto text = value.dump();
    return sha256({reinterpret_cast<const uint8_t *>(text.data()), text.size()});
}
std::vector<uint8_t> verified(const fs::path &root, const fs::path &path,
                              const std::string &expected) {
    auto bytes = read_bytes(contained(root, path), file_limit);
    if (!digest(expected) || sha256(bytes) != expected)
        throw std::runtime_error("repro content hash mismatch: " + path_text(path));
    return bytes;
}
} // namespace

json export_repro(const fs::path &dataset_path, const fs::path &results_path,
                  const std::string &id, const fs::path &output) {
    check_id(id);
    auto dataset = fs::canonical(dataset_path), results = fs::canonical(results_path);
    auto out = fs::weakly_canonical(output);
    disjoint(out, dataset);
    disjoint(out, results.parent_path());
    auto manifest = bounded_json(dataset / "manifest.json"), summary = bounded_json(results);
    if (manifest.value("schema", 0) != 1 || summary.value("schema", 0) != 1)
        throw std::runtime_error("unsupported repro source schema");
    disjoint(out, path_from(manifest.at("root").get<std::string>()));
    auto shader = manifest.at("shaders").at(id), row = summary.at("results").at(id);
    if (row.at("id") != id || shader.at("header_sha256") != id.substr(0, 64) ||
        shader.at("code_sha256") != id.substr(65))
        throw std::runtime_error("repro source case identity mismatch");
    const auto &profiles = summary.at("profile");
    if (json_hash(profiles) != summary.at("profile_sha256").get<std::string>())
        throw std::runtime_error("repro source profile bundle hash mismatch");
    json profile = profiles.contains("cases")
                       ? (profiles.at("cases").contains(id) ? profiles.at("cases").at(id)
                                                            : profiles.at("default"))
                       : profiles;
    if (!profile.is_object() || json_hash(profile) != row.at("profile_sha256").get<std::string>() ||
        !digest(summary.at("worker_sha256").get<std::string>()))
        throw std::runtime_error("repro requires a verified selected profile and worker identity");
    auto object = fs::path("objects") / id;
    auto header = verified(dataset, object / "header.bin", id.substr(0, 64));
    auto code = verified(dataset, object / "code.bin", id.substr(65));
    // A bundle is committed by writing repro.json last. Incomplete exports remain visible,
    // but cannot be replayed, and are never recursively deleted on the user's behalf.
    fresh_directory(out);
    write_bytes(out / "header.bin", header);
    write_bytes(out / "code.bin", code);
    atomic_json(out / "profile.json", profile);
    atomic_json(out / "original-result.json", row);
    atomic_json(out / "provenance.json", {{"shader", shader},
                {"source_manifest_sha256", hash_file(dataset / "manifest.json")},
                {"source_results_sha256", hash_file(results)}});
    json bundle = {{"schema", 1}, {"kind", "shader_lab_repro"}, {"id", id},
                   {"worker_sha256", summary.at("worker_sha256")},
                   {"timeout_ms", summary.value("timeout_ms", json(nullptr))},
                   {"files", json::object()}, {"artifacts", json::object()},
                   {"artifact_gaps", json::array()},
                   {"replay_state", "not_run"}, {"semantic_correctness", "not_tested"},
                   {"warning", "Contains original shader bytes, paths and diagnostics. Review redistribution rights. "
                               "Captured artifacts may be stale from earlier retries; only a fresh replay is new evidence."}};
    for (auto name : {"header.bin", "code.bin", "profile.json", "original-result.json", "provenance.json"})
        bundle["files"][name] = hash_file(out / name);
    constexpr std::array names = {"worker.log", "phase.json", "response.json", "instructions.json",
        "guest.asm", "native-cfg.txt", "cfg.dot", "cfg.json", "header-registers.json", "cfg.txt",
        "translated.ir", "memory-reads.json", "final.ir", "shader.spv", "shader.spvasm",
        "pass-trace.json", "pass-stop.ir", "captured-state.json"};
    uint64_t copied = 0;
    if (row.contains("artifacts")) {
        auto relative = path_from(row.at("artifacts").get<std::string>());
        auto source = fs::weakly_canonical(results.parent_path() / relative);
        auto cases = fs::weakly_canonical(results.parent_path() / "cases");
        if (relative.is_absolute() || relative.has_root_name() ||
            !is_within(cases, results.parent_path()) || !is_within(source, cases))
            throw std::runtime_error("repro artifact directory escapes the source cases tree");
        for (auto name : names) {
            if (!fs::exists(source / name))
                continue;
            auto path = contained(source, name);
            auto size = fs::file_size(path);
            if (size > file_limit || size > artifact_limit - copied) {
                bundle["artifact_gaps"].push_back({{"file", name}, {"reason", "size_budget"}});
                continue;
            }
            auto bytes = read_bytes(path, size);
            copied += bytes.size();
            write_bytes(out / "evidence" / name, bytes);
            bundle["artifacts"][name] = sha256(bytes);
        }
    }
    atomic_json(out / "repro.json", bundle);
    return bundle;
}

json replay_repro(const fs::path &bundle_path, const fs::path &worker_path,
                  const fs::path &output, uint64_t timeout_ms) {
    auto root = fs::canonical(bundle_path), worker = fs::canonical(worker_path);
    auto out = fs::weakly_canonical(output);
    disjoint(out, root);
    disjoint(out, worker);
    auto bundle = bounded_json(contained(root, "repro.json"));
    if (bundle.value("schema", 0) != 1 || bundle.value("kind", "") != "shader_lab_repro")
        throw std::runtime_error("unsupported repro bundle");
    auto id = bundle.at("id").get<std::string>();
    check_id(id);
    if (bundle.at("files").at("header.bin") != id.substr(0, 64) ||
        bundle.at("files").at("code.bin") != id.substr(65))
        throw std::runtime_error("repro bundle case identity mismatch");
    auto bytes = [&](const char *name) {
        return verified(root, name, bundle.at("files").at(name).get<std::string>());
    };
    auto header = bytes("header.bin"), code = bytes("code.bin");
    auto profile_bytes = bytes("profile.json"), original_bytes = bytes("original-result.json");
    auto provenance_bytes = bytes("provenance.json");
    auto provenance = json::parse(provenance_bytes.begin(), provenance_bytes.end());
    auto profile = json::parse(profile_bytes.begin(), profile_bytes.end());
    auto original = json::parse(original_bytes.begin(), original_bytes.end());
    auto shader = provenance.at("shader");
    if (shader.at("header_sha256") != id.substr(0, 64) || shader.at("code_sha256") != id.substr(65))
        throw std::runtime_error("repro provenance case identity mismatch");
    if (!profile.is_object() || original.at("id") != id || !original.at("status").is_string() ||
        !digest(bundle.at("worker_sha256").get<std::string>()) ||
        json_hash(profile) != original.at("profile_sha256").get<std::string>())
        throw std::runtime_error("repro profile or original result identity mismatch");
    auto recorded_timeout = bundle.at("timeout_ms");
    if (!timeout_ms && recorded_timeout.is_number_unsigned())
        timeout_ms = recorded_timeout.get<uint64_t>();
    if (!timeout_ms || timeout_ms > 3600000)
        throw std::runtime_error("replay needs a deadline of 1..3600000 ms; use --timeout-ms for older runs");
    // No worker path or executable from the bundle is ever executed. The caller selects it.
    fresh_directory(out);
    auto dataset = out / "dataset";
    auto object = dataset / "objects" / id;
    write_bytes(object / "header.bin", header);
    write_bytes(object / "code.bin", code);
    write_bytes(out / "profile.json", profile_bytes);
    atomic_json(dataset / "manifest.json", {{"schema", 1}, {"root", path_text(root)},
                {"shaders", {{id, shader}}}, {"files", json::object()}, {"shader_count", 1},
                {"limited", false}, {"traversal_errors", json::array()}});
    auto result = run({dataset, out / "run", worker, out / "profile.json", 1, timeout_ms, 0, false});
    const auto &fresh = result.at("results").at(id);
    json replay = {{"schema", 1}, {"id", id}, {"replay_state", "completed"},
                   {"worker_matches", result.at("worker_sha256") == bundle.at("worker_sha256")},
                   {"deadline_matches", !recorded_timeout.is_null() && recorded_timeout == timeout_ms},
                   {"original_status", original.at("status")}, {"status", fresh.at("status")},
                   {"status_matches", original.at("status") == fresh.at("status")},
                   {"results", "run/results.json"}, {"semantic_correctness", "not_tested"},
                   {"note", "Matching status alone does not establish the same failure or root cause. "
                            "Host environment and dependency DLL identity are not captured."}};
    atomic_json(out / "replay.json", replay);
    return replay;
}
} // namespace sl
