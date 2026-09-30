#include "shader_lab/lab.hpp"
#include <map>
#include <set>

namespace sl {
namespace {
void keys(const json &object, const std::set<std::string> &allowed) {
    if (!object.is_object())
        throw std::runtime_error("capture requires JSON objects");
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!allowed.contains(it.key()))
            throw std::runtime_error("unknown capture field: " + it.key());
}
uint32_t u32(const json &value) {
    if (!value.is_number_integer() ||
        (value.is_number_integer() && !value.is_number_unsigned() && value.get<int64_t>() < 0) ||
        value.get<uint64_t>() > UINT32_MAX)
        throw std::runtime_error("capture words must be unsigned 32-bit integers");
    return value.get<uint32_t>();
}
bool supported_register(uint32_t reg) {
    return (reg >= 0x204 && reg <= 0x209) || reg == 0x20c || reg == 0x20d || reg == 0x212 ||
           reg == 0x213 || reg == 0x215 || reg == 0x218 || reg == 0x228 || reg == 0x22a ||
           (reg >= 0x240 && reg <= 0x24f);
}
} // namespace

json captured_compute_state(Bytes header, const json &capture, uint64_t shader_address) {
    std::string reason;
    if (!valid_header(header, reason) || header[0x5a] != 0)
        throw std::runtime_error("captured compute replay requires a valid compute AGC header");
    if (shader_address == 0 || (shader_address & 0xffff0000000000ffull) != 0)
        throw std::runtime_error(
            "capture shader address must be nonzero, 256-byte aligned and 48-bit");
    keys(capture, {"schema", "provenance", "use_header_registers", "initial_sh_registers", "pm4"});
    if (u32(capture.at("schema")) != 1)
        throw std::runtime_error("unsupported compute capture schema");
    const auto &provenance = capture.at("provenance");
    keys(provenance, {"source", "method"});
    for (const char *key : {"source", "method"})
        if (!provenance.at(key).is_string() || provenance.at(key).get<std::string>().empty() ||
            provenance.at(key).get<std::string>().size() > 4096)
            throw std::runtime_error("capture needs bounded source and method descriptions");
    std::map<uint32_t, uint32_t> registers;
    json writes = json::array(), packets = json::array();
    auto write = [&](uint32_t offset, uint32_t value, const char *source, uint64_t position) {
        if (!supported_register(offset))
            throw std::runtime_error("unsupported compute SH register 0x" + hex(offset));
        registers[offset] = value;
        writes.push_back(
            {{"offset", offset}, {"value", value}, {"source", source}, {"position", position}});
    };
    if (capture.at("use_header_registers").get<bool>()) {
        auto base = 0x20 + integer(header, 0x20, 8);
        for (unsigned n = 0; n < header[0x5c]; ++n)
            write(uint32_t(integer(header, base + n * 8, 4)),
                  uint32_t(integer(header, base + n * 8 + 4, 4)), "header_sh", n);
    }
    const auto &initial = capture.at("initial_sh_registers");
    if (!initial.is_array() || initial.size() > 256)
        throw std::runtime_error("initial SH snapshot must have at most 256 entries");
    std::set<uint32_t> initial_offsets;
    for (size_t n = 0; n < initial.size(); ++n) {
        keys(initial[n], {"offset", "value"});
        auto offset = u32(initial[n].at("offset"));
        if (!initial_offsets.insert(offset).second)
            throw std::runtime_error("duplicate register in initial SH snapshot");
        write(offset, u32(initial[n].at("value")), "initial_sh", n);
    }
    const auto &stream = capture.at("pm4");
    if (!stream.is_array() || stream.empty() || stream.size() > 1024 * 1024)
        throw std::runtime_error("PM4 capture must contain 1..1048576 dwords");
    std::vector<uint32_t> words;
    words.reserve(stream.size());
    for (const auto &word : stream)
        words.push_back(u32(word));
    json dispatch;
    for (size_t pc = 0; pc < words.size();) {
        uint32_t packet = words[pc], opcode = (packet >> 8) & 255;
        size_t length = ((packet >> 16) & 0x3fff) + 2;
        // Type-3 only. Predicate and Kyty custom sub-op bits cannot be silently ignored.
        // The ordinary compute shader-type bit (bit 1) does not change SH addressing.
        if ((packet >> 30) != 3 || (packet & 0xfdu) != 0 || length > words.size() - pc)
            throw std::runtime_error("unsupported or truncated PM4 packet at dword " +
                                     std::to_string(pc));
        packets.push_back({{"dword", pc}, {"opcode", opcode}, {"length", length}});
        if (opcode == 0x76) { // IT_SET_SH_REG
            if (length < 3 || words[pc + 1] > 0x2ff || length - 2 > 0x300 - words[pc + 1])
                throw std::runtime_error("invalid PM4 SET_SH_REG extent/index encoding");
            for (size_t i = 2; i < length; ++i)
                write(words[pc + 1] + uint32_t(i - 2), words[pc + i], "pm4", pc + i);
        } else if (opcode == 0x15) { // IT_DISPATCH_DIRECT
            if (length != 5 || pc + length != words.size())
                throw std::runtime_error("capture must end at exactly one direct dispatch");
            uint32_t initiator = words[pc + 4];
            if ((initiator & ~0xa079u) != 0 || (initiator & 1u) == 0)
                throw std::runtime_error("unsupported or disabled dispatch initiator");
            for (size_t i = 1; i <= 3; ++i)
                if (!words[pc + i])
                    throw std::runtime_error("capture dispatch has a zero dimension");
            dispatch = {{"dimensions", {words[pc + 1], words[pc + 2], words[pc + 3]}},
                        {"initiator", initiator}};
        } else {
            throw std::runtime_error("unsupported PM4 opcode 0x" + hex(opcode) +
                                     "; indirect buffers, memory writes and synchronization need "
                                     "explicit replay support");
        }
        pc += length;
    }
    if (dispatch.is_null())
        throw std::runtime_error("capture has no final direct dispatch");
    for (uint32_t offset : {0x207u, 0x208u, 0x209u, 0x20cu, 0x20du, 0x212u, 0x213u, 0x228u})
        if (!registers.contains(offset))
            throw std::runtime_error("capture missing required SH register 0x" + hex(offset));
    if (registers.at(0x20d) > 255 || ((uint64_t(registers.at(0x20d)) << 40) |
                                      (uint64_t(registers.at(0x20c)) << 8)) != shader_address)
        throw std::runtime_error("captured program address does not match shader_base");
    const auto user_count = (registers.at(0x213) >> 1) & 31;
    if (user_count > 16)
        throw std::runtime_error(
            "captured compute user SGPR count exceeds supported direct-register range");
    for (uint32_t i = 0; i < user_count; ++i)
        if (!registers.contains(0x240 + i))
            throw std::runtime_error(
                "capture is missing a required user SGPR; zero is not inferred");
    uint64_t invocations = 1;
    for (uint32_t offset : {0x207u, 0x208u, 0x209u}) {
        auto n = registers.at(offset);
        if (!n || n > 1024 || invocations * n > 1024)
            throw std::runtime_error("unsupported captured compute workgroup extent");
        invocations *= n;
    }
    json final = json::array();
    for (const auto &[offset, value] : registers)
        final.push_back({{"offset", offset}, {"value", value}});
    return {{"schema", 1},
            {"adapter", "compute_pm4/1"},
            {"provenance", provenance},
            {"provenance_trust", "user_supplied_not_authenticated"},
            {"register_writes", writes},
            {"packets", packets},
            {"final_sh_registers", final},
            {"dispatch", dispatch},
            {"shader_address", "0x" + hex(shader_address)},
            {"header_context_registers_used", false},
            {"execution", "state_replay_only"}};
}
} // namespace sl
