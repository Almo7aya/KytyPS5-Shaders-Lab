// GPL-2.0-only: source-linked adapter to KytyPS5. Never executes guest code.
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/frontend/cfg/ShaderCFG.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"
#include "kytyGitVersion.h"
#include "shader_lab/compiler.hpp"
#include "shader_lab/compiler_trace.hpp"
#include "spirv-tools/libspirv.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
using namespace Libs::Graphics;
namespace sr = Libs::Graphics::ShaderRecompiler;
namespace {
struct PassTrace {
    bool enabled = false;
    int stop_after = -1;
    unsigned next = 0;
    bool active = false;
    sl::fs::path output;
    sl::json record;
} pass_trace;

struct Memory {
    struct Range {
        uint64_t base;
        std::vector<uint32_t> words;
    };
    std::vector<Range> ranges;
    sl::json reads = sl::json::array();
    bool missing = false;
};
bool read_memory(void *p, uint64_t address, std::span<uint32_t> words) {
    auto &m = *static_cast<Memory *>(p);
    bool found = false;
    for (const auto &r : m.ranges)
        if (address >= r.base && address - r.base <= r.words.size() * 4 &&
            words.size_bytes() <= r.words.size() * 4 - (address - r.base)) {
            std::memcpy(words.data(),
                        reinterpret_cast<const uint8_t *>(r.words.data()) + (address - r.base),
                        words.size_bytes());
            found = true;
            break;
        }
    if (m.reads.size() < 100000)
        m.reads.push_back({{"address", "0x" + sl::hex(address)},
                           {"bytes", words.size_bytes()},
                           {"available", found}});
    if (!found)
        m.missing = true;
    return found; // Never dereference a guest address or fabricate descriptor bytes.
}
uint32_t value(const sl::json &j, const char *key, uint32_t fallback) {
    if (!j.contains(key))
        return fallback;
    auto n = j.at(key).get<uint64_t>();
    if (n > UINT32_MAX)
        throw std::runtime_error(std::string(key) + " exceeds u32");
    return uint32_t(n);
}
void only_keys(const sl::json &j, const std::set<std::string> &keys) {
    if (!j.is_object())
        throw std::runtime_error("profile must be an object");
    for (auto it = j.begin(); it != j.end(); ++it)
        if (!keys.contains(it.key()))
            throw std::runtime_error("unknown profile field: " + it.key());
}
} // namespace
void sl::compiler_pass(unsigned index, bool completed, const sr::IR::Program *ir) {
    if (!pass_trace.enabled)
        return;
    auto &trace = pass_trace;
    if (index != trace.next || completed != trace.active)
        throw std::runtime_error("compiler pass catalog/order mismatch");
    const auto catalog = compiler_pass_catalog();
    if (index >= catalog.at("passes").size())
        throw std::runtime_error("unknown compiler pass checkpoint");
    const auto &pass = catalog.at("passes").at(index);
    trace.record["events"].push_back({{"index", index}, {"name", pass.at("name")},
                                     {"state", completed ? "completed" : "started"}});
    trace.record["last"] = trace.record["events"].back();
    atomic_json(trace.output / "pass-trace.json", trace.record);
    trace.active = !completed;
    if (!completed)
        return;
    ++trace.next;
    if (trace.stop_after == int(index)) {
        if (ir) {
            auto dump = sr::IR::ProgramToString(*ir);
            if (dump.size() <= 64 * 1024 * 1024) {
                write_text(trace.output / "pass-stop.ir", dump);
                trace.record["stop_ir_sha256"] = hash_file(trace.output / "pass-stop.ir");
            } else {
                trace.record["stop_ir_omission"] = "exceeds_64_mib";
            }
            atomic_json(trace.output / "pass-trace.json", trace.record);
        }
        throw CompilerCheckpointStop{index};
    }
}

sl::json sl::compiler_info_v1() {
    return {{"schema", 1}, {"compiler_interface", 1}, {"worker_protocol", 1},
            {"kyty_revision", KYTY_GIT_REVISION}, {"configured_checkout", SL_KYTY_REV},
            {"link_mode", "compiler_library"}, {"upstream_source_count", SL_COMPILER_SOURCE_COUNT},
            {"upstream_full_test_source_count", SL_FULL_TEST_SOURCE_COUNT},
            {"execution", "compiler_only"}, {"process_contract", "one_request_per_process"},
            {"pass_catalog", compiler_pass_catalog()}};
}

int sl::execute_compiler_request_v1(const sl::fs::path &request_path) {
    static std::atomic_flag started = ATOMIC_FLAG_INIT;
    if (started.test_and_set()) {
        std::cerr << "compiler interface v1 permits one request per process\n";
        return 2;
    }
    sl::fs::path out;
    sl::json result;
    std::string phase = "request";
    try {
        auto request = sl::read_json(request_path);
        out = sl::path_from(request.at("output").get<std::string>());
        if (request.value("schema", 0) != 1)
            throw std::runtime_error("unsupported worker protocol");
        result = {{"schema", 1},
                  {"id", request.at("id")},
                  {"kyty_revision", KYTY_GIT_REVISION},
                  {"configured_checkout", SL_KYTY_REV},
                  {"compiler", sl::compiler_info_v1()},
                  {"semantic_correctness", "not_tested"},
                  {"assumptions", sl::json::array()}};
        auto set_phase = [&](const char *p) {
            phase = p;
            sl::atomic_json(out / "phase.json", {{"phase", phase}});
            std::cout << "PHASE " << p << "\n" << std::flush;
        };
        auto finish = [&](const char *status) {
            result["status"] = status;
            result["last_phase"] = phase;
            sl::atomic_json(out / "response.json", result);
            return 0;
        };
        set_phase("input_validation");
        auto header = sl::read_bytes(sl::path_from(request.at("header").get<std::string>()));
        auto bytes = sl::read_bytes(sl::path_from(request.at("code").get<std::string>()));
        std::string why;
        if (!sl::valid_header(header, why) || bytes.size() != sl::integer(header, 0x44, 4)) {
            result["reason"] = why.empty() ? "code extent mismatch" : why;
            return finish("invalid_input");
        }
        const auto &profile = request.at("profile");
        only_keys(profile, {"schema", "mode", "host_subgroup_size", "wave_size", "stage",
                            "user_data", "memory", "compute", "pixel", "vertex", "shader_base",
                            "diagnostics"});
        if (profile.value("schema", 1) != 1)
            throw std::runtime_error("unsupported profile schema");
        auto mode = profile.value("mode", "header_probe");
        if (mode != "header_probe" && mode != "context_snapshot")
            throw std::runtime_error("mode must be header_probe or context_snapshot");
        if (profile.contains("diagnostics")) {
            const auto &diagnostics = profile.at("diagnostics");
            only_keys(diagnostics, {"pass_trace", "stop_after_pass"});
            pass_trace.enabled = diagnostics.value("pass_trace", false);
            if (diagnostics.contains("stop_after_pass")) {
                const auto &index = diagnostics.at("stop_after_pass");
                if (!pass_trace.enabled || !index.is_number_integer() ||
                    index.get<int64_t>() < 0 || index.get<uint64_t>() >= compiler_pass_catalog().at("passes").size())
                    throw std::runtime_error("stop_after_pass requires pass_trace and an index from the compiler pass catalog");
                pass_trace.stop_after = index.get<int>();
            }
            if (pass_trace.enabled) {
                pass_trace.output = out;
                pass_trace.record = {{"schema", 1}, {"catalog_schema", 1},
                    {"id", request.at("id")}, {"code_sha256", sl::sha256(bytes)},
                    {"configured_checkout", SL_KYTY_REV}, {"events", sl::json::array()},
                    {"semantic_correctness", "not_tested"}};
                sl::atomic_json(out / "pass-trace.json", pass_trace.record);
            }
        }
        result["context_mode"] = mode;
        if (mode == "context_snapshot" &&
            (!profile.contains("stage") || !profile.contains("user_data") ||
             !profile.contains("wave_size")))
            throw std::runtime_error(
                "context_snapshot requires explicit stage, wave_size and user_data; it is supplied "
                "context, not verified capture provenance");
        std::vector<uint32_t> code(bytes.size() / 4);
        std::memcpy(code.data(), bytes.data(), bytes.size());
        set_phase("initialize");
        Common::InitializeThreads();
        static Common::Subsystems subsystems;
        subsystems.Initialize<Config::Lifecycle>();
        Config::ConfigOptions config;
        config.printf_direction = Config::LogDirection::Console;
        Config::Load(config);
        subsystems.Initialize<Log::Lifecycle>();
        set_phase("decode");
        sr::Decoder::Program decoded;
        sr::Decoder::DecodeProgram(code, decoded);
        sl::write_text(out / "guest.asm", sr::Decoder::ProgramToString(decoded));
        sl::json instructions = sl::json::array(), histogram = sl::json::object(),
                 unsupported = sl::json::array();
        for (const auto &inst : decoded.instructions) {
            std::string op(magic_enum::enum_name(inst.opcode));
            histogram[op] = histogram.value(op, 0) + 1;
            sl::json row = {{"pc", inst.pc},
                            {"words", inst.word_count},
                            {"family", std::string(magic_enum::enum_name(inst.family))},
                            {"opcode", op},
                            {"opcode_id", inst.opcode_id},
                            {"text", sr::Decoder::InstructionToString(inst)}};
            if (sr::Decoder::IsDirectBranch(inst.opcode))
                row["branch_target"] = inst.branch_target;
            if (inst.opcode == sr::Decoder::Opcode::UNKNOWN ||
                inst.opcode == sr::Decoder::Opcode::UNSUPPORTED)
                unsupported.push_back(row);
            instructions.push_back(std::move(row));
        }
        sl::atomic_json(out / "instructions.json", {{"schema", 1},
                                                    {"code_sha256", sl::sha256(bytes)},
                                                    {"instructions", instructions},
                                                    {"histogram", histogram},
                                                    {"bvh_early_stop", decoded.has_bvh}});
        result["decoded_instruction_count"] = decoded.instructions.size();
        result["unsupported"] = unsupported;
        if (decoded.has_bvh)
            return finish("unsupported_ray_tracing");
        if (!unsupported.empty())
            return finish("unsupported_instruction");
        set_phase("cfg");
        auto graph = sr::CFG::BuildGraph(decoded);
        sl::write_text(out / "native-cfg.txt", sr::CFG::GraphToString(graph));
        std::string dot = "digraph shader {\n";
        sl::json blocks = sl::json::array();
        for (const auto &block : graph.blocks) {
            dot += "b" + std::to_string(block.id) + " [label=\"block " + std::to_string(block.id) +
                   " PC 0x" + sl::hex(block.start_pc) + "..0x" + sl::hex(block.end_pc) + "\"];\n";
            for (auto target : block.successors)
                dot += "b" + std::to_string(block.id) + " -> b" + std::to_string(target) + ";\n";
            blocks.push_back({{"id", block.id},
                              {"start_pc", block.start_pc},
                              {"end_pc", block.end_pc},
                              {"successors", block.successors},
                              {"predecessors", block.predecessors},
                              {"dominators", block.dominators}});
        }
        dot += "}\n";
        sl::write_text(out / "cfg.dot", dot);
        sl::atomic_json(out / "cfg.json", {{"blocks", blocks},
                                           {"unsupported", graph.unsupported},
                                           {"reason", graph.unsupported_reason}});
        auto type = header[0x5a];
        auto stage = profile.value("stage", type == 0 ? "CS" : type == 1 ? "PS" : "unknown");
        result["stage"] = stage;
        if (stage == "unknown") {
            result["reason"] = "fetch, fused, tessellation and NGG/mesh stages require explicit "
                               "stage/partner state; decoded only";
            return finish("missing_stage_context");
        }
        std::map<uint32_t, uint32_t> registers;
        for (auto [field, count] :
             {std::pair{0x18u, unsigned(header[0x5b])}, std::pair{0x20u, unsigned(header[0x5c])}}) {
            auto base = field + sl::integer(header, field, 8);
            for (unsigned i = 0; i < count; ++i)
                registers[uint32_t(sl::integer(header, base + 8 * i, 4))] =
                    uint32_t(sl::integer(header, base + 8 * i + 4, 4));
        }
        sl::json regs = sl::json::array();
        for (auto [r, v] : registers)
            regs.push_back({{"offset", r}, {"value", v}});
        sl::atomic_json(out / "header-registers.json", regs);
        auto reg = [&](uint32_t r, uint32_t fallback = 0) {
            auto it = registers.find(r);
            return it == registers.end() ? fallback : it->second;
        };
        sr::CompileOptions options;
        options.dump_ir = true;
        options.shader_hash = std::stoull(sl::xxh3(bytes), nullptr, 16);
        uint32_t dispatch = 0;
        auto sp = uint64_t(0x28) + sl::integer(header, 0x28, 8);
        if (sl::integer(header, 0x28, 8) && sl::contains(header, sp, 20))
            dispatch = uint32_t(sl::integer(header, sp + 16, 4));
        options.wave_size = value(profile, "wave_size",
                                  stage == "CS" ? ((dispatch & (1u << 15)) ? 32u : 64u) : 64u);
        if (options.wave_size != 32 && options.wave_size != 64)
            throw std::runtime_error("wave_size must be 32 or 64");
        std::vector<uint32_t> user_data = profile.value("user_data", std::vector<uint32_t>(32, 0));
        if (user_data.size() > 40)
            throw std::runtime_error("at most 40 user-data words");
        options.user_data = user_data;
        if (!profile.contains("user_data"))
            result["assumptions"].push_back(
                "user-data words are zero; no runtime descriptors captured");
        ShaderComputeInputInfo compute{};
        ShaderPixelInputInfo pixel{};
        ShaderVertexInputInfo vertex{};
        const auto empty = sl::json::object();
        if (stage == "CS") {
            options.stage = ShaderType::Compute;
            auto c = profile.value("compute", empty);
            only_keys(c, {"threads", "lds_size_dwords", "scratch_size_dwords", "group_id",
                          "thread_ids_num", "workgroup_register", "tg_size_en", "float_mode"});
            auto threads = c.value(
                "threads", std::vector<uint32_t>{reg(0x207, 1), reg(0x208, 1), reg(0x209, 1)});
            if (threads.size() != 3 || !threads[0] || !threads[1] || !threads[2] ||
                uint64_t(threads[0]) * threads[1] > 1024 ||
                uint64_t(threads[0]) * threads[1] * threads[2] > 1024)
                throw std::runtime_error("invalid compute workgroup");
            for (int i = 0; i < 3; ++i)
                compute.threads_num[i] = threads[i];
            auto r = reg(0x213);
            compute.wave_size = options.wave_size;
            compute.host_subgroup_size = value(profile, "host_subgroup_size", 32);
            compute.float_mode = uint8_t(value(c, "float_mode", (reg(0x212, 0xc0000) >> 12) & 255));
            compute.lds_size_dwords = value(c, "lds_size_dwords", ((r >> 15) & 511) * 128);
            compute.scratch_size_dwords =
                value(c, "scratch_size_dwords", uint32_t(sl::integer(header, 0x54, 2)));
            compute.thread_ids_num = int(value(c, "thread_ids_num", ((r >> 11) & 3) + 1));
            compute.workgroup_register = int(value(c, "workgroup_register", (r >> 1) & 31));
            compute.tg_size_en = c.value("tg_size_en", bool(r & (1u << 10)));
            auto groups =
                c.value("group_id", std::vector<bool>{bool(r & 128), bool(r & 256), bool(r & 512)});
            if (groups.size() != 3)
                throw std::runtime_error("group_id requires three booleans");
            for (int i = 0; i < 3; ++i)
                compute.group_id[i] = groups[i];
            options.input_info.compute = &compute;
            result["effective_compute"] = {{"threads", threads},
                                           {"wave_size", options.wave_size},
                                           {"host_subgroup_size", compute.host_subgroup_size},
                                           {"lds_size_dwords", compute.lds_size_dwords},
                                           {"thread_ids_num", compute.thread_ids_num},
                                           {"workgroup_register", compute.workgroup_register}};
            result["assumptions"].push_back("header-derived compiler input; no PM4 dispatch "
                                            "execution or PrepareProgram replay");
        } else if (stage == "PS") {
            options.stage = ShaderType::Pixel;
            auto p = profile.value("pixel", empty);
            only_keys(p, {"input_num", "ps_system_input_base", "target_output_mode",
                          "interpolator_settings", "ps_pos_x", "ps_pos_y", "ps_pos_z", "ps_pos_w",
                          "ps_front_face", "ps_ancillary", "ps_pixel_kill_enable",
                          "ps_depth_export_enable", "ps_early_z", "scratch_size_dwords"});
            pixel.wave_size = options.wave_size;
            pixel.input_num = value(p, "input_num", uint32_t(sl::integer(header, 0x50, 4)));
            if (pixel.input_num > 32)
                throw std::runtime_error("pixel inputs exceed 32");
            pixel.ps_system_input_base = value(p, "ps_system_input_base", 0);
            pixel.scratch_size_dwords =
                value(p, "scratch_size_dwords", uint32_t(sl::integer(header, 0x54, 2)));
            auto outputs = p.value("target_output_mode", std::vector<uint32_t>(8, 9));
            auto interpolation =
                p.value("interpolator_settings", std::vector<uint32_t>(pixel.input_num, 0));
            if (outputs.size() != 8 || interpolation.size() > 32)
                throw std::runtime_error("invalid pixel arrays");
            for (size_t i = 0; i < 8; ++i) {
                if (outputs[i] > 15)
                    throw std::runtime_error("invalid pixel output mode");
                pixel.target_output_mode[i] = uint8_t(outputs[i]);
            }
            for (size_t i = 0; i < interpolation.size(); ++i)
                pixel.interpolator_settings[i] = interpolation[i];
            pixel.ps_pos_x = p.value("ps_pos_x", false);
            pixel.ps_pos_y = p.value("ps_pos_y", false);
            pixel.ps_pos_z = p.value("ps_pos_z", false);
            pixel.ps_pos_w = p.value("ps_pos_w", false);
            pixel.ps_front_face = p.value("ps_front_face", false);
            pixel.ps_ancillary = p.value("ps_ancillary", false);
            pixel.ps_pixel_kill_enable = p.value("ps_pixel_kill_enable", false);
            pixel.ps_depth_export_enable = p.value("ps_depth_export_enable", false);
            pixel.ps_early_z = p.value("ps_early_z", false);
            options.input_info.pixel = &pixel;
            result["assumptions"].push_back("pixel probe defaults are not actual draw-state "
                                            "reconstruction; supply explicit profile fields");
        } else if (stage == "VS") {
            options.stage = ShaderType::Vertex;
            auto v = profile.value("vertex", empty);
            only_keys(v, {"user_data_base", "scratch_size_dwords", "pa_cl_vs_out_cntl"});
            vertex.wave_size = options.wave_size;
            vertex.scratch_size_dwords =
                value(v, "scratch_size_dwords", uint32_t(sl::integer(header, 0x54, 2)));
            vertex.pa_cl_vs_out_cntl = value(v, "pa_cl_vs_out_cntl", 0);
            options.user_data_base = value(v, "user_data_base", 8);
            options.input_info.vertex = &vertex;
            result["assumptions"].push_back("explicit simple vertex probe: no fetch tables, NGG, "
                                            "mesh or fused partner inferred");
        } else
            throw std::runtime_error("stage must be CS, PS or VS in this adapter");
        auto subgroup = value(profile, "host_subgroup_size", 32);
        if (subgroup != 32 && subgroup != 64)
            throw std::runtime_error("host_subgroup_size must be 32 or 64");
        set_phase("translate");
        sl::compiler_pass(0, false);
        sl::compiler_pass(0, true);
        auto translated = sr::TranslateProgram(code, options);
        if (translated.skip_dispatch)
            return finish("unsupported_ray_tracing");
        sl::write_text(out / "cfg.txt", translated.cfg_dump);
        sl::write_text(out / "translated.ir", sr::IR::ProgramToString(translated.program));
        result["ir_blocks"] = translated.program.blocks.size();
        result["dispatcher_fallback"] = translated.program.dispatcher_fallback;
        set_phase("materialize");
        sl::compiler_pass(12, false, &translated.program);
        auto plan = sr::IR::ExtractResourcePlan(translated.program);
        sl::compiler_pass(12, true, &translated.program);
        sr::IR::ResourceSnapshot snapshot;
        sr::IR::ResourceSpecialization specialization;
        Memory memory;
        memory.ranges.push_back({reinterpret_cast<uint64_t>(code.data()), code});
        if (profile.contains("shader_base")) {
            auto base = profile.at("shader_base").is_string()
                            ? std::stoull(profile.at("shader_base").get<std::string>(), nullptr, 0)
                            : profile.at("shader_base").get<uint64_t>();
            if (code.size() * 4 > UINT64_MAX - base)
                throw std::runtime_error("invalid shader base");
            memory.ranges.push_back({base, code});
        }
        if (profile.contains("memory")) {
            for (const auto &m : profile["memory"]) {
                auto base = m.at("base").is_string()
                                ? std::stoull(m.at("base").get<std::string>(), nullptr, 0)
                                : m.at("base").get<uint64_t>();
                auto words = m.at("words").get<std::vector<uint32_t>>();
                if (words.size() > 16 * 1024 * 1024 || words.size() * 4 > UINT64_MAX - base)
                    throw std::runtime_error("invalid memory snapshot extent");
                memory.ranges.push_back({base, std::move(words)});
            }
        }
        for (size_t i = 0; i < memory.ranges.size(); ++i)
            for (size_t j = i + 1; j < memory.ranges.size(); ++j) {
                auto &a = memory.ranges[i];
                auto &b = memory.ranges[j];
                if (a.base < b.base + b.words.size() * 4 && b.base < a.base + a.words.size() * 4)
                    throw std::runtime_error("overlapping memory snapshot ranges");
            }
        const sr::IR::SrtRuntime runtime{.user_data = options.user_data,
                                         .shader_base = reinterpret_cast<uint64_t>(code.data()),
                                         .read_memory = read_memory,
                                         .userdata = &memory,
                                         .read_specialization_memory = read_memory};
        sl::compiler_pass(13, false, &translated.program);
        bool materialized = sr::IR::MaterializeResources(plan, runtime, snapshot, specialization);
        sl::atomic_json(out / "memory-reads.json", memory.reads);
        if (!materialized) {
            result["missing_memory"] = memory.missing;
            return finish("resource_context_unresolved");
        }
        sl::compiler_pass(13, true, &translated.program);
        set_phase("emit");
        auto compiled = sr::CompileProgram(std::move(translated), options, specialization);
        sl::write_text(out / "final.ir", compiled.ir_dump);
        auto spv = sl::Bytes(reinterpret_cast<const uint8_t *>(compiled.spirv.data()),
                             compiled.spirv.size() * 4);
        sl::write_bytes(out / "shader.spv", spv);
        result["spirv_sha256"] = sl::sha256(spv);
        result["spirv_words"] = compiled.spirv.size();
        set_phase("validate");
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
        sl::json diagnostics = sl::json::array();
        tools.SetMessageConsumer([&](spv_message_level_t level, const char *,
                                     const spv_position_t &position, const char *message) {
            diagnostics.push_back({{"level", int(level)},
                                   {"index", position.index},
                                   {"message", message ? message : ""}});
        });
        bool valid = tools.Validate(compiled.spirv);
        std::string disassembly;
        tools.Disassemble(compiled.spirv, &disassembly);
        sl::write_text(out / "shader.spvasm", disassembly);
        result["validator_messages"] = diagnostics;
        result["validation_environment"] = "Vulkan 1.3";
        return finish(valid ? "spirv_valid_under_profile" : "spirv_invalid_under_profile");
    } catch (const CompilerCheckpointStop &stop) {
        result["status"] = "pass_checkpoint_reached";
        result["last_phase"] = phase;
        result["pass_checkpoint"] = compiler_pass_catalog().at("passes").at(stop.index);
        result["reason"] = "Diagnostic prefix stopped intentionally; no validation or semantic verdict";
        sl::atomic_json(out / "response.json", result);
        return 0;
    } catch (const std::exception &ex) {
        if (!out.empty() && result.contains("id")) {
            result["status"] = "adapter_error";
            result["last_phase"] = phase;
            result["reason"] = ex.what();
            sl::atomic_json(out / "response.json", result);
            return 0;
        }
        std::cerr << ex.what() << "\n";
        return 2;
    }
}
