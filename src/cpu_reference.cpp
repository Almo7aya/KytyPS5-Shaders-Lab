// GPL-2.0-only. Independently implemented integer model; no Kyty headers/code.
// Semantics: AMD RDNA2 ISA guide, sections 12.7/12.8 and global memory.
// Bitfields cross-checked with LLVM llvmorg-20.1.8 FLATInstructions.td and MC tests.
#include "shader_lab/cpu_reference.hpp"
#include "shader_lab/host_profile.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <iostream>
#include <set>

namespace sl {
namespace {
[[noreturn]] void unsupported(const std::string &reason) {
    throw std::runtime_error("rdna2-integer/1: " + reason);
}
uint32_t word(const json &j) {
    if (!j.is_number_integer() || (!j.is_number_unsigned() && j.get<int64_t>() < 0) ||
        j.get<uint64_t>() > UINT32_MAX)
        unsupported("expected unsigned 32-bit integer");
    return j.get<uint32_t>();
}
void keys(const json &j, const std::set<std::string> &allowed) {
    if (!j.is_object())
        unsupported("expected object");
    for (auto it = j.begin(); it != j.end(); ++it)
        if (!allowed.contains(it.key()))
            unsupported("unmodeled profile field " + it.key());
}
enum class Op { Nop, Wait, End, Mov, Integer, Load, Store };
struct Inst {
    Op op;
    uint32_t pc, a = 0, b = 0, dst = 0, subop = 0, literal = 0;
    int32_t offset = 0;
};
std::vector<Inst> decode(Bytes code) {
    if (code.empty() || code.size() % 4 || code.size() > 16384)
        unsupported("code must contain 1..4096 dwords");
    std::vector<Inst> result;
    size_t pc = 0;
    auto next = [&]() {
        if (!contains(code, pc, 4))
            unsupported("truncated instruction at byte " + std::to_string(pc));
        auto value = uint32_t(integer(code, pc, 4));
        pc += 4;
        return value;
    };
    while (pc < code.size()) {
        Inst i{Op::Nop, uint32_t(pc)};
        const auto w = next();
        if (w == 0xbf810000u) {
            if (pc != code.size())
                unsupported("trailing code after S_ENDPGM");
            i.op = Op::End;
        } else if (w == 0xbf8c0000u) {
            i.op = Op::Wait; // S_WAITCNT with all counters zero.
        } else if ((w & 0xffff0000u) == 0xbf800000u) {
            i.op = Op::Nop;
        } else if ((w & 0xfffff000u) == 0xdc308000u || (w & 0xfffff000u) == 0xdc708000u) {
            i.op = (w & 0x00400000u) ? Op::Store : Op::Load;
            i.offset = int32_t(w & 0xfff);
            if (i.offset & 0x800)
                i.offset -= 4096;
            const auto hi = next();
            i.a = hi & 255;         // vector address
            i.b = (hi >> 16) & 127; // scalar base, or NULL (125) for vector-pair address
            i.dst = i.op == Op::Store ? (hi >> 8) & 255 : hi >> 24;
            if ((hi & 0x00800000u) || (i.op == Op::Store ? (hi >> 24) != 0 : (hi & 0xff00) != 0) ||
                (i.b == 125 ? i.a == 255 : (i.b > 102 || (i.b & 1))))
                unsupported("global-memory modifiers or invalid register pair at byte " +
                            std::to_string(i.pc));
        } else if ((w & 0xfe000000u) == 0x7e000000u) {
            if (((w >> 9) & 255) != 1)
                unsupported("unmodeled VOP1 at byte " + std::to_string(i.pc));
            i.op = Op::Mov;
            i.a = w & 511;
            i.dst = (w >> 17) & 255;
            if (i.a == 255)
                i.literal = next();
        } else if (!(w >> 31)) {
            i.op = Op::Integer;
            i.subop = (w >> 25) & 63;
            if (!std::set<uint32_t>{17, 18, 19, 20, 22, 24, 26, 27, 28, 29, 30, 37, 38, 39}
                     .contains(i.subop))
                unsupported("unmodeled VOP2 at byte " + std::to_string(i.pc));
            i.a = w & 511;
            i.b = (w >> 9) & 255;
            i.dst = (w >> 17) & 255;
            if (i.a == 255)
                i.literal = next();
        } else {
            unsupported("unmodeled encoding 0x" + hex(w, 8) + " at byte " + std::to_string(i.pc));
        }
        if ((i.op == Op::Mov || i.op == Op::Integer) &&
            !(i.a <= 103 || (i.a >= 128 && i.a <= 208) || i.a >= 255))
            unsupported("special register, DPP/SDWA or float inline source at byte " +
                        std::to_string(i.pc));
        result.push_back(i);
    }
    if (result.empty() || result.back().op != Op::End)
        unsupported("missing S_ENDPGM");
    return result;
}
uint32_t alu(uint32_t op, uint32_t a, uint32_t b) {
    switch (op) {
    case 17:
        return std::bit_cast<int32_t>(a) < std::bit_cast<int32_t>(b) ? a : b;
    case 18:
        return std::bit_cast<int32_t>(a) > std::bit_cast<int32_t>(b) ? a : b;
    case 19:
        return std::min(a, b);
    case 20:
        return std::max(a, b);
    case 22:
        return b >> (a & 31);
    case 24:
        return std::bit_cast<uint32_t>(std::bit_cast<int32_t>(b) >> (a & 31));
    case 26:
        return b << (a & 31);
    case 27:
        return a & b;
    case 28:
        return a | b;
    case 29:
        return a ^ b;
    case 30:
        return ~(a ^ b);
    case 37:
        return a + b;
    case 38:
        return a - b;
    case 39:
        return b - a;
    default:
        unsupported("internal ALU dispatch mismatch");
    }
}
struct Register {
    uint32_t value = 0;
    bool defined = false, pending = false;
};
struct Access {
    uint64_t thread;
    bool multiple_readers = false, write = false;
};
} // namespace

CpuReferenceResult cpu_reference(ExecutionInputs inputs) {
    const auto instructions = decode(inputs.code);
    const auto &profile = inputs.profile, &execution = inputs.execution;
    keys(profile, {"schema", "mode", "stage", "wave_size", "user_data", "compute", "host",
                   "host_subgroup_size"});
    // Target-compiler host declarations do not change guest ISA reference semantics.
    // Validate their schema/consistency, but never use them for lane execution.
    (void)compiler_host_subgroup(profile);
    if (word(profile.at("schema")) != 1 || profile.at("mode") != "context_snapshot" ||
        profile.at("stage") != "CS" ||
        word(profile.at("wave_size")) != word(execution.at("wave_size")))
        unsupported("requires explicit matching compute context_snapshot");
    const auto &compute = profile.at("compute");
    keys(compute, {"threads", "group_id", "thread_ids_num", "workgroup_register", "tg_size_en",
                   "lds_size_dwords", "scratch_size_dwords", "float_mode"});
    if (compute.at("threads") != execution.at("workgroup_size") ||
        !compute.at("tg_size_en").is_boolean() || compute.at("tg_size_en").get<bool>() ||
        word(compute.at("lds_size_dwords")) || word(compute.at("scratch_size_dwords")) ||
        word(compute.at("float_mode")) > 255)
        unsupported("workgroup mismatch or unmodeled LDS/scratch/TG-size state");
    const auto thread_ids = word(compute.at("thread_ids_num"));
    if (thread_ids < 1 || thread_ids > 3)
        unsupported("thread_ids_num must be 1..3");
    const auto &users = profile.at("user_data"), &groups = compute.at("group_id");
    if (!users.is_array() || users.size() > 16 || !groups.is_array() || groups.size() != 3 ||
        word(compute.at("workgroup_register")) != users.size())
        unsupported("explicit direct user SGPRs and contiguous workgroup registers required");
    for (const auto &enabled : groups)
        if (!enabled.is_boolean())
            unsupported("group_id requires booleans");
    std::array<uint32_t, 3> threads{}, dispatch{};
    uint64_t group_threads = 1, group_count = 1;
    for (size_t d = 0; d < 3; ++d) {
        threads[d] = word(execution.at("workgroup_size").at(d));
        if (word(compute.at("threads").at(d)) != threads[d])
            unsupported("compute thread extent mismatch");
        dispatch[d] = word(execution.at("dispatch_size").at(d));
        if (!threads[d] || !dispatch[d] || threads[d] > 1024 || dispatch[d] > 16777216)
            unsupported("invalid execution dimensions");
        group_threads *= threads[d];
        group_count *= dispatch[d];
        if (group_threads > 1024 || group_count > 16777216)
            unsupported("execution dimensions too large");
    }
    const auto wave = word(execution.at("wave_size"));
    const auto mask_text = execution.at("exec_mask").get<std::string>();
    if ((wave != 32 && wave != 64) || mask_text.size() != 16 ||
        mask_text.find_first_not_of("0123456789abcdef") != mask_text.npos)
        unsupported("invalid wave or EXEC mask");
    const auto initial_exec = std::stoull(mask_text, nullptr, 16);
    if (wave == 32 && initial_exec >> 32)
        unsupported("wave32 EXEC overflow");
    if (group_count > 16777216 / group_threads ||
        instructions.size() > 16777216 / (group_count * group_threads))
        unsupported("model budget is 16777216 lane-instructions");
    for (auto it = inputs.resources.begin(); it != inputs.resources.end(); ++it) {
        const auto &r = it.value();
        if (r.at("kind") != "buffer" || (r.at("type") != "u32" && r.at("type") != "i32"))
            unsupported("only integer dword buffers are modeled");
        const auto base = std::stoull(r.at("guest_address").get<std::string>(), nullptr, 16);
        if ((base & 3) || inputs.resource_bytes.at(it.key()).size() % 4)
            unsupported("dword resources must be aligned");
    }
    std::map<uint64_t, Access> accesses;
    uint64_t loads = 0, stores = 0, executed = 0;
    auto memory = [&](uint64_t address, uint64_t thread, bool store, uint32_t value) {
        if (address & 3)
            unsupported("unaligned global access");
        auto [it, inserted] = accesses.emplace(address, Access{thread, false, store});
        if (!inserted) {
            auto &a = it->second;
            if (a.write)
                unsupported("store-to-load feedback and repeated stores need a memory-order model");
            if ((a.thread != thread && (a.write || store)) || (a.multiple_readers && store))
                unsupported("cross-lane/workgroup memory conflict; no ordering oracle");
            a.multiple_readers |= a.thread != thread;
            a.write |= store;
        }
        for (auto r = inputs.resources.begin(); r != inputs.resources.end(); ++r) {
            const auto base =
                std::stoull(r.value().at("guest_address").get<std::string>(), nullptr, 16);
            auto &bytes = inputs.resource_bytes.at(r.key());
            if (address < base || address - base > bytes.size() ||
                bytes.size() - (address - base) < 4)
                continue;
            const auto access = r.value().at("access").get<std::string>();
            if ((store && access == "read_only") || (!store && access == "write_only"))
                unsupported("global access violates declared resource access");
            const auto offset = size_t(address - base);
            if (store) {
                for (unsigned b = 0; b < 4; ++b)
                    bytes[offset + b] = uint8_t(value >> (b * 8));
                ++stores;
                return value;
            }
            ++loads;
            return uint32_t(integer(bytes, offset, 4));
        }
        unsupported("global access is not wholly backed by a declared resource");
    };
    for (uint64_t group = 0; group < group_count; ++group) {
        std::array<Register, 104> scalar{};
        for (size_t u = 0; u < users.size(); ++u)
            scalar[u] = {word(users[u]), true, false};
        size_t sgpr = users.size();
        const uint32_t group_ids[] = {uint32_t(group % dispatch[0]),
                                      uint32_t(group / dispatch[0] % dispatch[1]),
                                      uint32_t(group / dispatch[0] / dispatch[1])};
        for (size_t d = 0; d < 3; ++d)
            if (groups[d].get<bool>())
                scalar[sgpr++] = {group_ids[d], true, false};
        for (uint64_t start = 0; start < group_threads; start += wave) {
            std::array<std::array<Register, 256>, 64> vector{};
            for (uint32_t lane = 0; lane < wave && start + lane < group_threads; ++lane) {
                const uint64_t id = start + lane;
                const uint32_t ids[] = {uint32_t(id % threads[0]),
                                        uint32_t(id / threads[0] % threads[1]),
                                        uint32_t(id / threads[0] / threads[1])};
                for (uint32_t d = 0; d < thread_ids; ++d)
                    vector[lane][d] = {ids[d], true, false};
            }
            for (const auto &i : instructions) {
                if (i.op == Op::End)
                    break;
                if (i.op == Op::Wait) {
                    for (auto &lane : vector)
                        for (auto &reg : lane)
                            reg.pending = false;
                    continue;
                }
                if (i.op == Op::Nop)
                    continue;
                for (uint32_t lane = 0; lane < wave && start + lane < group_threads; ++lane) {
                    if (!(initial_exec & (uint64_t(1) << lane)))
                        continue;
                    ++executed;
                    auto read = [&](const Register &r) {
                        if (!r.defined || r.pending)
                            unsupported("undefined or pending register at byte " +
                                        std::to_string(i.pc));
                        return r.value;
                    };
                    auto source = [&](uint32_t n) -> uint32_t {
                        if (n <= 103)
                            return read(scalar[n]);
                        if (n >= 128 && n <= 192)
                            return n - 128;
                        if (n >= 193 && n <= 208)
                            return 0u - (n - 192);
                        if (n == 255)
                            return i.literal;
                        if (n >= 256 && n <= 511)
                            return read(vector[lane][n - 256]);
                        unsupported("unmodeled operand");
                    };
                    if (i.op == Op::Mov || i.op == Op::Integer) {
                        if (vector[lane][i.dst].pending)
                            unsupported("overwriting a pending load destination");
                        const auto a = source(i.a);
                        const auto result =
                            i.op == Op::Mov ? a : alu(i.subop, a, read(vector[lane][i.b]));
                        vector[lane][i.dst] = {result, true, false};
                    } else {
                        uint64_t address = 0;
                        if (i.b == 125)
                            address = uint64_t(read(vector[lane][i.a])) |
                                      (uint64_t(read(vector[lane][i.a + 1])) << 32);
                        else {
                            const auto base = uint64_t(read(scalar[i.b])) |
                                              (uint64_t(read(scalar[i.b + 1])) << 32);
                            const auto offset = read(vector[lane][i.a]);
                            if (offset > UINT64_MAX - base)
                                unsupported("global address overflow");
                            address = base + offset;
                        }
                        if (i.offset < 0) {
                            if (address < uint32_t(-i.offset))
                                unsupported("global address underflow");
                            address -= uint32_t(-i.offset);
                        } else {
                            if (uint32_t(i.offset) > UINT64_MAX - address)
                                unsupported("global address overflow");
                            address += uint32_t(i.offset);
                        }
                        const uint64_t thread = group * group_threads + start + lane;
                        if (i.op == Op::Store)
                            memory(address, thread, true, read(vector[lane][i.dst]));
                        else {
                            if (vector[lane][i.dst].pending)
                                unsupported("overlapping pending load destinations");
                            vector[lane][i.dst] = {memory(address, thread, false, 0), true, true};
                        }
                    }
                }
            }
        }
    }
    CpuReferenceResult result;
    for (auto it = inputs.resources.begin(); it != inputs.resources.end(); ++it)
        if (it.value().at("access") != "read_only")
            result.outputs[it.key()] = std::move(inputs.resource_bytes.at(it.key()));
    result.trace = {{"schema", 1},
                    {"model", "rdna2-integer/1"},
                    {"fixture", inputs.identity},
                    {"decoded_instructions", instructions.size()},
                    {"executed_lane_instructions", executed},
                    {"global_loads", loads},
                    {"global_stores", stores},
                    {"host_gpu_used", false},
                    {"hardware_conformance", "not_established"}};
    return result;
}

int cpu_reference_worker(const fs::path &request_path) {
    const auto request_file = fs::canonical(request_path);
    const auto bytes = read_bytes(request_file, 1024 * 1024);
    const auto request = json::parse(bytes.begin(), bytes.end());
    const auto out = fs::canonical(path_from(request.at("output").get<std::string>()));
    if (out != request_file.parent_path() / "backend")
        throw std::runtime_error("CPU backend output must be the attempt's backend directory");
    json response = {
        {"schema", 1},
        {"kind", "shader_lab_execution_response"},
        {"request_sha256", sha256(bytes)},
        {"status", "unsupported"},
        {"backend",
         {{"kind", "cpu"},
          {"identifier", "rdna2-integer/1"},
          {"method",
           "Independent integer/global-buffer ISA model; no Kyty code or GPU execution"}}}};
    try {
        if (request.at("backend_kind") != "cpu")
            unsupported("this backend only implements CPU execution");
        auto result = cpu_reference(read_execution_request(request_file));
        json outputs = json::object();
        unsigned index = 0;
        for (const auto &[name, data] : result.outputs) {
            auto file = std::to_string(index++) + ".bin";
            write_bytes(out / file, data);
            outputs[name] = {{"file", file}, {"sha256", sha256(data)}};
        }
        atomic_json(out / "model-trace.json", result.trace);
        response["outputs"] = outputs;
        response["status"] = "completed";
    } catch (const std::exception &error) {
        response["reason"] = std::string(error.what()).substr(0, 4096);
    }
    atomic_json(out / "response.json", response);
    return 0;
}
} // namespace sl
