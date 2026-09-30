#include "shader_lab/compiler_trace.hpp"

namespace sl {
namespace {
json bounded_json(const fs::path &path) {
    auto bytes = read_bytes(path);
    return json::parse(bytes.begin(), bytes.end());
}
void check_trace(const json &trace, const std::string &id, const json &catalog) {
    if (trace.value("schema", 0) != 1 || trace.value("catalog_schema", 0) != 1 ||
        trace.value("id", "") != id || trace.value("code_sha256", "") != id.substr(65))
        throw std::runtime_error("pass trace identity mismatch");
    const auto &events = trace.at("events");
    const auto &passes = catalog.at("passes");
    if (!events.is_array() || events.size() > passes.size() * 2)
        throw std::runtime_error("invalid pass trace extent");
    for (size_t n = 0; n < events.size(); ++n) {
        const auto &event = events.at(n);
        if (event.at("index") != n / 2 || event.at("name") != passes.at(n / 2).at("name") ||
            event.at("state") != (n % 2 == 0 ? "started" : "completed"))
            throw std::runtime_error("pass trace order/catalog mismatch");
    }
    if (!events.empty() && trace.at("last") != events.back())
        throw std::runtime_error("pass trace last event mismatch");
}
} // namespace

json bisect_passes(const PassBisectOptions &o) {
    if (o.confirmations < 2 || o.confirmations > 5)
        throw std::runtime_error("pass bisection requires 2..5 confirmations");
    auto bundle = fs::canonical(o.bundle), worker = fs::canonical(o.worker);
    auto out = fs::weakly_canonical(o.output);
    for (const auto &input : {bundle, worker})
        if (is_within(out, input) || is_within(input, out))
            throw std::runtime_error("pass bisection output must not overlap its inputs");
    auto descriptor_path = fs::canonical(bundle / "repro.json");
    auto original_path = fs::canonical(bundle / "original-result.json");
    if (!is_within(descriptor_path, bundle) || !is_within(original_path, bundle))
        throw std::runtime_error("pass bisection metadata escapes its bundle");
    auto descriptor_hash = hash_file(descriptor_path);
    auto descriptor = bounded_json(descriptor_path);
    auto original_bytes = read_bytes(original_path);
    if (descriptor.value("schema", 0) != 1 || descriptor.value("kind", "") != "shader_lab_repro" ||
        sha256(original_bytes) !=
            descriptor.at("files").at("original-result.json").get<std::string>())
        throw std::runtime_error("invalid pass bisection bundle/original result");
    auto expected = failure_signature(json::parse(original_bytes.begin(), original_bytes.end()));
    if (expected.is_null() || expected.at("status") != "worker_crash_or_error")
        throw std::runtime_error("pass bisection requires a detailed compiler assertion, not a "
                                 "timeout, structural validator result or output mismatch");
    auto worker_hash = hash_file(worker);
    if (worker_hash != descriptor.at("worker_sha256").get<std::string>())
        throw std::runtime_error("pass bisection requires the original worker; replay and export "
                                 "a new bundle before changing compilers");
    fs::create_directories(out.parent_path());
    if (!fs::create_directory(out))
        throw std::runtime_error("pass bisection requires a fresh output directory");
    const auto catalog = compiler_pass_catalog();
    json log = {{"schema", 1},
                {"state", "baseline"},
                {"worker_sha256", worker_hash},
                {"source_bundle_sha256", descriptor_hash},
                {"catalog", catalog},
                {"predicate", expected},
                {"confirmations", o.confirmations},
                {"attempts", json::array()},
                {"boundary_verified", false},
                {"semantic_correctness", "not_tested"},
                {"note",
                 "A repeated prefix boundary localizes where the compiler failed to "
                 "return, not which pass introduced the defect. No passes are skipped or "
                 "reordered. Shader semantic and SPIR-V validator failures are not bisected."}};
    auto checkpoint = [&] { atomic_json(out / "pass-bisection.json", log); };
    auto finish = [&](const char *state) {
        log["state"] = state;
        checkpoint();
        return log;
    };
    try {
        checkpoint();
        // Reuse the complete portable-bundle validation and make immutable input copies.
        auto baseline = out / "baseline";
        replay_repro(bundle, worker, baseline, o.timeout_ms);
        if (hash_file(descriptor_path) != descriptor_hash ||
            hash_file(original_path) != sha256(original_bytes) || hash_file(worker) != worker_hash)
            throw std::runtime_error("bundle/worker changed during baseline replay");
        auto summary = bounded_json(baseline / "run/results.json");
        auto id = descriptor.at("id").get<std::string>();
        auto baseline_signature = failure_signature(summary.at("results").at(id));
        log["attempts"].push_back({{"kind", "original_baseline"},
                                   {"results", "baseline/run/results.json"},
                                   {"signature", baseline_signature}});
        if (baseline_signature != expected)
            return finish("baseline_not_reproduced");
        const auto deadline = summary.at("timeout_ms").get<uint64_t>();
        auto profile = bounded_json(baseline / "profile.json");
        if (profile.contains("diagnostics") &&
            profile.at("diagnostics").contains("stop_after_pass"))
            throw std::runtime_error("cannot bisect an already truncated compiler profile");
        auto query = process(worker, {"--compiler-info"}, out, out / "compiler-info.json",
                             std::chrono::milliseconds(deadline));
        if (query.timed_out || query.exit_code != 0 ||
            bounded_json(out / "compiler-info.json").at("pass_catalog") != catalog)
            throw std::runtime_error("worker does not implement the required pass catalog");
        if (hash_file(worker) != worker_hash)
            throw std::runtime_error("worker changed during catalog query");
        unsigned attempt = 0;
        json last_trace;
        json expected_failure_events;
        auto probe = [&](int stop, const char *kind) {
            std::string classification;
            json repeated_trace;
            for (unsigned repeat = 0; repeat < o.confirmations; ++repeat) {
                auto dir = out / ("attempt-" + std::to_string(++attempt));
                auto selected = profile;
                selected["diagnostics"] = {{"pass_trace", true}};
                if (stop >= 0)
                    selected["diagnostics"]["stop_after_pass"] = stop;
                atomic_json(dir / "profile.json", selected);
                auto observed = run({baseline / "dataset", dir / "run", worker,
                                     dir / "profile.json", 1, deadline, 0, false});
                if (hash_file(worker) != worker_hash || observed.at("worker_sha256") != worker_hash)
                    throw std::runtime_error("worker changed during pass bisection");
                const auto &row = observed.at("results").at(id);
                auto signature = failure_signature(row);
                std::string outcome = "different_result";
                json trace = nullptr;
                if (row.contains("artifacts")) {
                    auto artifacts = fs::canonical(
                        dir / "run" / path_from(row.at("artifacts").get<std::string>()));
                    if (!is_within(artifacts, fs::canonical(dir / "run/cases")))
                        throw std::runtime_error("pass artifacts escape their fresh run");
                    auto trace_path = artifacts / "pass-trace.json";
                    if (fs::exists(trace_path)) {
                        if (!is_within(fs::canonical(trace_path), artifacts))
                            throw std::runtime_error("pass trace escapes its fresh case");
                        trace = bounded_json(trace_path);
                        check_trace(trace, id, catalog);
                    }
                }
                if (signature == expected && !trace.is_null() &&
                    (expected_failure_events.is_null() ||
                     trace.at("events") == expected_failure_events))
                    outcome = "same_assertion";
                if (stop >= 0 && row.value("status", "") == "pass_checkpoint_reached" &&
                    row.at("details").at("pass_checkpoint") ==
                        catalog.at("passes").at(size_t(stop)) &&
                    !trace.is_null() && trace.at("events").size() == size_t(stop + 1) * 2)
                    outcome = "checkpoint_reached";
                log["attempts"].push_back(
                    {{"kind", kind},
                     {"stop_after", stop},
                     {"repeat", repeat},
                     {"results", path_text(fs::relative(dir / "run/results.json", out))},
                     {"status", row.at("status")},
                     {"signature", signature},
                     {"outcome", outcome},
                     {"last_pass_event",
                      trace.is_null() ? json(nullptr) : trace.value("last", json(nullptr))}});
                checkpoint();
                if (outcome == "different_result" ||
                    (!classification.empty() && outcome != classification))
                    return std::string("inconclusive");
                // The same assertion at a different point is unstable too.
                auto events = trace.at("events");
                if (repeat && events != repeated_trace)
                    return std::string("inconclusive");
                repeated_trace = std::move(events);
                last_trace = std::move(trace);
                classification = outcome;
            }
            return classification;
        };
        if (probe(-1, "traced_baseline") != "same_assertion")
            return finish("instrumented_baseline_not_reproduced");
        expected_failure_events = last_trace.at("events");
        log["baseline_last_pass_event"] = last_trace.value("last", json(nullptr));
        if (last_trace.at("events").empty())
            return finish("before_first_checkpoint");
        int low = -1, high = int(catalog.at("passes").size());
        while (high - low > 1) {
            const int midpoint = low + (high - low) / 2;
            const auto outcome = probe(midpoint, "bisect_prefix");
            if (outcome == "checkpoint_reached")
                low = midpoint;
            else if (outcome == "same_assertion")
                high = midpoint;
            else
                return finish("inconclusive");
            log["interval"] = {{"last_completed", low}, {"first_unreachable", high}};
            checkpoint();
        }
        // Fresh endpoint runs, never reuse the binary-search observations.
        if (low >= 0 && probe(low, "confirm_completed_boundary") != "checkpoint_reached")
            return finish("boundary_not_reproduced");
        if (probe(high == int(catalog.at("passes").size()) ? -1 : high,
                  "confirm_failure_boundary") != "same_assertion")
            return finish("boundary_not_reproduced");
        log["boundary_verified"] = true;
        log["last_completed"] = low < 0 ? json(nullptr) : catalog.at("passes").at(size_t(low));
        log["first_unreachable"] = high == int(catalog.at("passes").size())
                                       ? json(nullptr)
                                       : catalog.at("passes").at(size_t(high));
        log["failure_last_pass_event"] = last_trace.value("last", json(nullptr));
        return finish(high == int(catalog.at("passes").size()) ? "after_last_checkpoint"
                                                               : "localized");
    } catch (const std::exception &ex) {
        log["state"] = "error";
        log["error"] = ex.what();
        checkpoint();
        throw;
    }
}
} // namespace sl
