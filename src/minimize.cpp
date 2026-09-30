#include "shader_lab/lab.hpp"
#include <algorithm>
#include <regex>

namespace sl {
namespace {
json bounded_json(const fs::path &path) {
    auto bytes = read_bytes(path);
    return json::parse(bytes.begin(), bytes.end());
}
std::string profile_hash(const json &profile) {
    auto text = profile.dump();
    return sha256({reinterpret_cast<const uint8_t *>(text.data()), text.size()});
}
struct InstructionRange {
    size_t begin, words;
};
constexpr uint32_t nop = 0xbf800000;
bool all_nops(Bytes code, InstructionRange range) {
    for (size_t word = 0; word < range.words; ++word)
        if (integer(code, (range.begin + word) * 4, 4) != nop)
            return false;
    return true;
}
void put_nop(std::vector<uint8_t> &code, size_t word) {
    for (size_t byte = 0; byte < 4; ++byte)
        code.at(word * 4 + byte) = uint8_t(nop >> (8 * byte));
}
} // namespace

json failure_signature(const json &row) {
    try {
        auto status = row.value("status", "");
        auto details = row.value("details", json::object());
        auto phase =
            details.value("last_phase", row.value("last_phase", json::object()).value("phase", ""));
        if (phase.empty())
            return nullptr;
        json signature = {{"status", status}, {"phase", phase}};
        if (status == "unsupported_instruction") {
            const auto &unsupported = details.at("unsupported");
            if (!unsupported.is_array() || unsupported.empty())
                return nullptr;
            signature["instructions"] = json::array();
            for (const auto &op : unsupported) {
                if (op.value("family", "").empty() || op.value("text", "").empty())
                    return nullptr;
                // PCs and instruction text are intentionally exact: code transforms preserve PCs.
                signature["instructions"].push_back({{"family", op.at("family")},
                                                     {"opcode_id", op.at("opcode_id")},
                                                     {"text", op.at("text")}});
            }
        } else if (status == "spirv_invalid_under_profile") {
            auto messages = details.at("validator_messages");
            if (!messages.is_array() || messages.empty() ||
                details.value("validation_environment", "").empty())
                return nullptr;
            signature["environment"] = details.at("validation_environment");
            signature["diagnostics"] = json::array();
            for (const auto &message : messages) {
                if (message.value("message", "").empty())
                    return nullptr;
                signature["diagnostics"].push_back(
                    {{"level", message.at("level")}, {"message", message.at("message")}});
            }
        } else if (status == "worker_crash_or_error") {
            auto log = row.value("log_tail", "");
            auto marker = log.rfind("--- Error ---");
            if (marker == std::string::npos || row.value("exit_code", uint32_t(0)) == 0)
                return nullptr;
            auto start = log.find_first_not_of("\r\n", marker + 13);
            if (start == std::string::npos)
                return nullptr;
            auto reason = log.substr(start, log.find_first_of("\r\n", start) - start);
            // Only the changing shader content hash is normalized. Do not normalize PCs,
            // fault addresses, source locations or other potentially distinct failure evidence.
            reason = std::regex_replace(reason, std::regex("hash=0x[0-9a-fA-F]+"), "hash=<case>");
            signature["assertion"] = reason;
            signature["exit_code"] = row.at("exit_code");
        } else {
            return nullptr; // Timeouts, generic failures and success are not reduction predicates.
        }
        return signature;
    } catch (const json::exception &) {
        return nullptr; // Malformed diagnostics cannot act as a failure-preservation oracle.
    }
}

json minimize_repro(const MinimizeOptions &o) {
    if (o.confirmations < 2 || o.confirmations > 5 || o.max_attempts < 2 * o.confirmations ||
        o.max_attempts > 10000)
        throw std::runtime_error(
            "minimize needs 2..5 confirmations and 2*confirmations..10000 attempts");
    auto bundle = fs::canonical(o.bundle), worker = fs::canonical(o.worker);
    auto out = fs::weakly_canonical(o.output);
    for (const auto &input : {bundle, worker})
        if (is_within(out, input) || is_within(input, out))
            throw std::runtime_error("minimize output must not overlap its bundle or worker");
    auto descriptor = bounded_json(bundle / "repro.json");
    if (descriptor.value("schema", 0) != 1 || descriptor.value("kind", "") != "shader_lab_repro")
        throw std::runtime_error("unsupported repro bundle");
    auto original_path = fs::canonical(bundle / "original-result.json");
    if (!is_within(original_path, bundle) || !is_within(bundle / "repro.json", bundle))
        throw std::runtime_error("repro metadata escapes the bundle");
    auto original_bytes = read_bytes(original_path);
    if (sha256(original_bytes) != descriptor.at("files").at("original-result.json").get<std::string>())
        throw std::runtime_error("original result hash mismatch");
    auto expected = failure_signature(json::parse(original_bytes.begin(), original_bytes.end()));
    if (expected.is_null())
        throw std::runtime_error("no supported diagnostic failure predicate; successes, timeouts "
                                 "and generic errors cannot be minimized");
    const auto expected_worker = descriptor.at("worker_sha256").get<std::string>();
    if (hash_file(worker) != expected_worker)
        throw std::runtime_error("minimize requires the original worker; replay and export a new "
                                 "bundle for another worker");
    fs::create_directories(out.parent_path());
    if (!fs::create_directory(out))
        throw std::runtime_error("minimize requires a fresh output directory");
    json log = {
        {"schema", 1},
        {"state", "baseline"},
        {"worker_sha256", expected_worker},
        {"source_bundle_sha256", hash_file(bundle / "repro.json")},
        {"confirmations", o.confirmations},
        {"max_attempts", o.max_attempts},
        {"attempts", json::array()},
        {"accepted", json::array()},
        {"final_verified", false},
        {"semantic_correctness", "not_tested"},
        {"note",
         "Preserves a repeated observed failure predicate, not root-cause identity or shader "
         "semantics. "
         "Code transformations replace whole decoded instructions with same-size NOP sequences."}};
    auto checkpoint = [&] { atomic_json(out / "minimization.json", log); };
    uint64_t used = 0;
    bool budget = false;
    fs::path best_dataset, best_profile, best_results;
    auto observe = [&](const json &summary, const fs::path &results, const std::string &kind) {
        ++used;
        if (hash_file(worker) != expected_worker || summary.at("worker_sha256") != expected_worker)
            throw std::runtime_error("worker changed during minimization");
        const auto &row = summary.at("results").begin().value();
        auto signature = failure_signature(row);
        bool match = !signature.is_null() && signature == expected;
        log["attempts"].push_back({{"number", used},
                                   {"kind", kind},
                                   {"id", row.at("id")},
                                   {"results", path_text(fs::relative(results, out))},
                                   {"status", row.at("status")},
                                   {"signature", signature},
                                   {"matches", match}});
        log["attempts_used"] = used;
        checkpoint();
        return match;
    };
    try {
        checkpoint();
        // replay_repro verifies the full bundle and creates immutable, portable input copies.
        auto baseline = out / "baseline";
        replay_repro(bundle, worker, baseline, o.timeout_ms);
        if (hash_file(original_path) != sha256(original_bytes) ||
            hash_file(bundle / "repro.json") != log.at("source_bundle_sha256").get<std::string>())
            throw std::runtime_error("source evidence changed during baseline replay");
        log["predicate"] = expected;
        auto first_summary = bounded_json(baseline / "run/results.json");
        if (!observe(first_summary, baseline / "run/results.json", "baseline")) {
            log["state"] = "baseline_not_reproduced";
            checkpoint();
            return log;
        }
        best_dataset = baseline / "dataset";
        best_profile = baseline / "profile.json";
        best_results = baseline / "run/results.json";
        auto timeout = first_summary.at("timeout_ms").get<uint64_t>();
        auto id = descriptor.at("id").get<std::string>();
        auto object = best_dataset / "objects" / id;
        auto header = read_bytes(object / "header.bin"), code = read_bytes(object / "code.bin");
        auto profile = bounded_json(best_profile);
        auto manifest = bounded_json(best_dataset / "manifest.json");
        auto shader = manifest.at("shaders").at(id);
        for (unsigned confirmation = 1; confirmation < o.confirmations; ++confirmation) {
            auto path = out / ("baseline-confirm-" + std::to_string(confirmation));
            auto summary = run({best_dataset, path, worker, best_profile, 1, timeout, 0, false});
            if (!observe(summary, path / "results.json", "baseline")) {
                log["state"] = "baseline_unstable";
                checkpoint();
                return log;
            }
            best_results = path / "results.json";
        }
        std::vector<InstructionRange> ranges;
        const auto &first = first_summary.at("results").at(id);
        auto metadata = baseline / "run" / path_from(first.at("artifacts").get<std::string>()) /
                        "instructions.json";
        if (fs::exists(metadata) && is_within(metadata, baseline / "run/cases")) {
            auto decoded = bounded_json(metadata);
            if (decoded.value("schema", 0) == 1 &&
                decoded.value("code_sha256", "") == sha256(code)) {
                uint64_t end = 0;
                for (const auto &inst : decoded.at("instructions")) {
                    auto pc = inst.at("pc").get<uint64_t>(),
                         words = inst.at("words").get<uint64_t>();
                    if (pc != end || pc % 4 || !words || words > code.size() / 4 ||
                        !contains(code, pc, words * 4))
                        throw std::runtime_error("fresh decoder instruction extents are invalid");
                    ranges.push_back({size_t(pc / 4), size_t(words)});
                    end = pc + words * 4;
                }
            }
        }
        log["code_reduction"] = ranges.empty()
                                    ? "unavailable: no content-bound fresh instruction inventory"
                                    : "pc_preserving_nops";
        log["original_code_sha256"] = sha256(code);
        log["original_profile_sha256"] = profile_hash(profile);
        log["state"] = "reducing";
        checkpoint();
        uint64_t proposal = 0;
        auto attempt = [&](const std::vector<uint8_t> &candidate_code,
                           const json &candidate_profile, const json &change) {
            if (used + 2 * o.confirmations > o.max_attempts) {
                budget = true;
                return false;
            }
            auto trial = out / "trials" / std::to_string(++proposal);
            auto candidate_id = sha256(header) + "-" + sha256(candidate_code);
            auto candidate_shader = shader;
            candidate_shader["code_sha256"] = sha256(candidate_code);
            candidate_shader["kyty_hash"] = xxh3(candidate_code);
            if (candidate_id != id) {
                candidate_shader["origins"] = json::array();
                candidate_shader["derived_from"] = id;
                candidate_shader["derivation"] = "instruction NOP reduction; header preserved, "
                                                 "bytes no longer original game evidence";
            }
            auto candidate_manifest = manifest;
            candidate_manifest["shaders"] = {{candidate_id, candidate_shader}};
            auto dataset = trial / "dataset";
            auto candidate_object = dataset / "objects" / candidate_id;
            write_bytes(candidate_object / "header.bin", header);
            write_bytes(candidate_object / "code.bin", candidate_code);
            atomic_json(dataset / "manifest.json", candidate_manifest);
            atomic_json(trial / "profile.json", candidate_profile);
            atomic_json(trial / "proposal.json", change);
            fs::path results;
            for (unsigned repetition = 0; repetition < o.confirmations; ++repetition) {
                auto path = trial / ("run-" + std::to_string(repetition));
                auto summary =
                    run({dataset, path, worker, trial / "profile.json", 1, timeout, 0, false});
                results = path / "results.json";
                if (!observe(summary, results, "proposal-" + std::to_string(proposal)))
                    return false;
            }
            best_dataset = dataset;
            best_profile = trial / "profile.json";
            best_results = results;
            code = candidate_code;
            profile = candidate_profile;
            log["accepted"].push_back({{"proposal", proposal},
                                       {"change", change},
                                       {"code_sha256", sha256(code)},
                                       {"profile_sha256", profile_hash(profile)}});
            checkpoint();
            return true;
        };
        // Deterministic chunk reduction reaches single-element trials when budget permits.
        // Rebuild units after every acceptance so deletions never use stale indices.
        auto reduce = [&](auto units, auto mutate, const char *kind) {
            auto indices = units();
            size_t chunk = indices.size();
            while (chunk && !budget) {
                bool accepted = false;
                for (size_t start = 0; start < indices.size() && !budget; start += chunk) {
                    auto candidate_code = code;
                    auto candidate_profile = profile;
                    auto count = std::min(chunk, indices.size() - start);
                    mutate(candidate_code, candidate_profile, indices, start, count);
                    json selected = json::array();
                    for (size_t i = start; i < start + count; ++i)
                        selected.push_back(indices[i]);
                    if (attempt(candidate_code, candidate_profile,
                                {{"kind", kind}, {"units", selected}})) {
                        indices = units();
                        chunk = std::min(chunk, indices.size());
                        accepted = true;
                        break;
                    }
                }
                if (!accepted)
                    chunk = chunk == 1 ? 0 : (chunk + 1) / 2;
            }
        };
        size_t before;
        do {
            before = log["accepted"].size();
            reduce(
                [&] {
                    std::vector<size_t> units;
                    if (profile.contains("memory")) {
                        if (!profile.at("memory").is_array())
                            throw std::runtime_error("memory must be an array");
                        for (size_t i = 0; i < profile.at("memory").size(); ++i)
                            units.push_back(i);
                    }
                    return units;
                },
                [](auto &, auto &p, const auto &indices, size_t start, size_t count) {
                    for (size_t i = start + count; i-- > start;)
                        p["memory"].erase(p["memory"].begin() + indices[i]);
                },
                "remove_memory_ranges");
            reduce(
                [&] {
                    std::vector<size_t> units;
                    if (profile.contains("user_data")) {
                        const auto values =
                            profile.at("user_data").template get<std::vector<uint32_t>>();
                        for (size_t i = 0; i < values.size(); ++i)
                            if (values[i])
                                units.push_back(i);
                    }
                    return units;
                },
                [](auto &, auto &p, const auto &indices, size_t start, size_t count) {
                    for (size_t i = start; i < start + count; ++i)
                        p["user_data"][indices[i]] = 0;
                },
                "zero_user_data_words");
            reduce(
                [&] {
                    std::vector<std::pair<size_t, size_t>> units;
                    if (profile.contains("memory"))
                        for (size_t r = 0; r < profile["memory"].size(); ++r) {
                            const auto values = profile["memory"][r]
                                                    .at("words")
                                                    .template get<std::vector<uint32_t>>();
                            for (size_t i = 0; i < values.size(); ++i)
                                if (values[i])
                                    units.emplace_back(r, i);
                        }
                    return units;
                },
                [](auto &, auto &p, const auto &indices, size_t start, size_t count) {
                    for (size_t i = start; i < start + count; ++i)
                        p["memory"][indices[i].first]["words"][indices[i].second] = 0;
                },
                "zero_memory_words");
            reduce(
                [&] {
                    std::vector<size_t> units;
                    for (size_t i = 0; i < ranges.size(); ++i)
                        if (!all_nops(code, ranges[i]))
                            units.push_back(i);
                    return units;
                },
                [&](auto &c, auto &, const auto &indices, size_t start, size_t count) {
                    for (size_t i = start; i < start + count; ++i) {
                        auto range = ranges[indices[i]];
                        for (size_t word = 0; word < range.words; ++word)
                            put_nop(c, range.begin + word);
                    }
                },
                "nop_decoded_instructions");
        } while (!budget && log["accepted"].size() != before);
        log["state"] = "final_confirmation";
        checkpoint();
        for (unsigned repetition = 0; repetition < o.confirmations; ++repetition) {
            auto path = out / ("final-" + std::to_string(repetition));
            auto summary = run({best_dataset, path, worker, best_profile, 1, timeout, 0, false});
            best_results = path / "results.json";
            if (!observe(summary, best_results, "final")) {
                log["state"] = "final_not_reproduced";
                checkpoint();
                return log;
            }
        }
        auto best = bounded_json(best_results).at("results").begin().key();
        export_repro(best_dataset, best_results, best, out / "minimized-bundle");
        log["state"] = budget ? "budget_exhausted" : "fixed_point";
        log["minimality"] = budget ? "incomplete_search"
                                   : "single_transform_fixed_point_within_available_reductions";
        log["final_verified"] = true;
        log["bundle"] = "minimized-bundle";
        log["final_code_sha256"] = sha256(code);
        log["final_profile_sha256"] = profile_hash(profile);
        checkpoint();
        return log;
    } catch (const std::exception &error) {
        log["state"] = "error";
        log["error"] = error.what();
        log["attempts_used"] = used;
        checkpoint();
        throw;
    }
}
} // namespace sl
