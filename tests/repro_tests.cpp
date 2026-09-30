#include "shader_lab/lab.hpp"

using namespace sl;

unsigned repro_tests(const fs::path &root, Bytes fixture, const fs::path &worker) {
    unsigned checks = 0;
    auto test = [&](bool ok, const char *name) {
        if (!ok)
            throw std::runtime_error(name);
        ++checks;
    };
    auto rejects = [&](auto action, const char *name) {
        bool rejected = false;
        try { action(); }
        catch (const std::exception &) { rejected = true; }
        test(rejected, name);
    };
    write_bytes(root / "input/fixture.elf", fixture);
    auto manifest = scan({root / "input", root / "dataset"});
    auto id = manifest.at("shaders").begin().key();
    auto profiles = root / "profiles.json";
    atomic_json(profiles, {{"default", {{"test_mode", "error"}}},
                          {"cases", {{id, {{"test_mode", "ok"}}}}}});
    auto original = run({root / "dataset", root / "source-run", worker, profiles, 1, 5000, 0, false});
    auto results = root / "source-run/results.json";
    auto artifact_dir = root / "source-run" / path_from(original["results"][id]["artifacts"].get<std::string>());
    write_text(artifact_dir / "do-not-export.exe", "not a replay dependency");
    write_text(artifact_dir / "guest.asm", "synthetic diagnostics");
    auto bundle = export_repro(root / "dataset", results, id, root / "bundle");
    test(bundle["replay_state"] == "not_run", "export is not a successful replay claim");
    test(read_json(root / "bundle/profile.json") == json{{"test_mode", "ok"}},
         "repro selects per-case profile instead of bundle default");
    test(bundle["artifacts"].contains("guest.asm") && !fs::exists(root / "bundle/evidence/do-not-export.exe"),
         "repro copies only allowlisted diagnostic artifacts");
    test(!fs::exists(root / "bundle/evidence/request.json"), "repro never reuses absolute-path worker requests");
    test(bundle["files"]["code.bin"] == id.substr(65), "repro fingerprints copied bytes");
    fs::rename(root / "bundle", root / "relocated bundle");
    auto moved = root / "relocated bundle";
    auto replay = replay_repro(moved, worker, root / "replay");
    test(replay["worker_matches"] == true && replay["deadline_matches"] == true &&
         replay["status_matches"] == true && replay["status"] == "fixture_pass",
         "moved bundle replays with original worker and selected profile");
    auto replay_results = read_json(root / "replay/run/results.json");
    test(replay_results["results"][id]["cache_hit"] == false, "repro replay always runs a fresh process");
    test(replay["semantic_correctness"] == "not_tested", "replay is not semantic equivalence");
    rejects([&] { replay_repro(moved, worker, root / "replay"); }, "replay cannot overwrite old evidence");
    rejects([&] { export_repro(root / "dataset", results, id, moved); }, "export cannot overwrite a bundle");
    rejects([&] { export_repro(root / "dataset", results, "../bad", root / "bad-id"); }, "repro rejects invalid case paths");
    rejects([&] { export_repro(root / "dataset", results, id, root / "input/export"); }, "repro protects source game tree");
    rejects([&] { replay_repro(moved, worker, moved / "run"); }, "replay cannot write into its bundle");
    auto code = read_bytes(moved / "code.bin");
    auto changed = code;
    changed[0] ^= 1;
    write_bytes(moved / "code.bin", changed);
    rejects([&] { replay_repro(moved, worker, root / "corrupt-replay"); }, "replay rejects changed shader content");
    test(!fs::exists(root / "corrupt-replay"), "replay checks integrity before creating output");
    write_bytes(moved / "code.bin", code);
    auto object_code = root / "dataset/objects" / id / "code.bin";
    write_bytes(object_code, changed);
    rejects([&] { export_repro(root / "dataset", results, id, root / "corrupt-export"); }, "export verifies source content");
    write_bytes(object_code, code);
    auto changed_summary = original;
    changed_summary["profile"]["cases"][id]["test_mode"] = "error";
    atomic_json(results, changed_summary);
    rejects([&] { export_repro(root / "dataset", results, id, root / "bad-profile"); }, "export rejects tampered profile snapshot");
    changed_summary = original;
    changed_summary["results"][id]["artifacts"] = "../input";
    atomic_json(results, changed_summary);
    rejects([&] { export_repro(root / "dataset", results, id, root / "escaped-artifacts"); }, "export rejects escaped artifact directory");
    test(!fs::exists(root / "escaped-artifacts/repro.json"), "failed export has no committed bundle descriptor");
    atomic_json(results, original);
    auto unknown_deadline = bundle;
    unknown_deadline["timeout_ms"] = nullptr;
    atomic_json(moved / "repro.json", unknown_deadline);
    rejects([&] { replay_repro(moved, worker, root / "no-deadline"); }, "older evidence requires explicit replay deadline");
    replay = replay_repro(moved, worker, root / "override-deadline", 5000);
    test(replay["deadline_matches"] == false && replay["status_matches"] == true,
         "unknown original deadline cannot be reported as reproduced");
    atomic_json(moved / "repro.json", bundle);
    // Exercise symlink containment on hosts that permit creation without extra privilege.
    std::error_code ec;
    fs::rename(moved / "code.bin", root / "outside-code.bin");
    fs::create_symlink(root / "outside-code.bin", moved / "code.bin", ec);
    if (!ec) {
        rejects([&] { replay_repro(moved, worker, root / "symlink-replay"); }, "repro input symlink cannot escape the bundle");
        fs::remove(moved / "code.bin");
    }
    fs::rename(root / "outside-code.bin", moved / "code.bin");
    return checks;
}
