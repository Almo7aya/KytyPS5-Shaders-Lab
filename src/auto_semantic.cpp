#include "shader_lab/lab.hpp"
#include <iostream>

namespace sl {
namespace {
json bounded_json(const fs::path &path) {
    const auto bytes = read_bytes(path, 1024 * 1024);
    return json::parse(bytes.begin(), bytes.end());
}
int rank(const std::string &status) {
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
fs::path generated_file(const fs::path &root, const json &name) {
    const auto relative = path_from(name.get<std::string>());
    if (relative.empty() || relative.is_absolute() || relative.has_root_name())
        throw std::runtime_error("generator returned a non-relative evidence path");
    const auto path = fs::canonical(root / relative);
    if (!is_within(path, root / "fixtures") || !fs::is_regular_file(path))
        throw std::runtime_error("generated evidence escapes its fixture tree");
    return path;
}
} // namespace

void generate_semantics(const fs::path &dataset, const fs::path &output, const json &manifest,
                        const fs::path &worker, SemanticOptions options, json &result) {
    const auto cpu = worker.parent_path() /
                     fs::path("shader-cpu-reference").replace_extension(worker.extension());
    const bool have_cpu = fs::is_regular_file(cpu);
    const auto cpu_hash = have_cpu ? hash_file(cpu) : "";
    // ordered_json stores object members in a vector. Add root metadata before
    // holding iterators/references into cases, so later updates cannot relocate it.
    result["generated_directory"] = "";
    fs::path attempt;
    size_t serial = 0;
    auto checkpoint = std::chrono::steady_clock::now();
    for (auto it = result["cases"].begin(); it != result["cases"].end(); ++it) {
        auto &record = it.value();
        // Explicit captures take precedence, including invalid/incomplete evidence.
        if (record.at("status") != "missing_evidence")
            continue;
        record["evidence_source"] = "generated_synthetic";
        record["reason"] = "Automatic synthetic test generation was not completed.";
        record["assumptions"] =
            "Synthetic inputs and launch state, not captured or inferred game state. An "
            "independent CPU model supplies expected outputs; it is not hardware-certified.";
        record["status"] = "unsupported";
        try {
            const auto &shader = manifest.at("shaders").at(it.key());
            if (shader.value("code_bytes", uint64_t(0)) > 16384) {
                record["reason"] = "Code exceeds the independent model's 4096-dword limit; "
                                   "automatic semantic testing is unsupported.";
                continue;
            }
            if (!have_cpu) {
                record["status"] = "backend_unavailable";
                record["reason"] = "Bundled shader-cpu-reference is missing; automatic independent "
                                   "reference generation cannot run.";
                continue;
            }
            const auto id = it.key();
            if (id.size() != 129 || id[64] != '-' ||
                id.find_first_not_of("0123456789abcdef-", 0) != id.npos)
                throw std::runtime_error("invalid extracted case identity");
            const auto object = fs::canonical(dataset / "objects" / path_from(id));
            if (!is_within(object, dataset / "objects"))
                throw std::runtime_error("shader object escapes the dataset");
            for (const auto *name : {"header.bin", "code.bin"})
                if (!fs::is_regular_file(object / name) || !is_within(object / name, object))
                    throw std::runtime_error("shader object file is missing or redirected");
            if (hash_file(object / "header.bin") != shader.at("header_sha256").get<std::string>() ||
                hash_file(object / "code.bin") != shader.at("code_sha256").get<std::string>())
                throw std::runtime_error("dataset shader bytes do not match their recorded hashes");
            if (attempt.empty()) {
                fs::create_directories(output);
                for (unsigned n = 1;; ++n) {
                    attempt = output / ("generated-" + std::to_string(n));
                    if (fs::create_directory(attempt))
                        break;
                    if (n == 100000)
                        throw std::runtime_error("too many generated attempts");
                }
                result["generated_directory"] = path_text(attempt.filename());
            }
            const auto dir = attempt / std::to_string(serial++);
            fs::create_directory(dir);
            const auto request = dir / "generate-request.json";
            atomic_json(request, {{"schema", 1},
                                  {"kind", "shader_lab_generate_request"},
                                  {"header", path_text(fs::absolute(object / "header.bin"))},
                                  {"code", path_text(fs::absolute(object / "code.bin"))},
                                  {"header_sha256", shader.at("header_sha256")},
                                  {"code_sha256", shader.at("code_sha256")}});
            const auto request_hash = hash_file(request);
            record["generation_artifacts"] = path_text(dir.lexically_relative(output));
            record["model_worker_sha256"] = cpu_hash;
            const auto process_result =
                process(cpu, {"--generate-fixtures", path_text(fs::absolute(request))}, dir,
                        dir / "generator.log", std::chrono::seconds(10));
            if (process_result.timed_out || process_result.exit_code) {
                record["status"] = process_result.timed_out ? "backend_timeout" : "backend_error";
                record["reason"] =
                    "Independent CPU fixture generator did not complete; inspect generator.log.";
            } else {
                if (hash_file(cpu) != cpu_hash || hash_file(request) != request_hash)
                    throw std::runtime_error(
                        "CPU generator or its request changed during execution");
                const auto generation = bounded_json(dir / "generation.json");
                if (generation.at("schema") != 1 ||
                    generation.at("kind") != "shader_lab_generated_fixtures" ||
                    generation.at("request_sha256") != request_hash ||
                    !generation.at("tests").is_array() || generation.at("tests").size() > 6)
                    throw std::runtime_error("invalid CPU generator response");
                record["generation"] = generation;
                if (generation.at("status") == "unsupported") {
                    record["reason"] = generation.at("reason");
                } else {
                    if (generation.at("status") != "assessed" || generation.at("tests").size() != 6)
                        throw std::runtime_error(
                            "generator did not account for all six planned tests");
                    unsigned number = 0;
                    for (const auto &generated : generation.at("tests")) {
                        json test = {{"index", number},
                                     {"status", "unsupported"},
                                     {"evidence_source", "generated_synthetic"},
                                     {"generation", generated},
                                     {"semantic_correctness", "not_proven"}};
                        try {
                            if (generated.at("status") == "unsupported") {
                                test["reason"] = generated.at("reason");
                            } else {
                                if (generated.at("status") != "generated")
                                    throw std::runtime_error("unknown generation outcome");
                                const auto fixture = generated_file(dir, generated.at("fixture"));
                                const auto reference =
                                    generated_file(dir, generated.at("reference"));
                                const auto identity = execution_fixture_identity(fixture);
                                if (identity.at("header_sha256") != shader.at("header_sha256") ||
                                    identity.at("shader_sha256") != shader.at("code_sha256") ||
                                    nlohmann::json(identity) !=
                                        nlohmann::json(generated.at("fixture_identity")))
                                    throw std::runtime_error(
                                        "generated fixture does not identify the extracted shader");
                                const auto expected = bounded_json(reference);
                                if (nlohmann::json(expected.at("fixture")) !=
                                        nlohmann::json(identity) ||
                                    verify_reference(reference, reference).at("status") != "match")
                                    throw std::runtime_error(
                                        "generated reference is inconsistent with its fixture");
                                test["fixture"] = identity;
                                test["reference_source"] = expected.at("reference_source");
                                test["reference_record_sha256"] = hash_file(reference);
                                test["generated_fixture"] =
                                    path_text(fixture.parent_path().lexically_relative(output));
                                if (!options.allow_gpu) {
                                    test["status"] = "gpu_not_allowed";
                                    test["reason"] =
                                        "Synthetic inputs and independent CPU expected outputs "
                                        "generated. Add --allow-gpu to compare Kyty's translation; "
                                        "it has not been executed.";
                                } else if (!fs::is_regular_file(worker)) {
                                    test["status"] = "backend_unavailable";
                                    test["reason"] = "Independent references generated, but "
                                                     "bundled shader-vulkan-replay is missing.";
                                } else {
                                    const auto replay = dir / ("gpu-" + std::to_string(number));
                                    const auto execution = execute_fixture(
                                        {fixture, reference, worker, replay, 30000, "gpu", true});
                                    test["artifacts"] =
                                        path_text(replay.lexically_relative(output));
                                    test["execution"] = execution;
                                    if (nlohmann::json(execution.at("fixture")) !=
                                            nlohmann::json(identity) ||
                                        execution.at("reference_record_sha256") !=
                                            test.at("reference_record_sha256"))
                                        throw std::runtime_error(
                                            "generated fixture/reference changed before execution");
                                    test["status"] = execution.at("status") == "match"
                                                         ? "matched_test_inputs"
                                                         : execution.at("status");
                                    test["reason"] = execution.value(
                                        "reason",
                                        execution.at("status") == "match"
                                            ? "Kyty output matched the independent CPU model for "
                                              "generated synthetic inputs. This is not a game "
                                              "capture or hardware-certified correctness proof."
                                            : "Generated test did not match or complete; "
                                              "investigate compiler, driver and reference-model "
                                              "behavior.");
                                }
                            }
                        } catch (const std::exception &error) {
                            test["status"] = "invalid_evidence";
                            test["reason"] = error.what();
                        }
                        if (record["tests"].empty() ||
                            rank(test.at("status")) > rank(record.at("status"))) {
                            record["status"] = test.at("status");
                            record["reason"] = test.at("reason");
                        }
                        record["tests"].push_back(std::move(test));
                        ++number;
                    }
                }
            }
            atomic_json(dir / "case-result.json", record);
        } catch (const std::exception &error) {
            record["status"] = "invalid_evidence";
            record["reason"] = error.what();
        }
        if (std::chrono::steady_clock::now() - checkpoint >= std::chrono::seconds(5)) {
            std::cout << "Semantic generation: " << serial << " shader cases assessed\n"
                      << std::flush;
            atomic_json(output / "checkpoint.json", result);
            checkpoint = std::chrono::steady_clock::now();
        }
    }
}
} // namespace sl
