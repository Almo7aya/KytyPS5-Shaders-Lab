#pragma once
#include "shader_lab/lab.hpp"

namespace sl {
// Versioned source interface; no Kyty headers or mutable IR escape this boundary.
// Request/response JSON uses worker protocol 1. This is not a stable binary ABI.
json compiler_info_v1();
// Call once, on the main thread of an isolated worker process. Upstream assertions
// may terminate that process. Never invoke in the scanner or an application's UI.
// Returns 0 when a response was written, including shader failures; 2 for setup errors.
int execute_compiler_request_v1(const fs::path &request_path);
} // namespace sl
