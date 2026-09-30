#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <vector>
namespace sl {
namespace fs = std::filesystem;
using json = nlohmann::ordered_json;
using Bytes = std::span<const uint8_t>;
inline constexpr int schema_version = 1;
std::string path_text(const fs::path &p);
fs::path path_from(std::string_view p);
uint64_t integer(Bytes bytes, uint64_t offset, unsigned width);
bool contains(Bytes bytes, uint64_t offset, uint64_t size);
std::vector<uint8_t> read_bytes(const fs::path &path, uint64_t limit = 64 * 1024 * 1024);
void write_bytes(const fs::path &path, Bytes bytes);
void write_text(const fs::path &path, std::string_view text);
void atomic_json(const fs::path &path, const json &value);
json read_json(const fs::path &path);
std::string sha256(Bytes bytes);
std::string hash_file(const fs::path &path);
std::string xxh3(Bytes bytes);
std::string hex(uint64_t value, unsigned width = 0);
std::string html_escape(std::string_view value);
bool is_within(const fs::path &path, const fs::path &root);
class MappedFile {
  public:
    explicit MappedFile(const fs::path &path);
    ~MappedFile();
    MappedFile(const MappedFile &) = delete;
    MappedFile &operator=(const MappedFile &) = delete;
    Bytes bytes() const {
        return {data_, size_};
    }

  private:
    const uint8_t *data_ = nullptr;
    size_t size_ = 0;
#ifdef _WIN32
    void *file_ = nullptr;
    void *mapping_ = nullptr;
#else
    int fd_ = -1;
#endif
};
struct Candidate {
    uint64_t header_offset = 0, code_offset = 0;
    std::vector<uint8_t> header, code;
    std::string method;
    json evidence = json::array();
    std::string offset_space = "file";
};
struct Extraction {
    std::vector<Candidate> candidates;
    json findings = json::array();
    bool limited = false;
};
bool valid_header(Bytes bytes, std::string &reason);
Extraction extract(Bytes bytes, size_t candidate_limit = 100000);
Extraction extract_containers(Bytes bytes, size_t candidate_limit = 100000);
struct ScanOptions {
    fs::path input, output;
    uint64_t max_files = 0;
    uint64_t max_file_bytes = 0;
    size_t max_candidates = 100000;
    bool resume = true;
};
json scan(const ScanOptions &options);
struct ProcessResult {
    uint32_t exit_code = 0;
    bool timed_out = false;
    uint64_t elapsed_ms = 0;
    fs::path working_directory;
};
ProcessResult process(const fs::path &executable, const std::vector<std::string> &args,
                      const fs::path &working_directory, const fs::path &log,
                      std::chrono::milliseconds timeout);
struct RunOptions {
    fs::path dataset, output, worker, profile;
    unsigned jobs = 1;
    uint64_t timeout_ms = 30000, limit = 0;
    bool resume = true;
};
json run(const RunOptions &options);
// A campaign runs the same corpus under multiple explicit, named contexts.
// Worker isolation and per-case cache semantics are inherited from run().
json campaign(const RunOptions &options, const fs::path &plan);
json verify_reference(const fs::path &reference, const fs::path &observed);
struct ExecuteOptions {
    fs::path fixture, reference, worker, output;
    uint64_t timeout_ms = 30000;
    std::string backend_kind = "cpu";
    bool allow_gpu = false;
};
// Validates and hashes actual fixture files. Does not execute a backend.
json execution_fixture_identity(const fs::path &fixture);
// Executes only the explicitly selected trusted backend, then compares reference outputs.
json execute_fixture(const ExecuteOptions &options);
struct ExecutionInputs {
    json identity, execution, resources, profile;
    std::vector<uint8_t> header, code;
    std::map<std::string, std::vector<uint8_t>> resource_bytes;
};
// Backend-side validation uses the same fixture identity and actual snapshot bytes.
ExecutionInputs read_execution_request(const fs::path &request);
json export_repro(const fs::path &dataset, const fs::path &results,
                  const std::string &id, const fs::path &output);
json replay_repro(const fs::path &bundle, const fs::path &worker,
                  const fs::path &output, uint64_t timeout_ms = 0);
struct MinimizeOptions {
    fs::path bundle, worker, output;
    uint64_t timeout_ms = 0, max_attempts = 128;
    unsigned confirmations = 2;
};
json failure_signature(const json &result);
json minimize_repro(const MinimizeOptions &options);
struct PassBisectOptions {
    fs::path bundle, worker, output;
    uint64_t timeout_ms = 0;
    unsigned confirmations = 2;
};
json bisect_passes(const PassBisectOptions &options);
// Bounded register/packet replay only. Never executes dispatches or guest memory writes.
json captured_compute_state(Bytes header, const json &capture, uint64_t shader_address);
// Normalize explicitly supplied pixel compiler inputs; not PM4/draw-state reconstruction.
json normalize_pixel_profile(Bytes header, const json &pixel, uint32_t wave_size);
json compare(const fs::path &before, const fs::path &after);
json correlate(const fs::path &dataset, const fs::path &log);
json inspect(const fs::path &dataset, std::string hash);
json cluster(const fs::path &results);
void report(const fs::path &dataset, const fs::path &results, const fs::path &output);
} // namespace sl
