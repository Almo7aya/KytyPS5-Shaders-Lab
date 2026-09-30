#include "shader_lab/lab.hpp"
#include <iostream>
#include <map>
#include <set>
namespace {
void usage() {
    std::cout << R"(PS5 Shader Lab 0.1.0 -- offline shader extraction and compiler regression lab
  shader-lab scan --input GAMES --output DATASET [--max-files N] [--max-file-mb N] [--no-resume]
  shader-lab run --dataset DATASET --worker EXE --output RUN [--profile JSON] [--jobs N] [--timeout-ms N] [--limit N] [--no-resume]
  shader-lab campaign --dataset DATASET --worker EXE --plan JSON --output CAMPAIGN [--jobs N] [--timeout-ms N] [--limit N] [--no-resume]
  shader-lab report --dataset DATASET --output report.html [--results RUN/results.json]
  shader-lab compare --before RUN/results.json --after RUN/results.json --output diff.json
  shader-lab inspect --dataset DATASET --hash KYTY_HASH_OR_CASE_ID --output shader.json
  shader-lab correlate --dataset DATASET --log EMULATOR_LOG --output trace.json
  shader-lab cluster --results RUN/results.json --output failures.json
  shader-lab verify --reference JSON --observed JSON --output comparison.json
  shader-lab repro --dataset DATASET --results RUN/results.json --case CASE_ID --output NEW_BUNDLE
  shader-lab replay --bundle BUNDLE --worker EXE --output NEW_RUN [--timeout-ms N]
  shader-lab minimize --bundle BUNDLE --worker EXE --output NEW_DIRECTORY [--max-attempts N] [--confirmations N] [--timeout-ms N]
Scan recursively inspects all regular files, not just eboot.bin. Game files are read-only.
No guest execution. No decryption. Unsupported containers need prior unpacking.
Compiler success and valid SPIR-V do NOT prove rendering/semantic correctness.
)";
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc < 2 || std::string(argv[1]) == "--help") {
            usage();
            return 0;
        }
        std::string command = argv[1];
        std::map<std::string, std::string> args;
        bool resume = true;
        const std::map<std::string, std::set<std::string>> allowed = {
            {"scan", {"--input", "--output", "--max-files", "--max-file-mb"}},
            {"run",
             {"--dataset", "--worker", "--output", "--profile", "--jobs", "--timeout-ms",
              "--limit"}},
            {"report", {"--dataset", "--output", "--results"}},
            {"campaign", {"--dataset", "--worker", "--plan", "--output", "--jobs", "--timeout-ms", "--limit"}},
            {"compare", {"--before", "--after", "--output"}},
            {"inspect", {"--dataset", "--hash", "--output"}},
            {"correlate", {"--dataset", "--log", "--output"}},
            {"cluster", {"--results", "--output"}},
            {"verify", {"--reference", "--observed", "--output"}},
            {"repro", {"--dataset", "--results", "--case", "--output"}},
            {"replay", {"--bundle", "--worker", "--output", "--timeout-ms"}},
            {"minimize", {"--bundle", "--worker", "--output", "--timeout-ms", "--max-attempts", "--confirmations"}}};
        if (!allowed.contains(command))
            throw std::runtime_error("unknown command");
        for (int i = 2; i < argc; ++i) {
            std::string k = argv[i];
            if (k == "--no-resume" && (command == "run" || command == "scan" || command == "campaign")) {
                resume = false;
                continue;
            }
            if (!allowed.at(command).contains(k) || args.contains(k) || i + 1 == argc)
                throw std::runtime_error("unknown, duplicate or incomplete option: " + k);
            args[k] = argv[++i];
        }
        auto path = [&](const std::string &k, bool required = true) {
            if (!args.contains(k)) {
                if (required)
                    throw std::runtime_error("missing " + k);
                return sl::fs::path{};
            }
            return sl::path_from(args.at(k));
        };
        auto number = [&](const std::string &k, uint64_t fallback) {
            if (!args.contains(k))
                return fallback;
            const auto &s = args.at(k);
            if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
                throw std::runtime_error("invalid unsigned number: " + k);
            size_t used = 0;
            auto n = std::stoull(s, &used);
            if (used != s.size())
                throw std::runtime_error("invalid number");
            return uint64_t(n);
        };
        if (command == "scan") {
            auto mb = number("--max-file-mb", 0);
            if (mb > UINT64_MAX / (1024 * 1024))
                throw std::runtime_error("size overflow");
            auto r = sl::scan({path("--input"), path("--output"), number("--max-files", 0),
                               mb * 1024 * 1024, 100000, resume});
            std::cout << "Extracted " << r["shader_count"] << " header+code cases.\n";
            return r["limited"].get<bool>() || !r["traversal_errors"].empty() ? 3 : 0;
        }
        if (command == "run" || command == "campaign") {
            auto jobs = number("--jobs", 1);
            if (jobs > 64 || !jobs)
                throw std::runtime_error("jobs must be 1..64");
            sl::RunOptions options{path("--dataset"), path("--output"), path("--worker"),
                                   path("--profile", false), unsigned(jobs),
                                   number("--timeout-ms", 30000), number("--limit", 0), resume};
            auto r = command == "campaign" ? sl::campaign(options, path("--plan")) : sl::run(options);
            std::cout << (command == "campaign" ? r["contexts"] : r["status_counts"]).dump(2) << "\n";
            return 0;
        }
        if (command == "report")
            sl::report(path("--dataset"), path("--results", false), path("--output"));
        if (command == "repro") {
            if (!args.contains("--case"))
                throw std::runtime_error("missing --case");
            auto result = sl::export_repro(path("--dataset"), path("--results"),
                                          args.at("--case"), path("--output"));
            std::cout << result.dump(2) << "\n";
        }
        if (command == "replay") {
            auto result = sl::replay_repro(path("--bundle"), path("--worker"),
                                          path("--output"), number("--timeout-ms", 0));
            std::cout << result.dump(2) << "\n";
        }
        if (command == "compare")
            sl::atomic_json(path("--output"), sl::compare(path("--before"), path("--after")));
        if (command == "minimize") {
            auto confirmations = number("--confirmations", 2);
            if (confirmations < 2 || confirmations > 5)
                throw std::runtime_error("confirmations must be 2..5");
            auto result = sl::minimize_repro({path("--bundle"), path("--worker"), path("--output"),
                number("--timeout-ms", 0), number("--max-attempts", 128), unsigned(confirmations)});
            std::cout << result.dump(2) << "\n";
            return result.value("final_verified", false) ? 0 : 4;
        }
        if (command == "inspect") {
            if (!args.contains("--hash"))
                throw std::runtime_error("missing --hash");
            sl::atomic_json(path("--output"), sl::inspect(path("--dataset"), args.at("--hash")));
        }
        if (command == "correlate")
            sl::atomic_json(path("--output"), sl::correlate(path("--dataset"), path("--log")));
        if (command == "cluster")
            sl::atomic_json(path("--output"), sl::cluster(path("--results")));
        if (command == "verify") {
            auto output = path("--output");
            if (sl::fs::exists(output))
                throw std::runtime_error("verify requires a fresh output file to preserve evidence");
            auto result = sl::verify_reference(path("--reference"), path("--observed"));
            sl::atomic_json(output, result);
            return result["status"] == "match" ? 0 : 4;
        }
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return 2;
    }
}
