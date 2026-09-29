#include "shader_lab/lab.hpp"
#include <algorithm>
#include <fstream>
#include <regex>
namespace sl {
json inspect(const fs::path &dataset, std::string hash) {
    auto d = read_json(dataset / "manifest.json");
    if (hash.starts_with("0x"))
        hash.erase(0, 2);
    std::transform(hash.begin(), hash.end(), hash.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    json cases = json::array();
    for (auto it = d["shaders"].begin(); it != d["shaders"].end(); ++it)
        if (it.key() == hash || it.value().value("kyty_hash", "") == hash) {
            auto c = it.value();
            c["id"] = it.key();
            cases.push_back(c);
        }
    return {{"schema", 1},
            {"hash", hash},
            {"cases", cases},
            {"note", "One code hash may have multiple header variants; select the complete case ID "
                     "for unambiguous replay."}};
}
json correlate(const fs::path &dataset, const fs::path &log) {
    auto d = read_json(dataset / "manifest.json");
    std::ifstream stream(log);
    if (!stream)
        throw std::runtime_error("cannot open trace log");
    json r = {{"schema", 1},
              {"trace_kind", "log observations, not guest execution or full shader coverage"},
              {"log_sha256", hash_file(log)},
              {"observed", json::object()},
              {"runtime_only", json::array()},
              {"offline_unobserved", json::array()}};
    std::regex expression("hash=(?:0x)?([0-9a-fA-F]{16})(?![0-9a-fA-F])");
    std::string line;
    uint64_t n = 0;
    while (std::getline(stream, line)) {
        ++n;
        if (line.size() > 1024 * 1024)
            continue;
        for (std::sregex_iterator it(line.begin(), line.end(), expression), end; it != end; ++it) {
            auto hash = (*it)[1].str();
            std::transform(hash.begin(), hash.end(), hash.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
            auto &entry = r["observed"][hash];
            if (entry.is_null())
                entry = {{"count", 0},
                         {"first_line", n},
                         {"examples", json::array()},
                         {"cases", json::array()}};
            entry["count"] = entry["count"].get<uint64_t>() + 1;
            if (entry["examples"].size() < 3)
                entry["examples"].push_back(line.substr(0, 2048));
        }
    }
    for (auto it = d["shaders"].begin(); it != d["shaders"].end(); ++it) {
        auto hash = it.value().at("kyty_hash").get<std::string>();
        if (r["observed"].contains(hash))
            r["observed"][hash]["cases"].push_back(it.key());
        else
            r["offline_unobserved"].push_back(it.key());
    }
    for (auto it = r["observed"].begin(); it != r["observed"].end(); ++it)
        if (it.value()["cases"].empty())
            r["runtime_only"].push_back(it.key());
    r["lines_read"] = n;
    return r;
}
json cluster(const fs::path &results) {
    auto d = read_json(results);
    json groups = json::object();
    for (auto it = d["results"].begin(); it != d["results"].end(); ++it) {
        auto &row = it.value();
        auto status = row.value("status", "unknown");
        if (status == "spirv_valid_under_profile")
            continue;
        auto details = row.value("details", json::object());
        std::string signature =
            status + " / " +
            details.value("last_phase",
                          row.value("last_phase", json::object()).value("phase", "unknown"));
        auto ops = details.value("unsupported", json::array());
        if (!ops.empty())
            signature += " / " + ops[0].value("family", "") + ":" +
                         std::to_string(ops[0].value("opcode_id", 0));
        auto diagnostics = details.value("validator_messages", json::array());
        if (!diagnostics.empty())
            signature += " / " + diagnostics[0].value("message", "");
        if (status == "worker_crash_or_error") {
            const auto log = row.value("log_tail", "");
            const auto marker = log.rfind("--- Error ---");
            if (marker != std::string::npos) {
                auto start = log.find_first_not_of("\r\n", marker + 13);
                if (start != std::string::npos) {
                    auto reason = log.substr(start, log.find_first_of("\r\n", start) - start);
                    reason = std::regex_replace(reason, std::regex("hash=0x[0-9a-fA-F]+"),
                                                "hash=<case>");
                    reason = std::regex_replace(reason, std::regex("pc=0x[0-9a-fA-F]+"),
                                                "pc=<instruction>");
                    const auto location = reason.find(" in ");
                    if (location != std::string::npos)
                        reason.resize(location);
                    signature += " / " + reason;
                }
            }
        }
        auto &group = groups[signature];
        if (group.is_null())
            group = {{"count", 0}, {"cases", json::array()}};
        group["count"] = group["count"].get<uint64_t>() + 1;
        group["cases"].push_back(it.key());
    }
    return {{"schema", 1},
            {"grouping", "triage heuristic; same group does not prove same root cause"},
            {"groups", groups}};
}
} // namespace sl
