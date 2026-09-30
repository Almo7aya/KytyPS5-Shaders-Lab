#include "shader_lab/lab.hpp"
#include <algorithm>
#include <iostream>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace sl {
unsigned automatic_jobs(unsigned cap, uint64_t memory_per_worker) {
    unsigned jobs = std::clamp(std::thread::hardware_concurrency() / 2, 1u, cap);
#ifdef _WIN32
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory) && memory_per_worker)
        jobs = std::min(jobs, unsigned(std::clamp<uint64_t>(
                                  memory.ullAvailPhys / 2 / memory_per_worker, 1, cap)));
#else
    (void)memory_per_worker;
    jobs = std::min(jobs, 4u); // Conservative fallback when available memory is not queried.
#endif
    return jobs;
}
fs::path executable_path() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    auto length = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
    if (!length || length >= buffer.size())
        throw std::runtime_error("cannot locate running shader-lab executable");
    buffer.resize(length);
    return fs::canonical(fs::path(buffer));
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        throw std::runtime_error("cannot locate running shader-lab executable");
    return fs::canonical(buffer.c_str());
#else
    return fs::canonical("/proc/self/exe");
#endif
}

json analyze_folders(const fs::path &input, const fs::path &output, const fs::path &worker_path,
                     SemanticOptions semantic) {
    if (semantic.allow_gpu && !semantic.enabled)
        throw std::runtime_error("--allow-gpu requires --semantic");
    if (input.empty() || output.empty())
        throw std::runtime_error("input and output folders must not be empty");
    const auto in = fs::canonical(input), out = fs::weakly_canonical(fs::absolute(output));
    if (!fs::is_directory(in))
        throw std::runtime_error("input must be a folder");
    if (is_within(out, in) || is_within(in, out))
        throw std::runtime_error("input and output must be disjoint folders");
    if (!fs::is_regular_file(worker_path))
        throw std::runtime_error(
            "missing bundled shader-kyty-worker; use the full package or build "
            "the Kyty worker beside shader-lab (no download is performed)");
    const auto worker = fs::canonical(worker_path);
    // Never adopt an arbitrary directory or redirect managed outputs through links.
    for (const auto *name : {"summary.json", "dataset", "run", "semantic", "report.html",
                             "failures.json", "compiler-info.json"}) {
        auto target = out / name;
        if (fs::is_symlink(fs::symlink_status(target)) || !is_within(target, out))
            throw std::runtime_error("workflow output paths must not be redirected: " +
                                     path_text(target));
    }
    if (fs::exists(out) && !fs::is_empty(out)) {
        if (!fs::is_regular_file(out / "summary.json"))
            throw std::runtime_error(
                "output is not empty; choose a fresh folder or an existing automatic run");
        const auto previous = read_json(out / "summary.json");
        if (previous.value("schema", 0) != 1 ||
            previous.value("workflow", "") != "folder-analysis" ||
            previous.value("input", "") != path_text(in))
            throw std::runtime_error(
                "output belongs to a different workflow/input; choose another folder");
    }
    fs::create_directories(out);
    if (!fs::create_directory(out / ".workflow-lock"))
        throw std::runtime_error(
            "output locked; confirm no analysis is running before removing .workflow-lock");
    struct Lock {
        fs::path path;
        ~Lock() {
            std::error_code ec;
            fs::remove(path, ec);
        }
    } lock{out / ".workflow-lock"};
    const unsigned jobs = automatic_jobs(16, 2ull * 1024 * 1024 * 1024);
    json summary = {{"schema", 1},
                    {"workflow", "folder-analysis"},
                    {"input", path_text(in)},
                    {"status", "in_progress"},
                    {"phase", "preflight"},
                    {"jobs", jobs},
                    {"timeout_ms", 30000},
                    {"semantic_correctness", "not_tested"},
                    {"semantic_requested", semantic.enabled},
                    {"gpu_opt_in", semantic.allow_gpu},
                    {"execution", "compiler_only"},
                    {"profile", "header_probe"},
                    {"host_subgroup_size", 32},
                    {"limitations",
                     "No all-shaders guarantee. Missing captured state is not inferred. "
                     "Valid SPIR-V does not prove equivalent execution or rendering."}};
    auto phase = [&](const char *name, const char *message) {
        summary["phase"] = name;
        atomic_json(out / "summary.json", summary);
        std::cout << message << "\n" << std::flush;
    };
    try {
        phase("preflight", "[1/5] Checking bundled Kyty compiler...");
        const auto probe = process(worker, {"--compiler-info"}, out, out / "compiler-info.json",
                                   std::chrono::seconds(10));
        if (probe.timed_out || probe.exit_code != 0)
            throw std::runtime_error("bundled compiler could not start; see compiler-info.json and "
                                     "check its dependency DLLs");
        summary["compiler"] = read_json(out / "compiler-info.json");
        if (summary["compiler"].value("worker_protocol", 0) != 1)
            throw std::runtime_error("bundled compiler has an incompatible worker protocol");
        phase("scan", "[2/5] Identifying eboot game roots and extracting their shaders...");
        ScanOptions scan_options{in, out / "dataset"};
        scan_options.games_only = true;
        const auto manifest = scan(scan_options);
        summary["games"] = manifest.at("games");
        summary["excluded_non_game_files"] = manifest.at("excluded_non_game_files");
        summary["scan_jobs"] = manifest.at("scan_jobs");
        summary["scan_elapsed_ms"] = manifest.at("elapsed_ms");
        size_t read_errors = 0;
        for (const auto &file : manifest.at("files"))
            if (file.value("status", "") == "read_error")
                ++read_errors;
        const bool incomplete = manifest.at("limited").get<bool>() ||
                                !manifest.at("traversal_errors").empty() || read_errors != 0;
        summary["files"] = manifest.at("file_count");
        summary["shaders"] = manifest.at("shader_count");
        summary["scan_incomplete"] = incomplete;
        summary["read_errors"] = read_errors;
        phase("compile", "[3/5] Compiling extracted cases (cached cases resume automatically)...");
        auto results = run({out / "dataset", out / "run", worker, {}, jobs, 30000, 0, true});
        summary["status_counts"] = results.at("status_counts");
        summary["compile_elapsed_ms"] = results.at("elapsed_ms");
        phase("semantic",
              "Assessing offline semantic evidence (device execution requires opt-in)...");
        auto replay_worker = worker.parent_path() /
                             fs::path("shader-vulkan-replay").replace_extension(worker.extension());
        auto semantics = validate_semantics(in, out / "semantic", manifest, replay_worker, semantic,
                                            out / "dataset");
        atomic_json(out / "semantic/results.json", semantics);
        results["semantics"] = semantics;
        atomic_json(out / "run/results.json", results);
        summary["semantic_status_counts"] = semantics.at("status_counts");
        summary["semantic_issues"] = semantics.at("issues");
        summary["semantic_correctness"] = semantic.enabled ? "not_proven" : "not_tested";
        summary["execution"] =
            semantic.enabled ? "compiler_and_semantic_assessment" : "compiler_only";
        phase("cluster", "[4/5] Grouping failures for investigation...");
        atomic_json(out / "failures.json", cluster(out / "run/results.json"));
        phase("report", "[5/5] Generating the offline HTML report...");
        report(out / "dataset", out / "run/results.json", out / "report.html");
        summary["status"] = incomplete ? "completed_with_scan_gaps" : "completed";
        summary["phase"] = "complete";
        summary["report"] = "report.html";
        summary["results"] = "run/results.json";
        summary["exit_code"] = incomplete ? 3 : 0;
        atomic_json(out / "summary.json", summary);
        std::cout << "\nAnalyzed " << summary["shaders"] << " unique shader cases from "
                  << summary["files"] << " files.\n"
                  << summary["status_counts"].dump(2)
                  << "\nReport: " << path_text(out / "report.html") << '\n';
        if (semantic.enabled)
            std::cout << "Semantic assessment: " << summary["semantic_status_counts"].dump()
                      << "\nMatches apply only to tested inputs; 100% correctness is NOT PROVEN.\n";
        else
            std::cout << "Semantic correctness: NOT TESTED (compiler validation only).\n";
        if (incomplete)
            std::cout
                << "Warning: scan limits or I/O errors left gaps; see the report and manifest.\n";
        return summary;
    } catch (const std::exception &ex) {
        summary["status"] = "failed";
        summary["error"] = ex.what();
        summary["exit_code"] = 2;
        try {
            atomic_json(out / "summary.json", summary);
        } catch (...) {
        }
        throw;
    }
}
} // namespace sl
