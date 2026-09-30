// GPL-2.0-only. Independently implemented integer model; no Kyty headers/code.
// Semantics: AMD RDNA2 ISA guide, sections 12.7/12.8 and global memory.
// Bitfields cross-checked with LLVM llvmorg-20.1.8 FLATInstructions.td/BUFInstructions.td.
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
    uint32_t soffset = 128;
    bool buffer = false, idxen = false, offen = false;
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
        } else if ((w & 0xffffc000u) == 0xe0300000u || (w & 0xffffc000u) == 0xe0700000u) {
            i.op = (w & 0x00400000u) ? Op::Store : Op::Load;
            i.buffer = true;
            i.idxen = (w & 0x2000u) != 0;
            i.offen = (w & 0x1000u) != 0;
            i.offset = int32_t(w & 0xfffu);
            const auto hi = next();
            i.a = hi & 255;
            i.b = ((hi >> 16) & 31) * 4;
            i.dst = (hi >> 8) & 255;
            i.soffset = hi >> 24;
            if ((hi & 0x00e00000u) || i.b > 100 || (i.idxen && i.offen && i.a == 255) ||
                !(i.soffset <= 103 || (i.soffset >= 128 && i.soffset <= 208)))
                unsupported("unmodeled buffer modifiers or resource register quartet at byte " +
                            std::to_string(i.pc));
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
    uint64_t loads = 0, stores = 0, buffer_loads = 0, buffer_stores = 0, executed = 0;
    auto memory = [&](uint64_t address, uint64_t thread, bool store, uint32_t value, bool buffer) {
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
                if (buffer)
                    ++buffer_stores;
                else
                    ++stores;
                return value;
            }
            if (buffer)
                ++buffer_loads;
            else
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
                        if (i.buffer) {
                            const auto lo = read(scalar[i.b]), hi = read(scalar[i.b + 1]);
                            const auto records = read(scalar[i.b + 2]),
                                       flags = read(scalar[i.b + 3]);
                            // Linear dword descriptors only: no swizzle/cache-swizzle/AddTid,
                            // type 0, identity selectors, R32_UINT format, OOB_SELECT=2.
                            if ((hi & 0xc0000000u) || flags != 0x20014facu)
                                unsupported(
                                    "unmodeled buffer descriptor format, flags or OOB mode");
                            const uint64_t base = uint64_t(lo) | (uint64_t(hi & 0xffffu) << 32);
                            const uint64_t stride = (hi >> 16) & 0x3fffu;
                            const uint64_t extent = stride ? stride * records : records;
                            const uint64_t index = i.idxen ? read(vector[lane][i.a]) : 0;
                            const uint64_t offset =
                                i.offen ? read(vector[lane][i.a + unsigned(i.idxen)]) : 0;
                            const uint64_t relative =
                                index * stride + offset + source(i.soffset) + uint32_t(i.offset);
                            // Reject wrap/OOB instead of inventing hardware zero/drop semantics.
                            if (relative > UINT32_MAX || extent < 4 || relative > extent - 4 ||
                                relative > 0xffffffffffffull - base)
                                unsupported("buffer access wraps or falls outside the modeled "
                                            "descriptor range");
                            address = base + relative;
                        } else if (i.b == 125)
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
                        if (!i.buffer && i.offset < 0) {
                            if (address < uint32_t(-i.offset))
                                unsupported("global address underflow");
                            address -= uint32_t(-i.offset);
                        } else if (!i.buffer) {
                            if (uint32_t(i.offset) > UINT64_MAX - address)
                                unsupported("global address overflow");
                            address += uint32_t(i.offset);
                        }
                        const uint64_t thread = group * group_threads + start + lane;
                        if (i.op == Op::Store)
                            memory(address, thread, true, read(vector[lane][i.dst]), i.buffer);
                        else {
                            if (vector[lane][i.dst].pending)
                                unsupported("overlapping pending load destinations");
                            vector[lane][i.dst] = {memory(address, thread, false, 0, i.buffer),
                                                   true, true};
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
                    {"buffer_loads", buffer_loads},
                    {"buffer_stores", buffer_stores},
                    {"host_gpu_used", false},
                    {"hardware_conformance", "not_established"}};
    return result;
}

int cpu_generate_fixtures(const fs::path &request_path) {
    const auto request_file = fs::canonical(request_path);
    const auto request_bytes = read_bytes(request_file, 1024 * 1024);
    const auto request = json::parse(request_bytes.begin(), request_bytes.end());
    const auto root = request_file.parent_path();
    json response = {{"schema", 1},
                     {"kind", "shader_lab_generated_fixtures"},
                     {"request_sha256", sha256(request_bytes)},
                     {"status", "unsupported"},
                     {"generator", "rdna2-synthetic/1"},
                     {"model", "rdna2-integer/1"},
                     {"semantic_correctness", "not_proven"},
                     {"tests", json::array()}};
    try {
        keys(request, {"schema", "kind", "header", "code", "header_sha256", "code_sha256"});
        if (request.at("schema") != 1 || request.at("kind") != "shader_lab_generate_request")
            unsupported("invalid generation request");
        auto header =
            read_bytes(path_from(request.at("header").get<std::string>()), 32 * 1024 * 1024);
        auto code = read_bytes(path_from(request.at("code").get<std::string>()), 16384);
        std::string why;
        if (!valid_header(header, why) || header.at(0x5a) != 0 ||
            integer(header, 0x44, 4) != code.size())
            unsupported("generation needs an intact compute shader/header pair: " + why);
        if (sha256(header) != request.at("header_sha256").get<std::string>() ||
            sha256(code) != request.at("code_sha256").get<std::string>())
            unsupported("generation input hash mismatch");
        const auto instructions = decode(code);
        std::set<uint32_t> descriptors;
        bool writes = false;
        for (const auto &instruction : instructions) {
            if (instruction.op != Op::Load && instruction.op != Op::Store)
                continue;
            if (!instruction.buffer || instruction.b > 12)
                unsupported("automatic fixtures support only direct buffer descriptors in s[0:15]; "
                            "global/BDA memory needs explicit captured state");
            descriptors.insert(instruction.b);
            writes |= instruction.op == Op::Store;
        }
        if (!writes)
            unsupported("no observable buffer stores; register-only results are not validated by "
                        "this generator");
        const auto fixture_root = root / "fixtures";
        if (!fs::create_directory(fixture_root))
            throw std::runtime_error("fixture generation requires fresh output");
        unsigned number = 0;
        for (const unsigned wave : {32u, 64u})
            for (unsigned pattern = 0; pattern < 3; ++pattern) {
                const auto name = std::to_string(number++);
                const auto dir = fixture_root / name;
                json test = {{"variant", name},
                             {"wave_size", wave},
                             {"pattern", pattern},
                             {"status", "unsupported"}};
                try {
                    fs::create_directory(dir);
                    ExecutionInputs inputs;
                    inputs.header = header;
                    inputs.code = code;
                    inputs.execution = {
                        {"stage", "CS"},
                        {"wave_size", wave},
                        {"exec_mask", wave == 32 ? "00000000ffffffff" : "ffffffffffffffff"},
                        {"workgroup_size", {wave, 1, 1}},
                        {"dispatch_size", {1, 1, 1}}};
                    std::vector<uint32_t> users(16, pattern == 0 ? 0u : pattern == 1 ? 1u : 3u);
                    json resources = json::object();
                    unsigned binding = 0;
                    for (auto reg : descriptors) {
                        const auto resource = "buffer" + std::to_string(reg / 4);
                        const uint32_t base = 0x1000u + reg / 4 * 0x10000u;
                        users[reg] = base;
                        users[reg + 1] = 0x00040000u; // 48-bit base, stride four.
                        users[reg + 2] = 1024;
                        users[reg + 3] =
                            0x20014facu; // Linear R32_UINT, identity selectors, OOB_SELECT=2.
                        auto &bytes = inputs.resource_bytes[resource];
                        bytes.resize(4096);
                        uint32_t random = 0x9e3779b9u ^ reg;
                        constexpr uint32_t edges[] = {0,           1,           0xffffffffu,
                                                      0x7fffffffu, 0x80000000u, 0xfffffffeu,
                                                      0x55555555u, 0xaaaaaaaau};
                        for (unsigned i = 0; i < 1024; ++i) {
                            random ^= random << 13;
                            random ^= random >> 17;
                            random ^= random << 5;
                            const auto value = pattern == 0   ? 0u
                                               : pattern == 1 ? edges[i % 8]
                                                              : random;
                            for (unsigned b = 0; b < 4; ++b)
                                bytes[i * 4 + b] = uint8_t(value >> (8 * b));
                        }
                        const auto file = resource + ".bin";
                        write_bytes(dir / file, bytes);
                        resources[resource] = {{"file", file},
                                               {"sha256", sha256(bytes)},
                                               {"kind", "buffer"},
                                               {"type", "u32"},
                                               {"access", "read_write"},
                                               {"binding", {0, binding++}},
                                               {"guest_address", hex(base, 16)}};
                    }
                    inputs.resources = resources;
                    inputs.profile = {{"schema", 1},
                                      {"mode", "context_snapshot"},
                                      {"stage", "CS"},
                                      {"wave_size", wave},
                                      {"host_subgroup_size", 32},
                                      {"user_data", users},
                                      {"compute",
                                       {{"threads", {wave, 1, 1}},
                                        {"group_id", {false, false, false}},
                                        {"thread_ids_num", 1},
                                        {"workgroup_register", 16},
                                        {"tg_size_en", false},
                                        {"lds_size_dwords", 0},
                                        {"scratch_size_dwords", 0},
                                        {"float_mode", 192}}}};
                    write_bytes(dir / "header.bin", header);
                    write_bytes(dir / "code.bin", code);
                    atomic_json(dir / "profile.json", inputs.profile);
                    auto file = [&](const char *filename) {
                        return json{{"file", filename}, {"sha256", hash_file(dir / filename)}};
                    };
                    atomic_json(
                        dir / "fixture.json",
                        {{"schema", 1},
                         {"kind", "shader_lab_execution_fixture"},
                         {"id", "generated-wave" + std::to_string(wave) + "-pattern" +
                                    std::to_string(pattern)},
                         {"shader", {{"header", file("header.bin")}, {"code", file("code.bin")}}},
                         {"profile", file("profile.json")},
                         {"execution", inputs.execution},
                         {"resources", resources}});
                    inputs.identity = execution_fixture_identity(dir / "fixture.json");
                    auto modeled = cpu_reference(std::move(inputs));
                    if (modeled.trace.at("buffer_stores").get<uint64_t>() == 0)
                        unsupported("synthetic state produced no observable stores");
                    json outputs = json::object();
                    for (const auto &[resource, bytes] : modeled.outputs) {
                        const auto expected = "expected-" + resource + ".bin";
                        write_bytes(dir / expected, bytes);
                        outputs[resource] = {{"file", expected},
                                             {"sha256", sha256(bytes)},
                                             {"kind", "buffer"},
                                             {"type", "u32"},
                                             {"comparison", {{"mode", "exact"}}}};
                    }
                    atomic_json(dir / "reference.json",
                                {{"schema", 1},
                                 {"fixture", modeled.trace.at("fixture")},
                                 {"reference_source",
                                  {{"kind", "independent_model"},
                                   {"identifier", "rdna2-integer/1"},
                                   {"method", "Independent CPU ISA execution over generated "
                                              "synthetic inputs; no Kyty output used. Model not "
                                              "hardware-certified; not captured game state."}}},
                                 {"outputs", outputs}});
                    atomic_json(dir / "model-trace.json", modeled.trace);
                    test["status"] = "generated";
                    test["fixture"] = path_text((dir / "fixture.json").lexically_relative(root));
                    test["reference"] =
                        path_text((dir / "reference.json").lexically_relative(root));
                    test["fixture_identity"] = modeled.trace.at("fixture");
                } catch (const std::exception &error) {
                    test["reason"] = std::string(error.what()).substr(0, 4096);
                }
                response["tests"].push_back(std::move(test));
            }
        response["status"] = "assessed";
        response["assumptions"] = "Synthetic wave32/wave64, one full-EXEC workgroup, direct linear "
                                  "4 KiB buffers, zero/edge/seeded inputs, integer-only, no "
                                  "LDS/scratch. Not inferred game state or a correctness proof.";
    } catch (const std::exception &error) {
        response["reason"] = std::string(error.what()).substr(0, 4096);
    }
    atomic_json(root / "generation.json", response);
    return 0;
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
