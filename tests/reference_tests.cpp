#include "shader_lab/lab.hpp"
#include <stdexcept>
using namespace sl;

unsigned reference_tests(const fs::path &root) {
    unsigned checks = 0;
    auto check = [&](bool ok, const char *message) {
        if (!ok) throw std::runtime_error(message);
        ++checks;
    };
    // Synthetic arithmetic vectors, independently specified in little-endian
    // bits. No translated shader output is used to generate its own oracle.
    auto words = [](std::initializer_list<uint32_t> values) {
        std::vector<uint8_t> bytes;
        for (auto value : values)
            for (unsigned shift = 0; shift < 32; shift += 8)
                bytes.push_back(uint8_t(value >> shift));
        return bytes;
    };
    auto digest = sha256(words({0, 1, 2, 3}));
    json fixture = {{"id", "synthetic-vector-1"}, {"shader_sha256", digest},
                    {"input_sha256", digest}, {"profile_sha256", digest},
                    {"wave_size", 32}, {"exec_mask", "00000000ffffffff"}};
    json source = {{"kind", "independent_model"}, {"identifier", "synthetic-arithmetic-v1"},
                   {"method", "Explicit integer and IEEE-754 bit-pattern expectations"}};
    json reference = {{"schema", 1}, {"fixture", fixture}, {"reference_source", source},
                      {"outputs", json::object()}};
    json observed = {{"schema", 1}, {"fixture", fixture}, {"outputs", json::object()}};
    const auto ref_file = root / "reference.json", obs_file = root / "observed.json";
    auto output = [&](json &record, std::string file, const std::vector<uint8_t> &bytes,
                      const std::string &type, json comparison) {
        write_bytes(root / path_from(file), bytes);
        record["outputs"]["out"] = {{"file", file}, {"sha256", sha256(bytes)},
                                     {"kind", "buffer"}, {"type", type},
                                     {"comparison", comparison}};
    };
    auto compare = [&] {
        atomic_json(ref_file, reference);
        atomic_json(obs_file, observed);
        return verify_reference(ref_file, obs_file);
    };
    auto rejected = [&] {
        bool failed = false;
        try { compare(); } catch (const std::exception &) { failed = true; }
        return failed;
    };
    output(reference, "expected.bin", words({0, 1, 0xffffffff}), "u32", {{"mode", "exact"}});
    output(observed, "actual.bin", words({0, 1, 0xffffffff}), "u32", {{"mode", "exact"}});
    auto result = compare();
    check(result["status"] == "match" && result["semantic_correctness"] == "not_proven",
          "reference exact match remains fixture scoped");
    observed["fixture"].erase("id");
    observed["fixture"]["id"] = "synthetic-vector-1";
    check(compare()["status"] == "match", "fixture member order is not identity");
    output(observed, "actual.bin", words({0, 2, 0xffffffff}), "u32", {{"mode", "exact"}});
    result = compare();
    check(result["status"] == "mismatch" && result["outputs"]["out"]["mismatch_count"] == 1 &&
              result["outputs"]["out"]["examples"][0]["element"] == 1,
          "reference exact integer mismatch locates element");
    observed["fixture"]["input_sha256"] = std::string(64, '0');
    check(compare()["status"] == "incomparable", "different inputs are not semantic failures");
    observed["fixture"] = fixture;
    observed["fixture"]["exec_mask"] = "0000000000000001";
    check(compare()["status"] == "incomparable", "different EXEC state is incomparable");
    observed["fixture"]["exec_mask"] = "ffffffffffffffff";
    check(rejected(), "wave32 EXEC overflow rejected");
    observed["fixture"] = fixture;
    auto saved_hash = observed["outputs"]["out"]["sha256"];
    observed["outputs"]["out"]["sha256"] = std::string(64, '0');
    check(rejected(), "tampered resource identity rejected");
    observed["outputs"]["out"]["sha256"] = saved_hash;
    observed["outputs"]["out"]["file"] = "../outside.bin";
    check(rejected(), "reference path traversal rejected");
    observed["outputs"]["out"]["file"] = "actual.bin";
    reference["outputs"]["out"]["comparison"]["mode"] = "approximate";
    check(rejected(), "integer tolerances rejected");
    reference["outputs"]["out"]["comparison"]["mode"] = "exact";
    reference["outputs"]["out"]["kind"] = "image";
    reference["outputs"]["out"]["shape"] = json::array({3, 1, 1, 1});
    observed["outputs"]["out"]["kind"] = "image";
    observed["outputs"]["out"]["shape"] = json::array({3, 1, 1, 1});
    check(compare()["status"] == "mismatch", "image comparison uses tightly packed components");
    observed["outputs"]["out"]["shape"] = json::array({1, 3, 1, 1});
    check(compare()["status"] == "incomparable", "different image shapes cannot compare");
    observed["outputs"]["out"]["shape"] = json::array({UINT64_MAX, 3, 1, 1});
    check(rejected(), "image extent overflow rejected");
    json float_rules = {{"mode", "float32"}, {"absolute_tolerance", 0.0},
                        {"relative_tolerance", 0.0}, {"max_ulps", 0},
                        {"nan_policy", "reject"}, {"signed_zero_policy", "distinct"}};
    output(reference, "expected.bin", words({0x3f800000}), "f32", float_rules);
    output(observed, "actual.bin", words({0x3f800001}), "f32", float_rules);
    check(compare()["status"] == "mismatch", "float exact rule detects adjacent values");
    reference["outputs"]["out"]["comparison"]["max_ulps"] = 1;
    check(compare()["status"] == "match", "explicit float ULP allowance");
    reference["outputs"]["out"]["comparison"] = float_rules;
    reference["outputs"]["out"]["comparison"]["absolute_tolerance"] = 0.000001;
    check(compare()["status"] == "match", "explicit float absolute tolerance");
    reference["outputs"]["out"]["comparison"] = float_rules;
    reference["outputs"]["out"]["comparison"]["relative_tolerance"] = 0.000001;
    check(compare()["status"] == "match", "explicit float relative tolerance");
    reference["outputs"]["out"]["comparison"]["relative_tolerance"] = -1;
    check(rejected(), "negative float tolerance rejected");
    output(reference, "expected.bin", words({0xbf800000}), "f32", float_rules);
    output(observed, "actual.bin", words({0xbf800001}), "f32", float_rules);
    reference["outputs"]["out"]["comparison"]["max_ulps"] = 1;
    check(compare()["status"] == "match", "negative float ULP ordering");
    output(reference, "expected.bin", words({0x00000000}), "f32", float_rules);
    output(observed, "actual.bin", words({0x80000000}), "f32", float_rules);
    check(compare()["status"] == "mismatch", "signed zero distinct policy");
    reference["outputs"]["out"]["comparison"]["signed_zero_policy"] = "equal";
    check(compare()["status"] == "match", "signed zero equality must be explicit");
    output(reference, "expected.bin", words({0x7fc00001}), "f32", float_rules);
    output(observed, "actual.bin", words({0x7fc00001}), "f32", float_rules);
    check(compare()["status"] == "mismatch", "NaN rejected even with identical bits");
    reference["outputs"]["out"]["comparison"]["nan_policy"] = "equal_bits";
    check(compare()["status"] == "match", "explicit identical NaN policy");
    output(observed, "actual.bin", words({0x7fc00002}), "f32", float_rules);
    check(compare()["status"] == "mismatch", "NaN payload identity retained");
    output(reference, "expected.bin", words({0x7f800000}), "f32", float_rules);
    output(observed, "actual.bin", words({0x7f800000}), "f32", float_rules);
    check(compare()["status"] == "match", "same infinity matches");
    output(observed, "actual.bin", words({0xff800000}), "f32", float_rules);
    check(compare()["status"] == "mismatch", "opposite infinities mismatch");
    observed["outputs"].clear();
    check(rejected(), "empty observed output is not a vacuous pass");
    return checks;
}
