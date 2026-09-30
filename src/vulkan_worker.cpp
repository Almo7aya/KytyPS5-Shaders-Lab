#include "shader_lab/compiler.hpp"
#include "shader_lab/vulkan_replay.hpp"
#include <iostream>
int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "shader-vulkan-replay --execute-fixture REQUEST_JSON\n"
                         "Opt-in isolated Kyty/Vulkan buffer-compute replay. Not a GPU sandbox.\n";
            return 0;
        }
        if (argc != 3 || std::string(argv[1]) != "--execute-fixture")
            throw std::runtime_error("expected --execute-fixture REQUEST_JSON");
        const auto file = sl::fs::canonical(sl::path_from(argv[2]));
        const auto bytes = sl::read_bytes(file, 1024 * 1024);
        const auto request = sl::json::parse(bytes.begin(), bytes.end());
        const auto out = sl::fs::canonical(sl::path_from(request.at("output").get<std::string>()));
        if (out != file.parent_path() / "backend")
            throw std::runtime_error(
                "Vulkan backend output must be the attempt's backend directory");
        sl::json response = {
            {"schema", 1},
            {"kind", "shader_lab_execution_response"},
            {"request_sha256", sl::sha256(bytes)},
            {"status", "unsupported"},
            {"backend",
             {{"kind", "gpu"},
              {"identifier", "kyty-vulkan-buffer/1"},
              {"method",
               "Real Kyty compilation and Vulkan compute dispatch with resource readback"}}}};
        try {
            // No loader, device, or compiler work before the execution contract is validated.
            if (request.at("backend_kind") != "gpu" || !request.at("allow_gpu").is_boolean() ||
                !request.at("allow_gpu").get<bool>())
                throw std::runtime_error(
                    "Vulkan replay requires GPU backend and explicit allow_gpu");
            const auto inputs = sl::read_execution_request(file);
            const auto compilation = out / "compilation";
            sl::fs::create_directories(compilation);
            sl::atomic_json(out / "compiler-request.json",
                            {{"schema", 1},
                             {"id", inputs.identity.at("id")},
                             {"header", request.at("header")},
                             {"code", request.at("code")},
                             {"profile", inputs.profile},
                             {"output", sl::path_text(compilation)}});
            if (sl::execute_compiler_request_v1(out / "compiler-request.json") != 0)
                throw std::runtime_error("compiler request failed");
            const auto compiled = sl::read_json(compilation / "response.json");
            if (compiled.at("status") != "spirv_valid_under_profile" ||
                !compiled.contains("compiler_layout") ||
                compiled.at("compiler_layout").at("status") != "exported")
                throw std::runtime_error(
                    "replay requires validated SPIR-V and an exported compiler layout: " +
                    compiled.at("status").get<std::string>());
            const auto layout_bytes = sl::read_bytes(compilation / "compiler-layout.json");
            if (sl::sha256(layout_bytes) !=
                compiled.at("compiler_layout").at("sha256").get<std::string>())
                throw std::runtime_error("compiler layout hash mismatch");
            const auto layout = sl::json::parse(layout_bytes.begin(), layout_bytes.end());
            auto replay =
                sl::vulkan_replay(inputs, sl::read_bytes(compilation / "shader.spv"), layout);
            sl::json outputs = sl::json::object();
            unsigned index = 0;
            for (const auto &[name, data] : replay.outputs) {
                auto name_on_disk = std::to_string(index++) + ".bin";
                sl::write_bytes(out / name_on_disk, data);
                outputs[name] = {{"file", name_on_disk}, {"sha256", sl::sha256(data)}};
            }
            sl::atomic_json(out / "replay-trace.json", replay.trace);
            response["outputs"] = outputs;
            response["status"] = "completed";
        } catch (const sl::VulkanReplayFailure &error) {
            std::cerr << error.what() << '\n';
            return 2; // Driver/runtime failure is not an unsupported-case or comparison verdict.
        } catch (const std::exception &error) {
            response["reason"] = std::string(error.what()).substr(0, 4096);
        }
        sl::atomic_json(out / "response.json", response);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
