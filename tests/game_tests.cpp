#include "shader_lab/lab.hpp"
#include <stdexcept>
using namespace sl;

unsigned game_tests(const fs::path &root, Bytes shader) {
    unsigned checks = 0;
    auto test = [&](bool ok, const char *message) {
        if (!ok)
            throw std::runtime_error(message);
        ++checks;
    };
    auto param = [](const std::string &title, const std::string &version) {
        return json{{"titleId", "PPSA00001"},
                    {"contentVersion", version},
                    {"contentId", "TEST-CONTENT-ID"},
                    {"masterVersion", "01.00"},
                    {"localizedParameters",
                     {{"defaultLanguage", "fr-FR"},
                      {"fr-FR", {{"titleName", title}}},
                      {"en-US", {{"titleName", "English fallback"}}}}}};
    };
    const auto input = root / "i";
    write_bytes(input / "folder-a/eboot.bin", shader);
    write_bytes(input / "folder-a/assets/shared.bin", shader);
    atomic_json(input / "folder-a/sce_sys/param.json", param("Real title", "01.002.003"));
    write_bytes(input / "folder-b/eboot.bin", shader);
    atomic_json(input / "folder-b/sce_sys/param.json", param("Real title", "02.000.001"));
    write_bytes(input / "folder-a/nested/eboot.bin", shader);
    atomic_json(input / "folder-a/nested/sce_sys/param.json", param("Nested title", "03.000.000"));
    write_bytes(input / "not-a-game/shader.bin", shader);
    atomic_json(input / "not-a-game/sce_sys/param.json", param("Not a game", "99.000.000"));
    fs::create_directories(input / "directory-marker/eboot.bin");
    write_bytes(input / "directory-marker/shader.bin", shader);
    write_text(input / "missing/eboot.bin", "");
    write_text(input / "broken/eboot.bin", "");
    write_text(input / "broken/sce_sys/param.json", "{ broken");
    auto incomplete = param("Unused", "01.000.000");
    incomplete["localizedParameters"]["fr-FR"]["titleName"] = 3;
    incomplete["titleId"] = 42;
    incomplete["contentVersion"] = nullptr;
    write_text(input / "incomplete/eboot.bin", "");
    atomic_json(input / "incomplete/sce_sys/param.json", incomplete);
    ScanOptions options{input, root / "d"};
    options.games_only = true;
    auto manifest = scan(options);
    const auto &games = manifest.at("games");
    test(games.size() == 6, "only regular eboot roots are identified");
    test(!games.contains("not-a-game") && !games.contains("directory-marker"),
         "metadata-only and directory markers rejected");
    test(games.at("folder-a").at("name") == "Real title",
         "default language title replaces folder name");
    test(games.at("folder-a").at("title_language") == "fr-FR", "selected locale recorded");
    test(games.at("folder-a").at("title_id") == "PPSA00001" &&
             games.at("folder-a").at("version") == "01.002.003",
         "ID and content version come from param.json exactly");
    test(games.at("folder-b").at("name") == games.at("folder-a").at("name") &&
             games.at("folder-b").at("version") != games.at("folder-a").at("version"),
         "duplicate title IDs and names do not merge installations");
    test(manifest.at("files").at("folder-a/assets/shared.bin").at("game") == "folder-a",
         "asset subdirectory belongs to ancestor game");
    test(manifest.at("files").at("folder-a/nested/eboot.bin").at("game") == "folder-a/nested",
         "nearest nested eboot wins");
    test(games.at("missing").at("metadata_status") == "missing" &&
             games.at("missing").at("name") == "Unknown title",
         "missing metadata never becomes a guessed title");
    test(games.at("broken").at("metadata_status") == "invalid", "malformed metadata disclosed");
    test(games.at("incomplete").at("name") == "English fallback" &&
             games.at("incomplete").at("title_id") == "" &&
             games.at("incomplete").at("version") == "",
         "locale fallback and invalid fields are explicit");
    test(!manifest.at("files").contains("not-a-game/shader.bin") &&
             manifest.at("excluded_non_game_files").get<size_t>() == 3,
         "normal scan excludes unrelated files before reading shader bytes");
    test(manifest.at("shader_count") == 1 &&
             manifest.at("shaders").begin().value().at("origins").size() == 4,
         "shared shader provenance includes only valid game files");
    atomic_json(input / "folder-a/sce_sys/param.json", param("Updated title", "04.005.006"));
    const auto resumed = scan(options);
    test(resumed.at("games").at("folder-a").at("name") == "Updated title" &&
             resumed.at("games").at("folder-a").at("version") == "04.005.006",
         "resume refreshes metadata independently of shader cache");
    test(resumed.at("files").at("folder-a/eboot.bin").at("cache_hit") == true,
         "unchanged shaders still resume");
    auto read_report = [&](const fs::path &file) {
        auto bytes = read_bytes(file);
        std::string html(bytes.begin(), bytes.end());
        const std::string marker = "<script id=report-data type=application/json>";
        const auto start = html.find(marker) + marker.size();
        return json::parse(html.substr(start, html.find("</script>", start) - start));
    };
    report(root / "d", {}, root / "snapshot.html");
    auto report_data = read_report(root / "snapshot.html");
    test(report_data.at("games").size() == 6 &&
             report_data.at("game_identification_source") == "scan_snapshot",
         "report consumes validated game snapshot");
    auto raw = scan({input, root / "raw"});
    raw.erase("games");
    raw.erase("game_identification");
    for (auto &file : raw["files"])
        file["game"] = "Bogus old folder label";
    atomic_json(root / "raw/manifest.json", raw);
    report(root / "raw", {}, root / "legacy.html");
    const auto legacy = read_report(root / "legacy.html");
    test(legacy.at("games").size() == 6 &&
             legacy.at("game_identification_source") == "source_eboot_and_param_refresh",
         "legacy report labels revalidated from actual source eboots");
    test(legacy.at("non_game_files") == 3, "legacy invalid folders excluded from tabs");
    fs::remove(input / "folder-b/eboot.bin");
    const auto removed = scan(options);
    test(!removed.at("games").contains("folder-b"),
         "removed eboot removes game even when param remains");
    const auto single = scan({input / "folder-a", root / "single"});
    test(single.at("games").contains(".") &&
             single.at("games").at(".").at("name") == "Updated title",
         "single-game input uses current root metadata");
    return checks;
}
