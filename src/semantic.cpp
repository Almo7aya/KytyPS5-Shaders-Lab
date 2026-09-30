#include "shader_lab/lab.hpp"

namespace sl {
namespace {
json bounded_json(const fs::path &path) {
    const auto bytes = read_bytes(path, 1024 * 1024);
    return json::parse(bytes.begin(), bytes.end());
}
fs::path evidence_path(const fs::path &root, const json &name) {
    const auto relative = path_from(name.get<std::string>());
    if (relative.empty() || relative.is_absolute() || relative.has_root_name())
        throw std::runtime_error("semantic evidence paths must be relative");
    const auto path = fs::weakly_canonical(root / relative);
    if (!is_within(path, root) || !fs::is_regular_file(path))
        throw std::runtime_error("semantic evidence is missing or outside .shader-lab");
    return path;
}
int priority(const std::string &status) {
    if (status == "mismatch")
        return 5;
    if (status == "execution_error" || status == "backend_error" || status == "backend_timeout" ||
        status == "invalid_evidence")
        return 4;
    if (status == "unsupported" || status == "backend_unavailable")
        return 3;
    if (status == "gpu_not_allowed")
        return 2;
    return 1;
}
} // namespace

json validate_semantics(const fs::path &input, const fs::path &output, const json &manifest,
                        const fs::path &worker, SemanticOptions options, const fs::path &dataset) {
    if (options.allow_gpu && !options.enabled)
        throw std::runtime_error("--allow-gpu requires --semantic");
    json result = {
        {"schema", 1},
        {"requested", options.enabled},
        {"gpu_opt_in", options.allow_gpu},
        {"semantic_correctness", "not_proven"},
        {"reference_authenticity", "model_generated_or_user_supplied_not_hardware_certified"},
        {"scope", "Matches apply only to tested inputs, state, outputs and comparison "
                  "policies; never 100% correctness."},
        {"cases", json::object()},
        {"issues", json::array()},
        {"status_counts", json::object()}};
    for (auto it = manifest.at("shaders").begin(); it != manifest.at("shaders").end(); ++it) {
        const bool compute = it.value().value("type", "") == "CS";
        result["cases"][it.key()] = {
            {"status", !options.enabled ? "not_requested"
                       : compute        ? "missing_evidence"
                                        : "unsupported"},
            {"reason", !options.enabled ? "Enable --semantic to assess execution evidence."
                       : compute ? "No matching fixture with captured inputs and independent "
                                   "reference was supplied in .shader-lab/semantics.json."
                                 : "Offline execution currently supports restricted compute "
                                   "fixtures only; graphics/stage state is unsupported."},
            {"semantic_correctness", "not_proven"},
            {"tests", json::array()}};
    }
    const auto root = fs::weakly_canonical(input / ".shader-lab");
    const auto index = root / "semantics.json";
    if (options.enabled && fs::exists(index)) {
        try {
            if (!is_within(root, input))
                throw std::runtime_error(".shader-lab must stay inside the input folder");
            const auto plan = bounded_json(evidence_path(root, "semantics.json"));
            if (!plan.is_object() || plan.size() != 2 || plan.at("schema") != 1 ||
                !plan.at("tests").is_array() || plan.at("tests").size() > 4096)
                throw std::runtime_error("expected semantic index schema 1 and at most 4096 tests");
            result["index_sha256"] = hash_file(index);
            // Execution attempts are never silently cached. Preserve every previous attempt.
            fs::path attempts;
            if (options.allow_gpu && !plan.at("tests").empty()) {
                fs::create_directories(output);
                for (unsigned n = 1;; ++n) {
                    attempts = output / ("attempt-" + std::to_string(n));
                    if (fs::create_directory(attempts))
                        break;
                    if (n == 100000)
                        throw std::runtime_error("too many semantic attempts");
                }
                result["attempt_directory"] = path_text(attempts.filename());
            }
            size_t serial = 0;
            for (const auto &entry : plan.at("tests")) {
                const auto number = serial++;
                std::string id;
                json test = {{"index", number}, {"status", "invalid_evidence"}};
                try {
                    id = entry.at("case_id").get<std::string>();
                    if (!result["cases"].contains(id))
                        throw std::runtime_error("fixture case_id is not in this extracted corpus");
                    if (!entry.is_object() || entry.size() != 3)
                        throw std::runtime_error(
                            "semantic test requires only case_id, fixture and reference");
                    const auto fixture = evidence_path(root, entry.at("fixture"));
                    const auto reference = evidence_path(root, entry.at("reference"));
                    const auto identity = execution_fixture_identity(fixture);
                    const auto &shader = manifest.at("shaders").at(id);
                    if (identity.at("header_sha256") != shader.at("header_sha256") ||
                        identity.at("shader_sha256") != shader.at("code_sha256"))
                        throw std::runtime_error(
                            "fixture bytes do not match this shader's header and code hashes");
                    const auto expected = bounded_json(reference);
                    if (nlohmann::json(expected.at("fixture")) != nlohmann::json(identity))
                        throw std::runtime_error(
                            "reference does not match the fixture inputs/state");
                    if (verify_reference(reference, reference).at("status") != "match")
                        throw std::runtime_error("reference fails its own comparison policy");
                    test["fixture"] = identity;
                    test["reference_source"] = expected.at("reference_source");
                    test["reference_record_sha256"] = hash_file(reference);
                    if (!options.allow_gpu) {
                        test["status"] = "gpu_not_allowed";
                        test["reason"] = "Evidence identity checked; translated execution requires "
                                         "--semantic --allow-gpu. No device was opened.";
                    } else if (!fs::is_regular_file(worker)) {
                        test["status"] = "backend_unavailable";
                        test["reason"] = "Bundled shader-vulkan-replay is missing; build/package "
                                         "the execution backend.";
                    } else {
                        const auto out = attempts / std::to_string(number);
                        const auto execution =
                            execute_fixture({fixture, reference, worker, out, 30000, "gpu", true});
                        test["execution"] = execution;
                        if (nlohmann::json(execution.at("fixture")) != nlohmann::json(identity) ||
                            execution.at("reference_record_sha256") !=
                                test.at("reference_record_sha256"))
                            throw std::runtime_error(
                                "fixture/reference changed between assessment and execution; "
                                "result cannot be attributed to this case");
                        test["status"] = execution.at("status") == "match" ? "matched_test_inputs"
                                                                           : execution.at("status");
                        test["reason"] = execution.value(
                            "reason",
                            execution.at("status") == "match"
                                ? "Observable outputs matched the supplied reference for this "
                                  "fixture only. Reference provenance is not authenticated; real "
                                  "game behavior remains unproven."
                                : "Inspect execution evidence; a mismatch is relative to the "
                                  "supplied reference, not automatically a proven emulator bug.");
                        test["artifacts"] = path_text(out.lexically_relative(output));
                    }
                } catch (const std::exception &error) {
                    test["reason"] = error.what();
                }
                if (!result["cases"].contains(id)) {
                    result["issues"].push_back(test);
                    continue;
                }
                auto &record = result["cases"][id];
                const auto status = test.at("status").get<std::string>();
                if (record["tests"].empty() || priority(status) > priority(record.at("status"))) {
                    record["status"] = status;
                    record["reason"] = test.at("reason");
                }
                record["tests"].push_back(std::move(test));
            }
        } catch (const std::exception &error) {
            result["issues"].push_back({{"status", "invalid_index"}, {"reason", error.what()}});
            for (auto &record : result["cases"])
                if (record["tests"].empty()) {
                    record["status"] = "invalid_evidence";
                    record["reason"] =
                        std::string("Semantic index could not be processed: ") + error.what();
                }
        }
    }
    if (options.enabled && !dataset.empty())
        generate_semantics(dataset, output, manifest, worker, options, result);
    for (const auto &record : result["cases"]) {
        const auto status = record.at("status").get<std::string>();
        result["status_counts"][status] = result["status_counts"].value(status, 0) + 1;
    }
    return result;
}
} // namespace sl
