#include "shader_lab/lab.hpp"
#include <stdexcept>
using namespace sl;

unsigned workflow_tests(const fs::path &root, Bytes shader, const fs::path &fixture_worker) {
    unsigned checks = 0;
    auto test = [&](bool ok, const char *label) {
        if (!ok)
            throw std::runtime_error(label);
        ++checks;
    };
    auto rejects = [&](auto action, const char *label) {
        bool rejected = false;
        try {
            action();
        } catch (const std::exception &) {
            rejected = true;
        }
        test(rejected, label);
    };
    const auto input = root / "input with spaces", out = root / "output with spaces";
    write_bytes(input / "sample.bin", shader);
    write_text(input / "eboot.bin", "");
    const auto original_hash = hash_file(input / "sample.bin");
    rejects([&] { analyze_folders(input, input / "output", fixture_worker); },
            "nested output rejected");
    rejects([&] { analyze_folders(input, root, fixture_worker); }, "ancestor output rejected");
    rejects([&] { analyze_folders(input, out, root / "absent"); }, "missing worker rejected");
    test(!fs::exists(out), "missing worker fails before creating output");
    rejects([&] { analyze_folders(input / "sample.bin", out, fixture_worker); },
            "non-folder input rejected");
    const auto first = analyze_folders(input, out, fixture_worker);
    test(first.at("status") == "completed" && first.at("shaders") == 1,
         "automatic pipeline completed");
    test(first.at("semantic_correctness") == "not_tested",
         "pipeline never claims semantic correctness");
    test(fs::is_regular_file(out / "report.html") && fs::is_regular_file(out / "failures.json") &&
             fs::is_regular_file(out / "run/results.json"),
         "all automatic outputs exist");
    test(!fs::exists(out / ".workflow-lock"), "workflow lock released");
    analyze_folders(input, out, fixture_worker);
    test(read_json(out / "run/results.json").at("results").begin().value().at("cache_hit") == true,
         "automatic compilation resumes");
    test(read_json(out / "dataset/manifest.json").at("files").at("sample.bin").at("cache_hit") ==
             true,
         "automatic scan resumes");
    test(hash_file(input / "sample.bin") == original_hash, "input bytes unchanged");
    fs::create_directory(out / ".workflow-lock");
    rejects([&] { analyze_folders(input, out, fixture_worker); }, "concurrent workflow rejected");
    fs::remove(out / ".workflow-lock");
    fs::create_directories(root / "different input");
    rejects([&] { analyze_folders(root / "different input", out, fixture_worker); },
            "different input cannot reuse output");
    write_text(root / "unrelated/keep.txt", "keep me");
    rejects([&] { analyze_folders(input, root / "unrelated", fixture_worker); },
            "unowned output rejected");
    test(fs::is_regular_file(root / "unrelated/keep.txt"), "unrelated output preserved");
    const auto empty =
        analyze_folders(root / "different input", root / "empty output", fixture_worker);
    test(empty.at("shaders") == 0 && fs::is_regular_file(root / "empty output/report.html"),
         "empty input gets a report");

    // Install an isolated synthetic package. Worker discovery must use the executable location,
    // not the unrelated working directory or an executable supplied by the scanned files.
    const auto ext = fixture_worker.extension();
    const auto cli = root / "package with spaces" / fs::path("shader-lab").replace_extension(ext);
    fs::create_directories(cli.parent_path());
    fs::copy_file(fixture_worker.parent_path() / cli.filename(), cli);
    const auto sibling = cli.parent_path() / fs::path("shader-kyty-worker").replace_extension(ext);
    fs::copy_file(fixture_worker, sibling);
    auto invoke = [&](const std::vector<std::string> &args) {
        return process(cli, args, root / "unrelated cwd", root / "cli.log",
                       std::chrono::seconds(30));
    };
    auto result = invoke({path_text(input), path_text(root / "cli output")});
    test(result.exit_code == 0 && !result.timed_out,
         "positional CLI discovers sibling worker from a different cwd");
    result = invoke({"--output", path_text(root / "cli output"), "--input", path_text(input)});
    test(result.exit_code == 0 && !result.timed_out,
         "named two-folder CLI supports reversed options and resume");
    test(invoke({"--input", path_text(input)}).exit_code == 2, "incomplete CLI rejected");
    test(invoke({"--input", path_text(input), "--input", path_text(input)}).exit_code == 2,
         "duplicate CLI option rejected");
    test(invoke({"--input", path_text(input), "--output", path_text(out), "--allow-gpu"})
                 .exit_code == 2,
         "GPU opt-in without semantic mode rejected");
    test(invoke({path_text(input), path_text(root / "cli output"), "--semantic"}).exit_code == 0,
         "positional main command supports semantic assessment without GPU");
    auto assessed = read_json(root / "cli output/run/results.json").at("semantics");
    test(assessed.at("cases").begin().value().at("status") == "backend_unavailable",
         "isolated package without CPU worker records automatic generation blocker");
    test(invoke({"--semantic", "--allow-gpu", "--input", path_text(input), "--output",
                 path_text(root / "cli output")})
                 .exit_code == 0,
         "named main command supports both semantic flags; no fixtures means no device work");
    test(invoke({path_text(input), path_text(root / "cli output"), "--semantic", "--semantic"})
                 .exit_code == 2,
         "duplicate semantic option rejected");
    test(invoke({path_text(input), path_text(root / "cli output")}).exit_code == 0,
         "compiler-only resume after semantic assessment succeeds");
    test(read_json(root / "cli output/run/results.json")
                 .at("semantics")
                 .at("cases")
                 .begin()
                 .value()
                 .at("status") == "not_requested",
         "disabled semantic mode cannot display stale execution results as current");
    test(invoke({"--help-advanced"}).exit_code == 0, "advanced help retained");
    fs::rename(sibling, sibling.parent_path() / "disabled-worker");
    test(invoke({path_text(input), path_text(root / "missing worker output")}).exit_code == 2,
         "CLI fails clearly when bundled worker is missing");
    test(!fs::exists(root / "missing worker output"),
         "CLI missing-worker failure does not start scanning");
    // A non-executable file is deliberately not a worker. The phase journal must retain failure.
    write_text(root / "broken-worker", "not an executable");
    rejects([&] { analyze_folders(input, root / "broken output", root / "broken-worker"); },
            "broken worker rejected");
    const auto failed = read_json(root / "broken output/summary.json");
    test(failed.at("status") == "failed" && failed.at("phase") == "preflight",
         "failed stage retained in summary");
    test(!fs::exists(root / "broken output/.workflow-lock"), "failure releases workflow lock");
    return checks;
}

unsigned workflow_real_tests(const fs::path &root, Bytes shader, const fs::path &worker) {
    write_bytes(root / "input/sample.bin", shader);
    write_text(root / "input/eboot.bin", "");
    const auto cli =
        worker.parent_path() / fs::path("shader-lab").replace_extension(worker.extension());
    const auto result = process(cli, {path_text(root / "input"), path_text(root / "output")}, root,
                                root / "workflow.log", std::chrono::seconds(60));
    if (result.timed_out || result.exit_code != 0)
        throw std::runtime_error("real compiler automatic CLI failed: " +
                                 path_text(root / "workflow.log"));
    const auto summary = read_json(root / "output/summary.json");
    if (summary.at("status_counts").value("spirv_valid_under_profile", 0) != 1 ||
        !fs::is_regular_file(root / "output/report.html"))
        throw std::runtime_error(
            "real compiler automatic CLI did not validate the synthetic compute shader");
    return 2;
}
