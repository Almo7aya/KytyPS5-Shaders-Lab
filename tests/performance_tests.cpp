#include "shader_lab/lab.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace sl;

unsigned performance_report_tests(const fs::path &root, Bytes shader, const fs::path &old_cli) {
    unsigned checks = 0;
    auto test = [&](bool ok, const char *name) {
        if (!ok)
            throw std::runtime_error(name);
        ++checks;
    };
    std::vector<uint8_t> payload(2 * 1024 * 1024, 0);
    std::copy(shader.begin(), shader.end(), payload.begin());
    for (unsigned i = 0; i < 32; ++i)
        write_bytes(root / "i" / (i % 2 ? "Game B" : "Game A") / (std::to_string(i) + ".bin"),
                    payload);
    auto unique = std::vector<uint8_t>(shader.begin(), shader.end());
    unique[480] ^= 1;
    write_bytes(root / "i/Game B/unique.bin", unique);
    write_text(root / "i/Empty game/notes.txt", "No shaders here");
    write_text(root / "i/Game A/archive.pkg", "Unsupported archive fixture");
    write_text(root / "i/Game B/archive.pak", "Unsupported archive fixture");
    for (const auto *game : {"Empty game", "Game A", "Game B"}) {
        write_text(root / "i" / game / "eboot.bin", "");
        atomic_json(root / "i" / game / "sce_sys/param.json",
                    {{"titleId", "PPSA00001"},
                     {"contentVersion", "01.000.000"},
                     {"localizedParameters",
                      {{"defaultLanguage", "en-US"}, {"en-US", {{"titleName", game}}}}}});
    }
    auto serial = scan({root / "i", root / "s", 0, 0, 100000, false, 1});
    auto parallel = scan({root / "i", root / "p", 0, 0, 100000, false, 4});
    test(serial.at("files") == parallel.at("files"), "serial/parallel file evidence identical");
    test(serial.at("shaders") == parallel.at("shaders"),
         "serial/parallel dedup and origins identical");
    test(parallel.at("scan_jobs") == 4 && parallel.at("shaders").size() == 2,
         "parallel scanner handles shared content");
    const auto resumed = scan({root / "i", root / "p", 0, 0, 100000, true, 4});
    test(resumed.at("shaders") == parallel.at("shaders"),
         "parallel resume preserves deterministic origins");
    for (const auto &file : resumed.at("files"))
        test(file.value("cache_hit", false), "parallel resume uses verified file cache");
    const auto limited = scan({root / "i", root / "l", 3, 0, 100000, false, 4});
    test(limited.at("limited") == true && limited.at("file_count") == 3,
         "parallel file limit preserved");
    report(root / "p", {}, root / "report.html");
    const auto bytes = read_bytes(root / "report.html");
    const std::string html(bytes.begin(), bytes.end());
    const std::string marker = "<script id=report-data type=application/json>";
    const auto start = html.find(marker) + marker.size();
    const auto data = json::parse(html.substr(start, html.find("</script>", start) - start));
    test(data.at("games").size() == 3, "zero-shader game retained as a tab");
    const auto &games = data.at("games");
    test(games[0].at("name") == "Empty game" && games[0].at("case_ids").empty(),
         "empty game has no foreign shaders");
    test(games[1].at("case_ids").size() == 1 && games[2].at("case_ids").size() == 2,
         "game-specific unique case counts");
    test(games[1].at("file_count") == 19 && games[2].at("file_count") == 20,
         "game-specific file counts");
    test(games[1].at("gap_indices").size() == 1 && games[2].at("gap_indices").size() == 1,
         "findings assigned to owning game");
    test(html.find("id=\"game-tabs\"") != std::string::npos &&
             html.find("id=\"game-filter\"") == std::string::npos,
         "game tabs replace combined-games filter");
    write_text(root / "flat/file.bin", "not a shader");
    const auto flat = scan({root / "flat", root / "f"});
    test(flat.at("files").begin().value().at("game") == "" && flat.at("games").empty(),
         "flat input without eboot is not a game");
    if (!old_cli.empty()) {
        const auto before = process(
            old_cli, {"scan", "--input", path_text(root / "i"), "--output", path_text(root / "b")},
            root, root / "before.log", std::chrono::seconds(120));
        test(!before.timed_out && before.exit_code == 0, "baseline scanner benchmark completed");
        const auto cli = executable_path().parent_path() /
                         fs::path("shader-lab").replace_extension(executable_path().extension());
        const auto after = process(cli,
                                   {"scan", "--input", path_text(root / "i"), "--output",
                                    path_text(root / "a"), "--jobs", "4"},
                                   root, root / "after.log", std::chrono::seconds(120));
        test(!after.timed_out && after.exit_code == 0, "updated scanner benchmark completed");
        std::cout << "BENCHMARK: 64 MiB synthetic corpus, baseline=" << before.elapsed_ms
                  << "ms, updated=" << after.elapsed_ms
                  << "ms (warm filesystem, not a full-game guarantee)\n";
        test(nlohmann::json::parse(read_json(root / "b/manifest.json").at("shaders").dump()) ==
                 nlohmann::json::parse(read_json(root / "a/manifest.json").at("shaders").dump()),
             "optimized hashing and extraction preserve baseline identities and provenance");
        test(read_json(root / "b/manifest.json").at("files") ==
                 read_json(root / "a/manifest.json").at("files"),
             "optimized file hashing matches portable baseline hashes");
    }
    return checks;
}
