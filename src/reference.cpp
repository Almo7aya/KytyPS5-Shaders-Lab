#include "shader_lab/lab.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace sl {
namespace {
bool hash_string(const json &value) {
    if (!value.is_string()) return false;
    const auto &text = value.get_ref<const std::string &>();
    return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}
uint64_t unsigned_integer(const json &value) {
    if (!value.is_number_unsigned() && !(value.is_number_integer() && value.get<int64_t>() >= 0))
        throw std::runtime_error("reference integer must be nonnegative and integral");
    return value.get<uint64_t>();
}
void identity(const json &document) {
    if (!document.is_object() || document.value("schema", 0) != 1)
        throw std::runtime_error("unsupported reference record schema");
    const auto &fixture = document.at("fixture");
    if (!fixture.is_object() || fixture.at("id").get<std::string>().empty() ||
        !hash_string(fixture.at("shader_sha256")) || !hash_string(fixture.at("input_sha256")) ||
        !hash_string(fixture.at("profile_sha256")))
        throw std::runtime_error("reference fixture requires shader, input and profile identities");
    const auto wave = unsigned_integer(fixture.at("wave_size"));
    const auto mask = fixture.at("exec_mask").get<std::string>();
    if ((wave != 32 && wave != 64) || mask.size() != 16 ||
        mask.find_first_not_of("0123456789abcdef") != std::string::npos ||
        (wave == 32 && mask.substr(0, 8) != "00000000"))
        throw std::runtime_error("invalid reference wave size or 16-digit EXEC mask");
    if (!document.at("outputs").is_object() || document["outputs"].empty() ||
        document["outputs"].size() > 128)
        throw std::runtime_error("reference record requires 1..128 named outputs");
}
struct FloatRules {
    double absolute, relative;
    uint64_t ulps;
    bool nan_bits, distinct_zero;
};
FloatRules rules(const json &value) {
    if (!value.is_object() || value.size() != 6)
        throw std::runtime_error("float32 comparison requires exactly six explicit rule fields");
    if (value.at("mode") != "float32")
        throw std::runtime_error("f32 output requires explicit float32 comparison rules");
    auto absolute = value.at("absolute_tolerance").get<double>();
    auto relative = value.at("relative_tolerance").get<double>();
    auto ulps = unsigned_integer(value.at("max_ulps"));
    auto nan = value.at("nan_policy").get<std::string>();
    auto zero = value.at("signed_zero_policy").get<std::string>();
    if (!std::isfinite(absolute) || !std::isfinite(relative) || absolute < 0 || relative < 0 ||
        ulps > UINT32_MAX || (nan != "reject" && nan != "equal_bits") ||
        (zero != "distinct" && zero != "equal"))
        throw std::runtime_error("invalid floating-point comparison rule");
    return {absolute, relative, ulps, nan == "equal_bits", zero == "distinct"};
}
bool float_equal(uint32_t a, uint32_t b, const FloatRules &rule) {
    constexpr uint32_t magnitude = 0x7fffffff, infinity = 0x7f800000;
    const auto am = a & magnitude, bm = b & magnitude;
    if (am > infinity || bm > infinity) return rule.nan_bits && a == b;
    if (am == infinity || bm == infinity) return a == b;
    if (am == 0 && bm == 0) return !rule.distinct_zero || a == b;
    if (a == b) return true;
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
    double x = std::bit_cast<float>(a), y = std::bit_cast<float>(b);
    // Tolerances are alternatives to ULP distance; their finite numeric bound is
    // abs_tol + rel_tol * max(abs(reference), abs(observed)).
    const auto scale = std::max(std::abs(x), std::abs(y));
    if (std::abs(x - y) <= rule.absolute + rule.relative * scale) return true;
    auto ordered = [](uint32_t bits) -> uint64_t {
        return (bits & 0x80000000u) ? ~bits : (bits | 0x80000000u);
    };
    const auto ax = ordered(a), bx = ordered(b);
    return (ax > bx ? ax - bx : bx - ax) <= rule.ulps;
}
std::vector<uint8_t> resource(const fs::path &record, const json &entry, uint64_t &budget) {
    auto path = fs::weakly_canonical(record.parent_path() / path_from(entry.at("file").get<std::string>()));
    if (!is_within(path, record.parent_path()) || !fs::is_regular_file(path))
        throw std::runtime_error("reference resource must be a regular file inside its record directory");
    if (!hash_string(entry.at("sha256")))
        throw std::runtime_error("reference resource requires a SHA-256 identity");
    auto size = fs::file_size(path);
    if (size > budget) throw std::runtime_error("reference resource byte budget exceeded");
    budget -= size;
    auto bytes = read_bytes(path, size);
    if (sha256(bytes) != entry["sha256"].get<std::string>())
        throw std::runtime_error("reference resource content hash mismatch");
    return bytes;
}
unsigned layout(const json &entry, uint64_t bytes) {
    auto type = entry.at("type").get<std::string>();
    const unsigned width = type == "u8" ? 1 : type == "u32" || type == "i32" || type == "f32" ? 4 : 0;
    if (!width || bytes == 0 || bytes % width)
        throw std::runtime_error("reference output must contain complete supported scalar elements");
    auto kind = entry.at("kind").get<std::string>();
    if (kind == "image") {
        const auto &shape = entry.at("shape");
        if (!shape.is_array() || shape.size() != 4)
            throw std::runtime_error("image shape must be [width, height, depth, channels]");
        uint64_t elements = 1;
        for (const auto &dimension : shape) {
            auto n = unsigned_integer(dimension);
            if (!n || n > bytes / width / elements)
                throw std::runtime_error("image extent overflow or output size mismatch");
            elements *= n;
        }
        if (elements != bytes / width)
            throw std::runtime_error("image output must be tightly packed in row-major order");
    } else if (kind != "buffer") {
        throw std::runtime_error("unsupported reference resource kind");
    }
    return width;
}
} // namespace

json verify_reference(const fs::path &reference_path, const fs::path &observed_path) {
    const auto reference_file = fs::canonical(reference_path), observed_file = fs::canonical(observed_path);
    auto reference = read_json(reference_file), observed = read_json(observed_file);
    identity(reference);
    identity(observed);
    const auto &source = reference.at("reference_source");
    if (!source.is_object() || source.at("identifier").get<std::string>().empty() ||
        source.at("method").get<std::string>().empty())
        throw std::runtime_error("reference_source must identify the independent reference and method");
    auto kind = source.at("kind").get<std::string>();
    if (kind != "independent_model" && kind != "hardware_capture")
        throw std::runtime_error("reference source must be an independent model or hardware capture");
    json result = {{"schema", 1}, {"reference_record_sha256", hash_file(reference_file)},
                   {"observed_record_sha256", hash_file(observed_file)}, {"reference_source", source},
                   {"reference_authenticity", "user_supplied_not_independently_authenticated"},
                   {"semantic_correctness", "not_proven"}, {"status", "incomparable"},
                   {"outputs", json::object()}};
    // JSON object member order is not part of execution identity.
    if (nlohmann::json(reference["fixture"]) != nlohmann::json(observed["fixture"])) {
        result["reason"] = "fixture identity or guest wave/EXEC state differs";
        return result;
    }
    result["fixture"] = reference["fixture"];
    if (reference["outputs"].size() != observed["outputs"].size()) {
        result["reason"] = "output resource sets differ";
        return result;
    }
    uint64_t budget = 128 * 1024 * 1024;
    bool passed = true;
    for (auto it = reference["outputs"].begin(); it != reference["outputs"].end(); ++it) {
        if (!observed["outputs"].contains(it.key())) {
            result["reason"] = "output resource sets differ";
            return result;
        }
        const auto &expected = it.value(), &actual = observed["outputs"][it.key()];
        auto a = resource(reference_file, expected, budget), b = resource(observed_file, actual, budget);
        auto width = layout(expected, a.size());
        layout(actual, b.size());
        if (expected.at("type") != actual.at("type") || expected.at("kind") != actual.at("kind") ||
            expected.value("shape", json{}) != actual.value("shape", json{}) || a.size() != b.size()) {
            result["reason"] = "output layouts differ: " + it.key();
            return result;
        }
        bool floating = expected["type"] == "f32";
        FloatRules rule{};
        if (floating) rule = rules(expected.at("comparison"));
        else if (expected.at("comparison") != json{{"mode", "exact"}})
            throw std::runtime_error("integer outputs require exact comparison");
        json row = {{"comparison", expected["comparison"]}, {"elements", a.size() / width},
                    {"mismatch_count", 0}, {"examples", json::array()}};
        uint64_t mismatches = 0;
        for (size_t offset = 0; offset < a.size(); offset += width) {
            auto x = uint32_t(integer(a, offset, width)), y = uint32_t(integer(b, offset, width));
            if (floating ? float_equal(x, y, rule) : x == y) continue;
            ++mismatches;
            if (row["examples"].size() < 64)
                row["examples"].push_back({{"element", offset / width}, {"byte_offset", offset},
                                          {"expected_bits", hex(x, width * 2)},
                                          {"observed_bits", hex(y, width * 2)}});
        }
        row["mismatch_count"] = mismatches;
        row["status"] = mismatches ? "mismatch" : "match";
        passed = passed && mismatches == 0;
        result["outputs"][it.key()] = std::move(row);
    }
    result["status"] = passed ? "match" : "mismatch";
    result["note"] = "Only the recorded outputs of this fixture were compared. This command does not execute shaders, authenticate captures, or prove general shader equivalence.";
    return result;
}
} // namespace sl
