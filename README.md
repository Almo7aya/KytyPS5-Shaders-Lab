# PS5 Shader Lab

A C++20 offline shader extraction, reverse-engineering and KytyPS5 compiler-regression tool.
It recursively scans game files, retains extraction provenance, and runs individual shader cases
through an independently built worker linked to **your selected KytyPS5 source checkout**.
It never starts a game or executes guest CPU code. The compiler worker does not create a Vulkan device.

This is a usable first implementation, **not a universal game unpacker or proof of shader correctness**.
Valid SPIR-V is a structural result under recorded inputs. It does not prove equivalent pixels,
memory writes, numerical behavior, synchronization, or performance. Every result explicitly says
`semantic_correctness: not_tested`.

See [validation results](docs/VALIDATION.md) for the tested corpora and exact outcome
counts, and [architecture](docs/ARCHITECTURE.md) for the data and worker contracts.

## What is implemented

- Recursive, read-only scanning of every regular file, independent of extension; symlinks are not followed.
- Embedded AMDGPU ELF64 shaders with bounded section/name/header parsing.
- Bare AGC headers paired to code using trailer-checksum evidence; ambiguous matches are reported,
  not guessed. The 256-byte alignment heuristic is retained and disclosed.
- Clear SELF load-segment reconstruction, with original-file, ELF-file and virtual-address mappings.
  Candidate bytes must be backed by actual segments, not zero-filled holes.
- Independently framed Zstandard payload discovery when built with the Kyty worker dependencies.
  Declared-size, decompression-budget and candidate limits protect against oversized inputs.
- Existing `.header` / `.code` pairs from the supplied batch tool, without requiring Python.
- SHA-256 identity for **both header and code**, plus Kyty-compatible XXH3-64 code hashes.
  Different headers are never collapsed just because their code hashes match. All origins are retained.
- Content-verified incremental scanning; changed extractor sources invalidate cached parsing.
- One compiler subprocess per case; parallel jobs, deadlines, 2 GiB process memory limit,
  per-case logs, phase checkpoints, crash isolation and resumable results.
- Real Kyty decoding, native CFGs, translation/IR, resource materialization, SPIR-V generation
  and SPIRV-Tools validation for Vulkan 1.3. No second imitation compiler.
- Header-derived compute probes, explicitly approximate pixel probes, explicit simple vertex
  profiles, optional supplied user-data and bounded memory snapshots.
- Guest disassembly, instruction inventory, opcode histogram, CFG text/JSON/DOT, intermediate/final IR,
  memory-read trace, SPIR-V binary/disassembly and validator diagnostics.
- Searchable offline HTML reports; failure grouping; run-to-run outcome/SPIR-V comparisons;
  hash-to-source lookup; correlation with `hash=0x...` observations from existing emulator logs.

## Important coverage limits

“All regular files visited” is not “all shaders recovered.” Encryption, game-specific archive
indexes, Oodle/Kraken, ZIP/7z/PSARC/Unreal container decoding, compressed SELF segments, unknown-size
or dictionary-dependent Zstandard frames, nested compressed layers, cross-file bare-header pairing,
patched/runtime-generated code and dynamically loaded shader libraries can leave shaders undiscovered.
Container warnings and rejected/unpaired candidates remain in the manifest and report. Use authorized,
already-unpacked data where necessary. No decryption keys, DRM bypass or game redistribution is needed.

The worker currently does **not** reproduce `AgcCreateShader`, complete PM4 state, `PrepareProgram`,
fetch-table construction, shader fusion or full NGG/mesh/tessellation setup. Such stages are decoded
but normally report `missing_stage_context`. It is not yet a feature-for-feature replacement for
every stage supported by the older batch binary. Its stronger parts are traceability, corpus identity,
clear SELF handling, failure isolation and direct linkage to the selected current compiler.

Pixel probe defaults and zero user-data are assumptions, not captured game state. A failure under
those assumptions is a **reproduction candidate**, not automatically an emulator bug. Unavailable
memory reads fail materialization instead of being silently replaced with fabricated descriptors.
`context_snapshot` means user-supplied context, not independently authenticated reference execution.

## Build

Windows x64 is tested. Use CMake 3.24+, Ninja, Clang, installed MSVC/Windows SDK libraries, and
the prerequisites of the selected KytyPS5 checkout. The source-linked build inherits upstream
dependency downloads/build generators (including its Python-based SPIR-V build tools), but all
application, extraction, orchestration and test code in this repository is C++; no Python runtime
or extraction script is used by the application.

From this project directory:

```powershell
cmake -S . -B build-kyty -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DKYTY_ROOT="Z:/projects/PS5/src/leaning-something/KytyPS5" -DSHADER_LAB_BUILD_KYTY_WORKER=ON
cmake --build build-kyty --target shader-lab shader-kyty-worker shader-lab-tests --parallel 8
ctest --test-dir build-kyty -R '^shader_lab_' --output-on-failure
```

Outputs: `build-kyty/shader-lab.exe`, `build-kyty/shader-kyty-worker.exe` and its required
`libwinpthread-1.dll`. Keep the DLL beside the worker. The scanner itself does not need that DLL.
The first worker build is sizable: it reuses the upstream standalone-test link closure, which
includes renderer/library dependencies, even though this entry point never initializes a GPU.
Subsequent compiler edits rebuild incrementally. The selected checkout's source files are not edited.

For a lightweight extraction/reporting-only build:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_C_COMPILER=clang-cl -DSHADER_LAB_DEPS="Z:/projects/PS5/src/leaning-something/KytyPS5/3rdparty"
cmake --build build --parallel 8
ctest --test-dir build --output-on-failure
```

Without a local dependency directory, CMake fetches pinned nlohmann/json and xxHash releases.
The lightweight build does not include Zstandard decoding. Linux process/mapping implementations
are supplied but not yet build/runtime-verified. Do not claim cross-platform validation from the
Windows results.

The adapter was built against clean KytyPS5 revision
`2650478d92c394c092b36ee129962060c623ca16`. A changed upstream API may require adapter maintenance;
compilation should fail visibly instead of silently switching to an older compiler.

## Scan your games and compile the corpus

```powershell
.\build-kyty\shader-lab.exe scan --input "D:\PS5_Games" --output datasets/all-games
.\build-kyty\shader-lab.exe run --dataset datasets/all-games --worker build-kyty/shader-kyty-worker.exe --output runs/current --jobs 4 --timeout-ms 30000
.\build-kyty\shader-lab.exe report --dataset datasets/all-games --results runs/current/results.json --output runs/current/report.html
.\build-kyty\shader-lab.exe cluster --results runs/current/results.json --output runs/current/failures.json
```

Output directories must not overlap game inputs. Game files are never modified. You may scan a
single game, a single executable, or an existing batch-tool shader directory. A dataset represents
the input selection of its latest scan: use a separate output for partial scans rather than replacing
an all-games manifest with one title. Removed inputs disappear from the new manifest, while old object
files remain available; the tool does not garbage-collect or delete your corpus.

`scan --max-files N` and `--max-file-mb N` provide explicit partial-scan limits. `run --limit N`
selects a bounded compiler sample. They are not coverage estimates. SHA-256 resume re-reads input
bytes to establish freshness; it avoids parsing/compilation, not all disk I/O.

Runs cache by header+code identity, worker executable SHA-256, selected profile content, timeout and
protocol version. Changing the compiler executable/profile/deadline reruns affected cases.
`--no-resume` forces a repeat. Do not rebuild/replace a worker while that worker's run is active.
Treat its dependency DLL as part of the installation; it is not included in the cache key yet.

Directory locks reject concurrent writers. After a hard crash, confirm no process still uses the
output before manually removing only its `.scan-lock` or `.run-lock` directory. Per-case results are
written atomically; rerunning skips completed cases and retries incomplete ones. Timeout/crash results
are cached too; use a changed timeout or `--no-resume` to retry them.

## Investigate and compare

```powershell
.\build-kyty\shader-lab.exe inspect --dataset datasets/all-games --hash 9000dc4bca87a4a2 --output runs/shader-origin.json
.\build-kyty\shader-lab.exe correlate --dataset datasets/all-games --log "D:\captured-kyty.log" --output runs/runtime-coverage.json
.\build-kyty\shader-lab.exe compare --before runs/before/results.json --after runs/after/results.json --output runs/compiler-diff.json
```

Log correlation identifies observed hashes missing from extraction and offline cases not observed
in that log. Neither category proves dead code or complete runtime coverage. First-line and bounded
example evidence are retained; the tool does not attach to games or start tracing automatically.
CFG/DOT and IR are static compiler analyses, while `memory-reads.json` records CPU-side descriptor
materialization reads, **not** per-lane GPU execution. Failure clusters are heuristic triage groups,
not proven shared root causes. Changed SPIR-V bytes can be benign; compare semantics separately.

To investigate a result, use its `artifacts` path in `results.json`. Start with `phase.json`,
`worker.log`, `instructions.json`, `cfg.json`, `translated.ir` and `memory-reads.json`. Then find
the case ID in the manifest for its original file, offsets, offset coordinate system and evidence.
Offsets in a reconstructed ELF or decompressed frame are deliberately not labeled as physical
offsets in the original game file.

## Profiles and real context

`--profile profiles/probe-wave32.json` and `probe-wave64.json` exercise different wave/subgroup
assumptions in separate runs. Overriding the guest wave size is an experiment, not automatic detection.
Use the actual draw/dispatch state for an evidence-backed reproduction.

`profiles/compute-context.example.json` documents the accepted shape; its addresses and values are
**fictional**. Replace them with authorized captures. Memory ranges contain `base` (integer or
`0x...` string) and `words` (u32 values). Reads must be wholly backed; overlapping snapshots are
rejected. An optional `shader_base` adds the captured shader-code address as a read-only alias.
No guest address is dereferenced directly by the materialization callback.

For different metadata per shader, supply a profile bundle:

```json
{
  "default": {"schema": 1, "mode": "header_probe", "host_subgroup_size": 32},
  "cases": {
    "HEADER_SHA256-CODE_SHA256": {
      "schema": 1,
      "mode": "context_snapshot",
      "stage": "CS",
      "wave_size": 64,
      "user_data": [0, 0, 0, 0],
      "compute": {"threads": [64, 1, 1]}
    }
  }
}
```

Replace the placeholder key with an actual full case ID. Unknown IDs/fields are rejected.
Profiles select one context per case per run. Use separate runs for multiple contexts or host
feature assumptions. The worker's checked-in profile parser is the authoritative supported field list.

## Outcomes

| Status | What it establishes |
| --- | --- |
| `spirv_valid_under_profile` | Emission succeeded and SPIRV-Tools accepted the module under this profile. |
| `spirv_invalid_under_profile` | Structural validation failed; inspect the profile and validator message. |
| `unsupported_instruction` | Current decoder reported unknown/unsupported instruction semantics. |
| `unsupported_ray_tracing` | Current BVH skip path was detected; this is not successful RT compilation. |
| `missing_stage_context` | Stage/partner/fetch/NGG state cannot be responsibly inferred by this adapter. |
| `resource_context_unresolved` | Materialization could not resolve supplied/assumed descriptor state. |
| `worker_crash_or_error`, `timeout` | Isolated worker failed; inspect last phase and log, not just exit code. |
| `adapter_error`, `invalid_input` | Request/profile or extracted input could not be used. |
| `runner_error`, `worker_protocol_error` | Orchestration/artifact problem rather than a shader verdict. |

CLI exit codes: `0` means the command completed (individual shader failures remain in results),
`2` is a command/setup error, `3` is an explicitly limited scan or traversal error. Inspect all
manifest file statuses for read failures/coverage gaps even after exit 0. No status means “100% correct.”
Use result JSON rather than a successful process exit as your regression gate.

## Next development milestones

1. Captured PM4/header-to-state replay through upstream preparation, full supported graphics-stage
   metadata, fused partner identity and host feature profiles.
2. Additional versioned archive adapters, compressed SELF blocks and nested payload budgets;
   preserve archive-member provenance and report unsupported encryption explicitly.
3. A reference-execution harness: known input resources, guest wave/EXEC state, output buffers/images,
   exact integer checks and specified floating-point rules, with trusted independent reference data.
4. GPU replay tests for a supported compute subset, then graphics fixtures and differential debugging.
   GPU work must be isolated and explicitly enabled; compiler validation cannot substitute for it.
5. Minimized reproductions, pass-level bisection, multi-context campaigns and a stable smaller
   upstream compiler-library interface to reduce build/link dependencies.

Generated SPIR-V here is a research artifact, not an importable Kyty runtime pipeline cache.
It speeds compiler iteration by avoiding full game boot/reproduction; it does not automatically
prewarm game pipelines or prove that all runtime specializations have been compiled.

## Project boundaries and references

This directory is an independent Git repository. Its parent learning repository locally excludes it.
Move the whole directory (including hidden `.git`) later; reconfigure into a **fresh build directory**
with the new `KYTY_ROOT` rather than reusing absolute-path CMake caches. Dataset origins are relative
to their recorded root; compiled case artifacts are local and ignored by Git. Review licensing and
remove private captures before sharing anything. No remote is configured automatically.

Inspired by [ps5rs](https://github.com/claimore22/ps5rs) and the supplied shader batch package.
AGC extraction evidence was cross-checked against its `extract_shaders.py` and Kyty's shader structs;
SELF mapping against Kyty `src/loader/elf.{h,cpp}` and
[ps5rs constants](https://github.com/claimore22/ps5rs/blob/master/crates/ps5-format/src/self_constants.rs).
Application source is GPL-2.0-only, compatible with the source-linked Kyty worker; dependencies retain
their own licenses. See `LICENSE` and `THIRD_PARTY.md`.
