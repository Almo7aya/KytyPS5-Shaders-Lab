#include "shader_lab/lab.hpp"
#include <map>
#include <set>

namespace sl {
namespace {
std::string text_field(const json &object, const char *key) {
    if (!object.is_object() || !object.contains(key) || !object.at(key).is_string())
        return {};
    const auto value = object.at(key).get<std::string>();
    const auto first = value.find_first_not_of(" \t\r\n");
    return first == std::string::npos
               ? std::string{}
               : value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
bool regular_unlinked(const fs::path &path) {
    std::error_code ec;
    return fs::is_regular_file(fs::symlink_status(path, ec)) && !ec;
}
json read_game(const fs::path &root, const fs::path &directory, const std::string &key) {
    json game = {{"key", key},
                 {"root", key},
                 {"name", "Unknown title"},
                 {"title_id", ""},
                 {"version", ""},
                 {"content_id", ""},
                 {"master_version", ""},
                 {"title_language", ""},
                 {"metadata_status", "missing"},
                 {"eboot", path_text((directory / "eboot.bin").lexically_relative(root))},
                 {"param", path_text((directory / "sce_sys/param.json").lexically_relative(root))},
                 {"issues", json::array()}};
    const auto param = directory / "sce_sys/param.json";
    try {
        if (!regular_unlinked(param)) {
            game["issues"].push_back(
                "sce_sys/param.json is missing, not a regular file, or is a symbolic link");
            return game;
        }
        if (fs::is_symlink(fs::symlink_status(directory / "sce_sys")) ||
            !is_within(param, directory))
            throw std::runtime_error(
                "metadata path is redirected outside the game root or through a symbolic link");
        const auto bytes = read_bytes(param, 4 * 1024 * 1024);
        const auto metadata = json::parse(bytes.begin(), bytes.end());
        if (!metadata.is_object())
            throw std::runtime_error("param.json must contain an object");
        game["param_sha256"] = sha256(bytes);
        game["title_id"] = text_field(metadata, "titleId");
        game["version"] = text_field(metadata, "contentVersion");
        game["content_id"] = text_field(metadata, "contentId");
        game["master_version"] = text_field(metadata, "masterVersion");
        const auto localized = metadata.value("localizedParameters", json::object());
        const auto default_language = text_field(localized, "defaultLanguage");
        std::vector<std::string> languages{default_language, "en-US"};
        if (localized.is_object()) {
            std::set<std::string> remaining;
            for (auto it = localized.begin(); it != localized.end(); ++it)
                if (it.value().is_object())
                    remaining.insert(it.key());
            languages.insert(languages.end(), remaining.begin(), remaining.end());
        }
        for (const auto &language : languages) {
            if (language.empty() || !localized.is_object() || !localized.contains(language))
                continue;
            auto title = text_field(localized.at(language), "titleName");
            if (!title.empty()) {
                game["name"] = title;
                game["title_language"] = language;
                if (language != default_language)
                    game["issues"].push_back("Default-language title unavailable; using locale " +
                                             language);
                break;
            }
        }
        if (game["title_language"] == "")
            game["issues"].push_back("Localized titleName is missing or invalid");
        if (game["title_id"] == "")
            game["issues"].push_back("titleId is missing or invalid");
        if (game["version"] == "")
            game["issues"].push_back("contentVersion is missing or invalid");
        game["metadata_status"] = game["issues"].empty() ? "complete" : "incomplete";
    } catch (const std::exception &ex) {
        game["metadata_status"] = "invalid";
        game["issues"].push_back(ex.what());
    }
    return game;
}
} // namespace

json identify_games(const fs::path &root_path, const std::vector<fs::path> &files) {
    const auto root = fs::weakly_canonical(root_path);
    json catalog = {{"identification", "eboot-param-v1"},
                    {"games", json::object()},
                    {"owners", json::object()}};
    std::map<fs::path, bool> valid_roots;
    std::map<std::string, json> games;
    for (const auto &file : files) {
        const auto relative = file.lexically_relative(root);
        // Dataset manifests are untrusted: never inspect an absolute or escaping source entry.
        if (relative.empty() || relative.is_absolute())
            continue;
        bool escaping = false;
        for (const auto &part : relative)
            if (part == "..")
                escaping = true;
        if (escaping || !is_within(file, root))
            continue;
        std::string owner;
        for (auto parent = file.parent_path();; parent = parent.parent_path()) {
            if (!valid_roots.contains(parent))
                valid_roots[parent] =
                    regular_unlinked(parent / "eboot.bin") && is_within(parent / "eboot.bin", root);
            if (valid_roots.at(parent)) {
                owner = path_text(parent.lexically_relative(root));
                if (owner.empty())
                    owner = ".";
                if (!games.contains(owner))
                    games[owner] = read_game(root, parent, owner);
                break;
            }
            if (parent == root || parent == parent.parent_path())
                break;
        }
        catalog["owners"][path_text(relative)] = owner;
    }
    for (const auto &[key, game] : games)
        catalog["games"][key] = game;
    return catalog;
}
} // namespace sl
