#include "shader_lab/lab.hpp"

using namespace sl;
namespace {
json fixture_capture() {
    json registers = json::array();
    for (auto [offset, value] : {std::pair{0x207u, 4u},
                                 {0x208u, 2u},
                                 {0x209u, 1u},
                                 {0x20cu, 0x10u},
                                 {0x20du, 0u},
                                 {0x212u, 0xc0000u},
                                 {0x213u, 0x8984u},
                                 {0x228u, 0u},
                                 {0x240u, 11u},
                                 {0x241u, 22u}})
        registers.push_back({{"offset", offset}, {"value", value}});
    return {
        {"schema", 1},
        {"provenance",
         {{"source", "synthetic C++ fixture"},
          {"method", "Explicit register writes and one direct dispatch; no captured game data"}}},
        {"use_header_registers", true},
        {"initial_sh_registers", registers},
        {"pm4", {0xc0017602u, 0x241u, 33u, 0xc0031502u, 2u, 3u, 4u, 0x8041u}}};
}
} // namespace

unsigned capture_tests(Bytes header) {
    unsigned checks = 0;
    auto test = [&](bool ok, const char *name) {
        if (!ok)
            throw std::runtime_error(name);
        ++checks;
    };
    auto reject = [&](json capture, const char *name, uint64_t address = 0x1000) {
        bool rejected = false;
        try {
            captured_compute_state(header, capture, address);
        } catch (const std::exception &) {
            rejected = true;
        }
        test(rejected, name);
    };
    const auto capture = fixture_capture();
    const auto state = captured_compute_state(header, capture, 0x1000);
    test(state.at("register_writes").back().at("value") == 33 && state.at("packets").size() == 2,
         "captured register overwrite and packet provenance retained");
    test(state.at("final_sh_registers").back() == json({{"offset", 0x241}, {"value", 33}}),
         "PM4 final register state follows write order");
    test(state.at("dispatch").at("dimensions") == json({2, 3, 4}) &&
             state.at("dispatch").at("initiator") == 0x8041,
         "dispatch state retains guest dimensions and mode");
    std::vector<uint8_t> header_with_register(header.begin(), header.end());
    header_with_register.resize(0x68);
    header_with_register[0x40] = 0x68;
    header_with_register[0x20] = 0x40;
    header_with_register[0x5c] = 1;
    header_with_register[0x60] = 7;
    header_with_register[0x61] = 2;
    header_with_register[0x64] = 8;
    auto header_state = captured_compute_state(header_with_register, capture, 0x1000);
    test(header_state.at("register_writes")[0].at("source") == "header_sh" &&
             header_state.at("register_writes")[0].at("value") == 8 &&
             header_state.at("final_sh_registers")[0].at("value") == 4,
         "captured initial state overrides header defaults in recorded order");
    for (size_t count = 0; count < capture.at("pm4").size(); ++count) {
        auto truncated = capture;
        truncated["pm4"] = json::array();
        for (size_t i = 0; i < count; ++i)
            truncated["pm4"].push_back(capture.at("pm4")[i]);
        reject(truncated, "every incomplete PM4 prefix rejected");
    }
    auto bad = capture;
    bad["pm4"][0] = 0xc0017603u;
    reject(bad, "predicated packet must not be replayed unconditionally");
    bad = capture;
    bad["pm4"][0] = 0xc0017604u;
    reject(bad, "custom packet sub-op cannot be ignored");
    bad = capture;
    bad["pm4"][0] = 0xc0013700u;
    reject(bad, "guest memory writes cannot masquerade as register writes");
    bad = capture;
    bad["pm4"][1] = 0x10000240u;
    reject(bad, "indexed register addressing explicitly unsupported");
    bad = capture;
    bad["pm4"][2] = -1;
    reject(bad, "negative capture words rejected");
    bad = capture;
    bad["pm4"][2] = 3.5;
    reject(bad, "floating-point capture words rejected");
    bad = capture;
    bad["pm4"][7] = 0x18041u;
    reject(bad, "unknown dispatch mode bits rejected");
    bad = capture;
    bad["pm4"][4] = 0;
    reject(bad, "zero-work capture not treated as an executed shader");
    bad = capture;
    bad["pm4"].push_back(0xc0031502u);
    reject(bad, "capture must end at the selected dispatch");
    bad = capture;
    bad["initial_sh_registers"].erase(8);
    reject(bad, "missing required user SGPR cannot be defaulted to zero");
    bad = capture;
    bad["initial_sh_registers"].erase(7);
    reject(bad, "missing resource register is explicit incomplete state");
    bad = capture;
    bad["initial_sh_registers"].push_back(bad["initial_sh_registers"][0]);
    reject(bad, "ambiguous initial snapshot rejected");
    bad = capture;
    bad["initial_sh_registers"][0]["value"] = 1025;
    reject(bad, "oversized workgroup rejected");
    bad = capture;
    bad["initial_sh_registers"][6]["value"] = 34;
    reject(bad, "unsupported compute user-data extent rejected");
    bad = capture;
    bad["provenance"]["source"] = "";
    reject(bad, "capture provenance is mandatory");
    reject(capture, "wrong program address rejected", 0x2000);
    reject(capture, "unaligned program address rejected", 0x1001);
    reject(capture, "oversized guest address rejected", 0x1000000001000ull);
    return checks;
}

unsigned capture_real_tests(const fs::path &root, Bytes input_header, const fs::path &worker) {
    unsigned checks = 0;
    auto test = [&](bool ok, const char *name) {
        if (!ok)
            throw std::runtime_error(name);
        ++checks;
    };
    write_bytes(root / "input/shader.header", input_header);
    const std::vector<uint8_t> end = {0, 0, 0x81, 0xbf};
    write_bytes(root / "input/shader.code", end);
    auto manifest = scan({root / "input", root / "dataset"});
    const auto id = manifest.at("shaders").begin().key();
    json profile = {{"schema", 1},
                    {"mode", "captured_compute"},
                    {"stage", "CS"},
                    {"shader_base", "0x1000"},
                    {"host_subgroup_size", 32},
                    {"capture", fixture_capture()}};
    auto compile = [&](const json &p, const char *name) {
        atomic_json(root / name / "profile.json", p);
        return run({root / "dataset", root / name / "run", worker, root / name / "profile.json", 1,
                    10000, 0, false});
    };
    auto captured = compile(profile, "captured");
    const auto &row = captured.at("results").at(id);
    test(row.at("status") == "spirv_valid_under_profile",
         "captured state compiles through real upstream preparation");
    const auto &compute = row.at("details").at("effective_compute");
    test(compute.at("preparation") == "upstream_PrepareProgram" &&
             compute.at("threads") == json({4, 2, 1}) && compute.at("wave_size") == 32 &&
             compute.at("lds_size_dwords") == 128 && compute.at("workgroup_register") == 2 &&
             compute.at("thread_ids_num") == 2,
         "upstream metadata reflects the captured register decoder, not probe defaults");
    auto state =
        read_json(root / "captured/run" / path_from(row.at("artifacts").get<std::string>()) /
                  "captured-state.json");
    test(state.at("prepared_user_data") == json({11, 33}),
         "upstream preparation receives latest PM4 user-data words");
    auto thread_dimensions = profile;
    thread_dimensions["capture"]["pm4"][7] = 0x8061;
    auto threads = compile(thread_dimensions, "thread-dimensions");
    test(
        threads.at("results").at(id).at("details").at("effective_compute").at("dispatch_threads") ==
            json({2, 3, 4}),
        "captured dispatch-thread dimensions survive upstream preparation");
    auto missing = profile;
    missing["capture"]["initial_sh_registers"].erase(8);
    test(compile(missing, "missing-user").at("results").at(id).at("status") == "adapter_error",
         "worker rejects incomplete captured user data before calling the compiler");
    auto conflicting = profile;
    conflicting["wave_size"] = 64;
    test(compile(conflicting, "conflicting-wave").at("results").at(id).at("status") ==
             "adapter_error",
         "probe overrides cannot silently replace captured state");
    return checks;
}
