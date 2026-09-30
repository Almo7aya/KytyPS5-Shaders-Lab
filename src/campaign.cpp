#include "shader_lab/lab.hpp"
#include <limits>
#include <set>
#include <stdexcept>

namespace sl {
namespace {
std::string digest(const json &value) {
    auto text = value.dump();
    return sha256({reinterpret_cast<const uint8_t *>(text.data()), text.size()});
}
void fields(const json &object, const std::set<std::string> &allowed) {
    if (!object.is_object())
        throw std::runtime_error("campaign entries must be objects");
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!allowed.contains(it.key()))
            throw std::runtime_error("unknown campaign field: " + it.key());
}
void validate_profile(const json &profile, const json &manifest) {
    if (!profile.is_object())
        throw std::runtime_error("campaign profile must be an inline object");
    if (profile.contains("cases")) {
        fields(profile, {"default", "cases"});
        if (!profile.contains("default") || !profile["default"].is_object() ||
            !profile["cases"].is_object())
            throw std::runtime_error("campaign profile bundle requires default and cases objects");
        for (auto it = profile["cases"].begin(); it != profile["cases"].end(); ++it)
            if (!manifest.at("shaders").contains(it.key()) || !it.value().is_object())
                throw std::runtime_error("campaign profile bundle has an invalid case");
    }
}
} // namespace

json campaign(const RunOptions &options, const fs::path &plan_path) {
    if (!options.profile.empty())
        throw std::runtime_error("campaign contexts provide profiles; do not also set --profile");
    if (!options.jobs || options.jobs > 64 || !options.timeout_ms ||
        options.timeout_ms > uint64_t(std::numeric_limits<int64_t>::max()))
        throw std::runtime_error("invalid campaign jobs or timeout");
    auto plan = read_json(plan_path);
    fields(plan, {"schema", "contexts"});
    if (plan.value("schema", 0) != 1 || !plan.contains("contexts") ||
        !plan["contexts"].is_array() || plan["contexts"].empty() ||
        plan["contexts"].size() > 128)
        throw std::runtime_error("campaign schema 1 requires 1..128 contexts");
    auto dataset = fs::canonical(options.dataset);
    auto manifest = read_json(dataset / "manifest.json");
    if (manifest.value("schema", 0) != schema_version || !manifest.at("shaders").is_object())
        throw std::runtime_error("unsupported campaign dataset");
    std::set<std::string> names;
    for (const auto &context : plan["contexts"]) {
        fields(context, {"name", "profile"});
        auto name = context.at("name").get<std::string>();
        if (name.empty() || name.size() > 128 || !names.insert(name).second)
            throw std::runtime_error("context names must be unique and 1..128 bytes");
        validate_profile(context.at("profile"), manifest);
    }
    auto out = fs::weakly_canonical(options.output);
    auto input = path_from(manifest.at("root").get<std::string>());
    auto overlaps = [&](const fs::path &path) {
        return is_within(out, path) || is_within(path, out);
    };
    if (overlaps(dataset) || overlaps(input) || is_within(plan_path, out) ||
        is_within(options.worker, out))
        throw std::runtime_error("campaign output must not overlap inputs, dataset, plan or worker");
    auto worker = fs::canonical(options.worker);
    auto manifest_hash = hash_file(dataset / "manifest.json");
    auto worker_hash = hash_file(worker);
    json identity = {{"protocol", 1}, {"plan", plan}, {"manifest_sha256", manifest_hash},
                     {"worker_sha256", worker_hash}, {"timeout_ms", options.timeout_ms},
                     {"limit", options.limit}};
    const auto key = digest(identity);
    fs::create_directories(out);
    if (!fs::create_directory(out / ".campaign-lock"))
        throw std::runtime_error("campaign directory locked; confirm no active campaign before removing .campaign-lock");
    struct Lock {
        fs::path path;
        ~Lock() { std::error_code error; fs::remove(path, error); }
    } lock{out / ".campaign-lock"};
    auto session = out / "campaigns" / key;
    fs::create_directories(session);
    json summary = {{"schema", 1}, {"campaign_key", key}, {"identity", identity},
                    {"state", "running"}, {"semantic_correctness", "not_tested"},
                    {"contexts", json::array()}, {"cases", json::object()},
                    {"context_sensitive_cases", json::array()}};
    auto save = [&] {
        atomic_json(session / "campaign.json", summary);
        atomic_json(out / "campaign.json", summary);
    };
    save();
    try {
        for (const auto &context : plan["contexts"]) {
            // Never use a user-provided name as a path component.
            auto context_dir = session / "contexts" / digest(context);
            auto profile_path = session / "profiles" / (digest(context) + ".json");
            atomic_json(profile_path, context.at("profile"));
            auto selected = options;
            selected.dataset = dataset;
            selected.worker = worker;
            selected.profile = profile_path;
            selected.output = context_dir;
            auto result = run(selected);
            if (hash_file(dataset / "manifest.json") != manifest_hash || hash_file(worker) != worker_hash)
                throw std::runtime_error("campaign input changed during execution; restart with stable inputs");
            auto name = context.at("name").get<std::string>();
            json row = {{"name", name}, {"profile_sha256", result.at("profile_sha256")},
                        {"results", path_text(fs::relative(context_dir / "results.json", out))},
                        {"status_counts", result.at("status_counts")},
                        {"selected_cases", result.at("selected_cases")},
                        {"dataset_cases", result.at("dataset_cases")},
                        {"limited", result.at("limited")}};
            summary["contexts"].push_back(row);
            for (auto it = result["results"].begin(); it != result["results"].end(); ++it) {
                auto &entries = summary["cases"][it.key()];
                if (entries.is_null()) entries = json::array();
                entries.push_back({{"context", name}, {"status", it.value().value("status", "unknown")},
                                   {"spirv_sha256", it.value().value("details", json::object()).value("spirv_sha256", "")},
                                   {"cache_hit", it.value().value("cache_hit", false)}});
            }
            save();
        }
        for (auto it = summary["cases"].begin(); it != summary["cases"].end(); ++it) {
            std::set<std::string> signatures;
            for (const auto &row : it.value())
                signatures.insert(json::array({row.at("status"), row.at("spirv_sha256")}).dump());
            if (signatures.size() > 1)
                summary["context_sensitive_cases"].push_back(it.key());
        }
        summary["state"] = "completed";
        summary["note"] = "Context-dependent outcomes or SPIR-V bytes are observations, not semantic regressions. Inspect each context's results and assumptions.";
        save();
    } catch (const std::exception &error) {
        summary["state"] = "interrupted";
        summary["error"] = error.what();
        save();
        throw;
    }
    return summary;
}
} // namespace sl
