#include "shader_lab/lab.hpp"
#include <map>
#include <set>

namespace sl {
namespace {
constexpr uint64_t payload_limit = 64 * 1024 * 1024;
constexpr uint64_t json_limit = 1024 * 1024;
void keys(const json &object, const std::set<std::string> &allowed) {
    if (!object.is_object())
        throw std::runtime_error("execution protocol requires objects");
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!allowed.contains(it.key()))
            throw std::runtime_error("unknown execution field: " + it.key());
}
uint64_t number(const json &n) {
    if (!n.is_number_integer() || (!n.is_number_unsigned() && n.get<int64_t>() < 0))
        throw std::runtime_error("execution integers must be nonnegative and integral");
    return n.get<uint64_t>();
}
bool digest(const std::string &s) {
    return s.size() == 64 && s.find_first_not_of("0123456789abcdef") == s.npos;
}
std::string canonical_hash(const json &j) {
    const auto text = nlohmann::json(j).dump();
    return sha256({reinterpret_cast<const uint8_t *>(text.data()), text.size()});
}
json bounded_json(const fs::path &path) {
    const auto bytes = read_bytes(path, json_limit);
    return json::parse(bytes.begin(), bytes.end());
}
fs::path contained(const fs::path &root, const json &name) {
    auto relative = path_from(name.get<std::string>());
    if (relative.empty() || relative.is_absolute() || relative.has_root_name())
        throw std::runtime_error("execution resource paths must be relative");
    const auto path = fs::canonical(root / relative);
    if (!is_within(path, root) || !fs::is_regular_file(path))
        throw std::runtime_error("execution resource escapes its record directory");
    return path;
}
std::vector<uint8_t> verified(const fs::path &root, const json &entry, uint64_t &budget) {
    auto expected = entry.at("sha256").get<std::string>();
    if (!digest(expected))
        throw std::runtime_error("execution resource needs a SHA-256 hash");
    const auto path = contained(root, entry.at("file"));
    if (fs::file_size(path) > budget)
        throw std::runtime_error("execution byte budget exceeded");
    auto bytes = read_bytes(path, budget);
    budget -= bytes.size();
    if (sha256(bytes) != expected)
        throw std::runtime_error("execution resource hash mismatch");
    return bytes;
}
void layout(const json &entry, uint64_t size) {
    auto type = entry.at("type").get<std::string>();
    uint64_t width = type == "u8" ? 1 : type == "u32" || type == "i32" || type == "f32" ? 4 : 0;
    if (!width || !size || size % width)
        throw std::runtime_error("invalid execution scalar extent");
    auto kind = entry.at("kind").get<std::string>();
    if (kind == "image") {
        const auto &shape = entry.at("shape");
        if (!shape.is_array() || shape.size() != 4)
            throw std::runtime_error("execution image requires [width,height,depth,channels]");
        uint64_t elements = 1;
        for (const auto &dimension : shape) {
            auto n = number(dimension);
            if (!n || n > size / width / elements)
                throw std::runtime_error("execution image extent overflow");
            elements *= n;
        }
        if (elements != size / width)
            throw std::runtime_error("execution image size mismatch");
    } else if (kind != "buffer" || entry.contains("shape")) {
        throw std::runtime_error("invalid execution resource layout");
    }
}
void description(const json &value) {
    if (!value.is_string() || value.get_ref<const std::string &>().empty() ||
        value.get_ref<const std::string &>().size() > 4096)
        throw std::runtime_error("execution descriptions require 1..4096 bytes");
}
struct Fixture {
    json identity, execution, resources, profile, source;
    std::vector<uint8_t> header, code, profile_bytes;
    std::map<std::string, std::vector<uint8_t>> bytes;
};
Fixture load_fixture(const fs::path &file) {
    const auto root = file.parent_path();
    Fixture f;
    f.source = bounded_json(file);
    keys(f.source, {"schema", "kind", "id", "shader", "profile", "execution", "resources"});
    if (number(f.source.at("schema")) != 1 || f.source.at("kind") != "shader_lab_execution_fixture")
        throw std::runtime_error("unsupported execution fixture schema");
    description(f.source.at("id"));
    const auto &shader = f.source.at("shader");
    keys(shader, {"header", "code"});
    for (const auto *entry : {&shader.at("header"), &shader.at("code"), &f.source.at("profile")})
        keys(*entry, {"file", "sha256"});
    uint64_t budget = payload_limit;
    f.header = verified(root, shader.at("header"), budget);
    f.code = verified(root, shader.at("code"), budget);
    f.profile_bytes = verified(root, f.source.at("profile"), budget);
    if (f.profile_bytes.size() > json_limit)
        throw std::runtime_error("execution profile too large");
    f.profile = json::parse(f.profile_bytes.begin(), f.profile_bytes.end());
    std::string reason;
    if (!f.profile.is_object() || !valid_header(f.header, reason) || f.header[0x5a] != 0 ||
        f.code.size() != integer(f.header, 0x44, 4))
        throw std::runtime_error("execution fixture needs a valid compute shader and profile");
    f.execution = f.source.at("execution");
    keys(f.execution, {"stage", "wave_size", "exec_mask", "workgroup_size", "dispatch_size"});
    const auto wave = number(f.execution.at("wave_size"));
    const auto mask = f.execution.at("exec_mask").get<std::string>();
    if (f.execution.at("stage") != "CS" || (wave != 32 && wave != 64) || mask.size() != 16 ||
        mask.find_first_not_of("0123456789abcdef") != mask.npos ||
        (wave == 32 && mask.substr(0, 8) != "00000000"))
        throw std::runtime_error("invalid execution stage or guest wave/EXEC state");
    uint64_t invocations = 1;
    for (const char *field : {"workgroup_size", "dispatch_size"}) {
        const auto &dimensions = f.execution.at(field);
        if (!dimensions.is_array() || dimensions.size() != 3)
            throw std::runtime_error("execution dimensions require three integers");
        uint64_t product = 1, limit = std::string_view(field) == "workgroup_size" ? 1024 : 16777216;
        for (const auto &dimension : dimensions) {
            auto n = number(dimension);
            if (!n || n > limit / product)
                throw std::runtime_error("execution extent exceeds budget");
            product *= n;
        }
        if (product > 16777216 / invocations)
            throw std::runtime_error("execution invocation budget exceeded");
        invocations *= product;
    }
    const auto &resources = f.source.at("resources");
    if (!resources.is_object() || resources.empty() || resources.size() > 128)
        throw std::runtime_error("execution requires 1..128 resources");
    f.resources = json::object();
    std::set<std::pair<uint64_t, uint64_t>> bindings;
    std::vector<std::pair<uint64_t, uint64_t>> addresses;
    unsigned outputs = 0;
    for (auto it = resources.begin(); it != resources.end(); ++it) {
        description(it.key());
        const auto &entry = it.value();
        keys(entry,
             {"file", "sha256", "access", "kind", "type", "shape", "binding", "guest_address"});
        auto bytes = verified(root, entry, budget);
        layout(entry, bytes.size());
        const auto access = entry.at("access").get<std::string>();
        if (access != "read_only" && access != "read_write" && access != "write_only")
            throw std::runtime_error("invalid execution resource access");
        outputs += access != "read_only";
        const auto &binding = entry.at("binding");
        if (!binding.is_array() || binding.size() != 2 || number(binding[0]) > 31 ||
            number(binding[1]) > 65535 ||
            !bindings.emplace(number(binding[0]), number(binding[1])).second)
            throw std::runtime_error("invalid or duplicate descriptor set/binding");
        const auto address = entry.at("guest_address").get<std::string>();
        if (address.size() != 16 || address.find_first_not_of("0123456789abcdef") != address.npos)
            throw std::runtime_error("guest_address requires 16 lowercase hexadecimal digits");
        auto base = std::stoull(address, nullptr, 16);
        if (!base || bytes.size() > UINT64_MAX - base)
            throw std::runtime_error("invalid guest resource address extent");
        for (auto [start, end] : addresses)
            if (base < end && start < base + bytes.size())
                throw std::runtime_error("overlapping guest resource addresses");
        addresses.emplace_back(base, base + bytes.size());
        auto metadata = entry;
        metadata.erase("file");
        metadata["bytes"] = bytes.size();
        f.resources[it.key()] = metadata;
        f.bytes.emplace(it.key(), std::move(bytes));
    }
    if (!outputs)
        throw std::runtime_error("execution needs at least one observable output");
    f.identity = {
        {"id", f.source.at("id")},
        {"shader_sha256", sha256(f.code)},
        {"header_sha256", sha256(f.header)},
        {"profile_sha256", canonical_hash(f.profile)},
        {"input_sha256", canonical_hash({{"execution", f.execution}, {"resources", f.resources}})},
        {"wave_size", wave},
        {"exec_mask", mask}};
    return f;
}
void disjoint(const fs::path &out, const fs::path &input) {
    if (is_within(out, input) || is_within(input, out))
        throw std::runtime_error("execution output must not overlap input evidence or worker");
}
} // namespace

json execution_fixture_identity(const fs::path &fixture) {
    return load_fixture(fs::canonical(fixture)).identity;
}

json execute_fixture(const ExecuteOptions &options) {
    if (!options.timeout_ms || options.timeout_ms > 86400000 ||
        (options.backend_kind != "cpu" && options.backend_kind != "gpu"))
        throw std::runtime_error("invalid execution deadline or backend kind");
    if (options.backend_kind == "gpu" && !options.allow_gpu)
        throw std::runtime_error("GPU execution requires explicit --allow-gpu");
    const auto fixture_file = fs::canonical(options.fixture),
               reference_file = fs::canonical(options.reference);
    const auto worker = fs::canonical(options.worker), out = fs::weakly_canonical(options.output);
    if (!fs::is_regular_file(worker))
        throw std::runtime_error("execution worker is not a regular file");
    disjoint(out, fixture_file.parent_path());
    disjoint(out, reference_file.parent_path());
    disjoint(out, worker);
    auto fixture = load_fixture(fixture_file);
    auto reference = bounded_json(reference_file);
    if (nlohmann::json(reference.at("fixture")) != nlohmann::json(fixture.identity))
        throw std::runtime_error("reference does not identify the verified execution fixture");
    // Also validates layouts, comparison policies, provenance and actual reference bytes.
    if (verify_reference(reference_file, reference_file).at("status") != "match")
        throw std::runtime_error("reference outputs do not satisfy their own comparison policy");
    auto expected = reference.at("outputs");
    uint64_t budget = payload_limit;
    std::map<std::string, std::vector<uint8_t>> reference_bytes;
    for (auto it = expected.begin(); it != expected.end(); ++it) {
        if (!fixture.resources.contains(it.key()))
            throw std::runtime_error("unknown reference output");
        const auto &resource = fixture.resources.at(it.key());
        if (resource.at("access") == "read_only" || it.value().at("kind") != resource.at("kind") ||
            it.value().at("type") != resource.at("type") ||
            it.value().value("shape", json{}) != resource.value("shape", json{}))
            throw std::runtime_error("reference output disagrees with fixture layout/access");
        auto bytes = verified(reference_file.parent_path(), it.value(), budget);
        if (bytes.size() != number(resource.at("bytes")))
            throw std::runtime_error("reference output size mismatch");
        reference_bytes.emplace(it.key(), std::move(bytes));
    }
    for (auto it = fixture.resources.begin(); it != fixture.resources.end(); ++it)
        if (it.value().at("access") != "read_only" && !expected.contains(it.key()))
            throw std::runtime_error("reference is missing an observable output");
    const auto worker_hash = hash_file(worker);
    fs::create_directories(out.parent_path());
    if (!fs::create_directory(out))
        throw std::runtime_error("execution requires a fresh output directory");
    json result = {{"schema", 1},
                   {"kind", "shader_lab_execution_result"},
                   {"fixture", fixture.identity},
                   {"fixture_manifest_sha256", hash_file(fixture_file)},
                   {"reference_record_sha256", hash_file(reference_file)},
                   {"worker_sha256", worker_hash},
                   {"backend_kind", options.backend_kind},
                   {"gpu_opt_in", options.allow_gpu},
                   {"timeout_ms", options.timeout_ms},
                   {"semantic_correctness", "not_proven"},
                   {"reference_authenticity", "user_supplied_not_independently_authenticated"},
                   {"status", "preparing"},
                   {"phase", "snapshot"}};
    std::map<fs::path, std::string> protected_files;
    auto protect = [&](const fs::path &path, Bytes bytes) {
        write_bytes(path, bytes);
        protected_files[path] = sha256(bytes);
    };
    auto protect_json = [&](const fs::path &path, const json &j) {
        atomic_json(path, j);
        protected_files[path] = hash_file(path);
    };
    auto finish = [&](const char *status) {
        result["status"] = status;
        atomic_json(out / "execution.json", result);
        return result;
    };
    try {
        protect(out / "inputs/header.bin", fixture.header);
        protect(out / "inputs/code.bin", fixture.code);
        protect(out / "inputs/profile.json", fixture.profile_bytes);
        protect_json(out / "fixture-source.json", fixture.source);
        json resources = fixture.resources;
        unsigned index = 0;
        for (auto it = resources.begin(); it != resources.end(); ++it, ++index) {
            const auto path = out / "inputs" / (std::to_string(index) + ".bin");
            protect(path, fixture.bytes.at(it.key()));
            it.value()["file"] = path_text(path);
        }
        index = 0;
        for (auto it = expected.begin(); it != expected.end(); ++it, ++index) {
            auto name = std::to_string(index) + ".bin";
            protect(out / "reference" / name, reference_bytes.at(it.key()));
            it.value()["file"] = name;
        }
        reference["outputs"] = expected;
        protect_json(out / "reference/record.json", reference);
        const auto backend_dir = out / "backend";
        fs::create_directory(backend_dir);
        json request = {{"schema", 1},
                        {"kind", "shader_lab_execution_request"},
                        {"fixture", fixture.identity},
                        {"execution", fixture.execution},
                        {"header", path_text(out / "inputs/header.bin")},
                        {"code", path_text(out / "inputs/code.bin")},
                        {"profile", path_text(out / "inputs/profile.json")},
                        {"resources", resources},
                        {"output", path_text(backend_dir)},
                        {"backend_kind", options.backend_kind},
                        {"allow_gpu", options.allow_gpu}};
        protect_json(out / "request.json", request);
        const auto request_hash = protected_files.at(out / "request.json");
        result["request_sha256"] = request_hash;
        result["phase"] = "execute";
        atomic_json(out / "execution.json", result);
        auto attempt =
            process(worker, {"--execute-fixture", path_text(out / "request.json")}, backend_dir,
                    backend_dir / "worker.log", std::chrono::milliseconds(options.timeout_ms));
        result["exit_code"] = attempt.exit_code;
        result["elapsed_ms"] = attempt.elapsed_ms;
        result["worker_working_directory"] = path_text(attempt.working_directory);
        if (attempt.timed_out)
            return finish("backend_timeout");
        if (attempt.exit_code != 0)
            return finish("backend_error");
        result["phase"] = "validate_response";
        for (const auto &[path, hash] : protected_files)
            if (hash_file(path) != hash)
                throw std::runtime_error("backend modified protected input/reference evidence");
        if (hash_file(worker) != worker_hash)
            throw std::runtime_error("backend executable changed during execution");
        const auto response_file = contained(backend_dir, "response.json");
        const auto response = bounded_json(response_file);
        keys(response,
             {"schema", "kind", "request_sha256", "status", "backend", "reason", "outputs"});
        if (number(response.at("schema")) != 1 ||
            response.at("kind") != "shader_lab_execution_response" ||
            response.at("request_sha256") != request_hash)
            throw std::runtime_error("execution response schema/request identity mismatch");
        const auto &backend = response.at("backend");
        keys(backend, {"kind", "identifier", "method"});
        if (backend.at("kind") != options.backend_kind)
            throw std::runtime_error("execution backend kind mismatch");
        description(backend.at("identifier"));
        description(backend.at("method"));
        result["backend"] = backend;
        result["response_sha256"] = hash_file(response_file);
        if (response.at("status") == "unsupported") {
            description(response.at("reason"));
            result["reason"] = response.at("reason");
            return finish("unsupported");
        }
        if (response.at("status") != "completed")
            throw std::runtime_error("invalid execution completion status");
        const auto &outputs = response.at("outputs");
        if (!outputs.is_object() || outputs.size() != expected.size())
            throw std::runtime_error("backend did not produce the declared output set");
        json observed = {{"schema", 1}, {"fixture", fixture.identity}, {"outputs", json::object()}};
        budget = payload_limit;
        index = 0;
        for (auto it = expected.begin(); it != expected.end(); ++it, ++index) {
            const auto &entry = outputs.at(it.key());
            keys(entry, {"file", "sha256"});
            auto bytes = verified(backend_dir, entry, budget);
            if (bytes.size() != reference_bytes.at(it.key()).size())
                throw std::runtime_error("backend output size mismatch");
            auto name = std::to_string(index) + ".bin";
            write_bytes(out / "observed" / name, bytes);
            auto metadata = it.value();
            metadata["file"] = name;
            metadata["sha256"] = sha256(bytes);
            observed["outputs"][it.key()] = metadata;
        }
        atomic_json(out / "observed/record.json", observed);
        result["phase"] = "compare";
        const auto comparison =
            verify_reference(out / "reference/record.json", out / "observed/record.json");
        atomic_json(out / "comparison.json", comparison);
        result["comparison"] = comparison;
        result["phase"] = "completed";
        return finish(comparison.at("status") == "match" ? "match" : "mismatch");
    } catch (const std::exception &error) {
        result["reason"] = error.what();
        return finish("execution_error");
    }
}
} // namespace sl
