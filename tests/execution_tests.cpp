#include "shader_lab/lab.hpp"
#include <thread>

using namespace sl;
namespace {
std::vector<uint8_t> words(std::initializer_list<uint32_t> values) {
    std::vector<uint8_t> bytes;
    for (auto value : values)
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.push_back(uint8_t(value >> shift));
    return bytes;
}
} // namespace

// Protocol-only backend. It does not interpret the shader or create a GPU device.
int execution_fixture_worker(const fs::path &request_path) {
    const auto q = read_json(request_path);
    const auto out = path_from(q.at("output").get<std::string>());
    const auto profile = read_json(path_from(q.at("profile").get<std::string>()));
    const auto mode = profile.value("test_execution", "match");
    if (mode == "timeout")
        std::this_thread::sleep_for(std::chrono::seconds(10));
    if (mode == "fail")
        return 19;
    if (q.at("schema") != 1 || q.at("kind") != "shader_lab_execution_request" ||
        q.at("execution").at("wave_size") != 32 ||
        q.at("execution").at("exec_mask") != "00000000ffffffff" ||
        q.at("execution").at("workgroup_size") != json({32, 1, 1}) ||
        q.at("execution").at("dispatch_size") != json({1, 1, 1}))
        return 20;
    json response = {{"schema", 1},
                     {"kind", "shader_lab_execution_response"},
                     {"request_sha256", hash_file(request_path)},
                     {"status", "completed"},
                     {"backend",
                      {{"kind", q.at("backend_kind")},
                       {"identifier", "protocol-test-double/1"},
                       {"method", "Synthetic resource increment; no shader/GPU execution"}}},
                     {"outputs", json::object()}};
    unsigned index = 0;
    for (auto it = q.at("resources").begin(); it != q.at("resources").end(); ++it) {
        const auto &entry = it.value();
        if (entry.at("access") == "read_only")
            continue;
        auto bytes = read_bytes(path_from(entry.at("file").get<std::string>()));
        const unsigned width = entry.at("type") == "u8" ? 1 : 4;
        for (size_t offset = 0; offset < bytes.size(); offset += width) {
            const auto value = uint32_t(integer(bytes, offset, width)) + 1u;
            for (unsigned i = 0; i < width; ++i)
                bytes[offset + i] = uint8_t(value >> (8 * i));
        }
        if (mode == "mismatch")
            bytes[0] ^= 1;
        if (mode == "wrong-size")
            bytes.pop_back();
        const auto name = std::to_string(index++) + ".bin";
        write_bytes(out / name, bytes);
        response["outputs"][it.key()] = {{"file", name}, {"sha256", sha256(bytes)}};
    }
    if (mode == "mutate-input")
        write_bytes(path_from(q.at("code").get<std::string>()), words({0}));
    if (mode == "wrong-request")
        response["request_sha256"] = std::string(64, '0');
    if (mode == "wrong-kind")
        response["backend"]["kind"] = "unrecognized";
    if (mode == "wrong-hash")
        response["outputs"].begin().value()["sha256"] = std::string(64, '0');
    if (mode == "extra-output")
        response["outputs"]["extra"] = response["outputs"].begin().value();
    if (mode == "missing-output")
        response["outputs"].erase(response["outputs"].begin());
    if (mode == "escape")
        response["outputs"].begin().value()["file"] = "../inputs/0.bin";
    if (mode == "unsupported") {
        response["status"] = "unsupported";
        response["reason"] = "Synthetic backend does not implement the requested instruction";
        response.erase("outputs");
    }
    if (mode == "bad-json")
        write_text(out / "response.json", "{not valid json");
    else
        atomic_json(out / "response.json", response);
    return 0;
}

unsigned execution_tests(const fs::path &root, Bytes header, const fs::path &worker) {
    unsigned checks = 0;
    auto test = [&](bool ok, const char *name) {
        if (!ok)
            throw std::runtime_error(name);
        ++checks;
    };
    auto fixture_at = [&](const std::string &name, const std::string &mode) {
        auto dir = root / name / "fixture";
        write_bytes(dir / "header.bin", header);
        write_bytes(dir / "code.bin", words({0xbf810000u}));
        atomic_json(dir / "profile.json", {{"test_execution", mode}});
        write_bytes(dir / "buffer.bin", words({0, 1, 2}));
        write_bytes(dir / "image.bin", std::vector<uint8_t>{0, 1, 2, 3});
        write_bytes(dir / "input.bin", std::vector<uint8_t>{10, 20});
        auto file = [&](const char *name) {
            return json{{"file", name}, {"sha256", hash_file(dir / name)}};
        };
        json resources = json::object();
        unsigned binding = 0;
        for (const char *name : {"buffer", "image", "input"}) {
            const bool image = std::string_view(name) == "image",
                       input = std::string_view(name) == "input";
            auto entry = file((std::string(name) + ".bin").c_str());
            entry["access"] = input ? "read_only" : "read_write";
            entry["kind"] = image ? "image" : "buffer";
            entry["type"] = image || input ? "u8" : "u32";
            entry["binding"] = {0, binding};
            entry["guest_address"] = hex(uint64_t(++binding) * 4096, 16);
            if (image)
                entry["shape"] = {4, 1, 1, 1};
            resources[name] = entry;
        }
        json fixture = {{"schema", 1},
                        {"kind", "shader_lab_execution_fixture"},
                        {"id", "synthetic-execution-protocol"},
                        {"shader", {{"header", file("header.bin")}, {"code", file("code.bin")}}},
                        {"profile", file("profile.json")},
                        {"execution",
                         {{"stage", "CS"},
                          {"wave_size", 32},
                          {"exec_mask", "00000000ffffffff"},
                          {"workgroup_size", {32, 1, 1}},
                          {"dispatch_size", {1, 1, 1}}}},
                        {"resources", resources}};
        atomic_json(dir / "fixture.json", fixture);
        // Constants specify only this protocol test's mock operation, not an ISA oracle.
        write_bytes(dir / "expected-buffer.bin", words({1, 2, 3}));
        write_bytes(dir / "expected-image.bin", std::vector<uint8_t>{1, 2, 3, 4});
        json expected = json::object();
        for (const char *name : {"buffer", "image"}) {
            auto entry = file((std::string("expected-") + name + ".bin").c_str());
            entry["kind"] = resources.at(name).at("kind");
            entry["type"] = resources.at(name).at("type");
            entry["comparison"] = {{"mode", "exact"}};
            if (resources.at(name).contains("shape"))
                entry["shape"] = resources.at(name).at("shape");
            expected[name] = entry;
        }
        atomic_json(dir / "reference.json",
                    {{"schema", 1},
                     {"fixture", execution_fixture_identity(dir / "fixture.json")},
                     {"reference_source",
                      {{"kind", "independent_model"},
                       {"identifier", "protocol-constants/1"},
                       {"method", "Explicit expected bytes for protocol test double only"}}},
                     {"outputs", expected}});
        return ExecuteOptions{dir / "fixture.json",
                              dir / "reference.json",
                              worker,
                              root / name / "run",
                              5000,
                              "cpu",
                              false};
    };
    auto options = fixture_at("match", "match");
    auto result = execute_fixture(options);
    test(result.at("status") == "match" && result.at("comparison").at("outputs").size() == 2,
         "isolated backend outputs are compared as buffers and images");
    test(result.at("semantic_correctness") == "not_proven" &&
             result.at("reference_authenticity") == "user_supplied_not_independently_authenticated",
         "execution match never authenticates a reference or proves general shader equivalence");
    test(result.at("request_sha256") == hash_file(options.output / "request.json") &&
             result.at("worker_sha256") == hash_file(worker),
         "execution receipts bind the request and selected backend binary");
    test(read_bytes(options.fixture.parent_path() / "code.bin") == words({0xbf810000u}),
         "source shader remains unchanged by execution");
    test(verify_reference(options.output / "reference/record.json",
                          options.output / "observed/record.json")
                 .at("status") == "match",
         "copied evidence supports independent offline re-comparison");
    bool overwrite = false;
    try {
        execute_fixture(options);
    } catch (const std::exception &) {
        overwrite = true;
    }
    test(overwrite, "execution never overwrites a prior attempt");
    for (const auto &[mode, expected] :
         std::initializer_list<std::pair<const char *, const char *>>{
             {"mismatch", "mismatch"},
             {"unsupported", "unsupported"},
             {"fail", "backend_error"},
             {"bad-json", "execution_error"},
             {"wrong-request", "execution_error"},
             {"wrong-kind", "execution_error"},
             {"wrong-hash", "execution_error"},
             {"wrong-size", "execution_error"},
             {"extra-output", "execution_error"},
             {"missing-output", "execution_error"},
             {"escape", "execution_error"},
             {"mutate-input", "execution_error"}}) {
        auto selected = fixture_at(mode, mode);
        const auto attempt = execute_fixture(selected);
        test(attempt.at("status") == expected, "backend failure must not become an output match");
        test(read_json(selected.output / "execution.json").at("status") == expected,
             "failed execution retains a durable receipt");
    }
    auto timeout = fixture_at("timeout", "timeout");
    timeout.timeout_ms = 150;
    test(execute_fixture(timeout).at("status") == "backend_timeout",
         "execution backend deadline terminates the child");
    auto gpu = fixture_at("gpu-gate", "match");
    gpu.backend_kind = "gpu";
    bool denied = false;
    try {
        execute_fixture(gpu);
    } catch (const std::exception &) {
        denied = true;
    }
    test(denied && !fs::exists(gpu.output),
         "GPU request without opt-in is rejected before creating or launching anything");
    // Tests only the opt-in protocol: the test double never loads a GPU driver.
    gpu.allow_gpu = true;
    test(execute_fixture(gpu).at("gpu_opt_in") == true,
         "explicit GPU permission is propagated and recorded");
    auto invalid = fixture_at("invalid", "match");
    const auto original = read_json(invalid.fixture);
    auto reject = [&](json fixture, const char *name) {
        atomic_json(invalid.fixture, fixture);
        bool rejected = false;
        try {
            execution_fixture_identity(invalid.fixture);
        } catch (const std::exception &) {
            rejected = true;
        }
        test(rejected, name);
    };
    auto changed = original;
    changed["execution"]["exec_mask"] = "ffffffffffffffff";
    reject(changed, "execution identity rejects wave32 EXEC overflow");
    changed = original;
    changed["schema"] = 1.0;
    reject(changed, "schema version cannot be a floating-point coercion");
    changed = original;
    changed["unexpected"] = true;
    reject(changed, "unknown fixture fields rejected");
    changed = original;
    changed["execution"]["dispatch_size"] = {UINT64_MAX, 1, 1};
    reject(changed, "execution invocation count cannot overflow");
    changed = original;
    changed["execution"]["workgroup_size"][0] = 32.5;
    reject(changed, "execution dimensions cannot coerce floating-point values");
    changed = original;
    changed["resources"]["image"]["guest_address"] =
        changed["resources"]["buffer"]["guest_address"];
    reject(changed, "overlapping guest resources rejected");
    changed = original;
    changed["resources"]["image"]["binding"] = changed["resources"]["buffer"]["binding"];
    reject(changed, "duplicate descriptor bindings rejected");
    changed = original;
    changed["resources"]["image"]["shape"] = {UINT64_MAX, 1, 1, 1};
    reject(changed, "image layout overflow rejected");
    changed = original;
    changed["resources"]["buffer"]["sha256"] = std::string(64, '0');
    reject(changed, "input resource content is actually hashed");
    changed = original;
    changed["resources"]["buffer"]["file"] = "../outside.bin";
    reject(changed, "fixture cannot read outside its resource directory");
    changed = original;
    changed["resources"]["buffer"]["access"] = "read_only";
    changed["resources"]["image"]["access"] = "read_only";
    reject(changed, "execution cannot produce a vacuous no-output pass");
    atomic_json(invalid.fixture, original);
    const auto identity = execution_fixture_identity(invalid.fixture);
    auto reordered = json::parse(nlohmann::json(original).dump());
    atomic_json(invalid.fixture, reordered);
    test(nlohmann::json(execution_fixture_identity(invalid.fixture)) == nlohmann::json(identity),
         "execution identity is independent of JSON object ordering");
    auto nonoverlap = invalid;
    nonoverlap.output = invalid.fixture.parent_path() / "attempt";
    bool overlap = false;
    try {
        execute_fixture(nonoverlap);
    } catch (const std::exception &) {
        overlap = true;
    }
    test(overlap && !fs::exists(nonoverlap.output),
         "execution cannot write into its source fixture tree");
    auto reference = read_json(invalid.reference);
    reference["fixture"]["input_sha256"] = std::string(64, '0');
    atomic_json(invalid.reference, reference);
    bool wrong_reference = false;
    try {
        execute_fixture(invalid);
    } catch (const std::exception &) {
        wrong_reference = true;
    }
    test(wrong_reference && !fs::exists(invalid.output),
         "wrong reference inputs reject before launching backend");
    return checks;
}
