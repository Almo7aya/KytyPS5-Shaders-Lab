# PS5 Shader Lab

A C++20 offline shader extraction, reverse-engineering and KytyPS5 compiler-regression tool.
It recursively scans game files, retains extraction provenance, and runs individual shader cases
through an independently built worker linked to **your selected KytyPS5 source checkout**.
It never starts a game or executes guest CPU code. The compiler worker does not create a Vulkan device.

This is a usable first implementation, **not a universal game unpacker or proof of shader correctness**.
Valid SPIR-V is a structural result under recorded inputs. It does not prove equivalent pixels,
memory writes, numerical behavior, synchronization, or performance. Every result explicitly says
`semantic_correctness: not_tested`.

See [testing and validation](docs/VALIDATION.md) for reproducible checks and their limits,
and [architecture](docs/ARCHITECTURE.md) for the data and worker contracts.
See [builds and releases](docs/RELEASING.md) for manual builds, fork selection, releases,
packaged binaries, checksums and corresponding sources.

## What is implemented

- Recursive, read-only scanning of every regular file, independent of extension; symlinks are not followed.
- Embedded AMDGPU ELF64 shaders with bounded section/name/header parsing.
- Bare AGC headers paired to code using trailer-checksum evidence; ambiguous matches are reported,
  not guessed. The 256-byte alignment heuristic is retained and disclosed.
- Clear SELF load-segment reconstruction, with original-file, ELF-file and virtual-address mappings.
  Candidate bytes must be backed by actual segments, not zero-filled holes.
- Clear compressed SELF load segments using linked block extents, mixed stored/zlib blocks,
  optional SHA-256 digest checks, padding validation and per-block provenance.
- Versioned ZIP adapter for stored/Deflate members, central-directory and streamed-descriptor
  validation, CRC verification, nested traversal and member provenance. Members are never written
  to paths provided by the archive.
- Independently framed and nested Zstandard payload discovery when built with the Kyty worker dependencies.
  Shared expansion, nesting-depth, frame-count and candidate limits bound work across the input tree.
- Existing `.header` / `.code` pairs, without requiring Python.
- SHA-256 identity for **both header and code**, plus Kyty-compatible XXH3-64 code hashes.
  Different headers are never collapsed just because their code hashes match. All origins are retained.
- Content-verified incremental scanning; changed extractor sources invalidate cached parsing.
- One compiler subprocess per case; parallel jobs, deadlines, 2 GiB process memory limit,
  per-case logs, phase checkpoints, crash isolation and resumable results.
- Real Kyty decoding, native CFGs, translation/IR, resource materialization, SPIR-V generation
  and SPIRV-Tools validation for Vulkan 1.3. No second imitation compiler.
- A compiler-only static-library source boundary and versioned worker entry point, with an
  auditable source list. Upstream configure-time dependency setup is still inherited.
- Header-derived compute probes, explicitly approximate pixel probes, explicit simple vertex
  profiles, optional supplied user-data and bounded memory snapshots.
- Explicit pixel compiler metadata including barycentric/custom interpolation, export channel
  mappings, sample masks and dual-source/alpha-remap blending, with every default disclosed.
- Declared Vulkan host profiles drive compute subgroup lowering and separately assess emitted
  SPIR-V feature/property requirements. Unknown support and runtime-compatibility limits remain explicit.
- An isolated compute-fixture execution protocol for explicitly selected trusted backends,
  hash-verified initial resources, reference-output comparison and an explicit GPU opt-in gate.
  A separate bounded CPU integer ISA model and experimental
  [Vulkan buffer-compute backend](docs/VULKAN_REPLAY.md) are available; broad GPU/graphics and
  trusted hardware reference coverage remain open.
- Captured compute SH-register/PM4 replay through the selected upstream register decoder
  and `PrepareProgram`, with required-state checks and per-write provenance.
- Guest disassembly, instruction inventory, opcode histogram, CFG text/JSON/DOT, intermediate/final IR,
  memory-read trace, SPIR-V binary/disassembly and validator diagnostics.
- Searchable offline HTML reports; failure grouping; run-to-run outcome/SPIR-V comparisons;
  hash-to-source lookup; correlation with `hash=0x...` observations from existing emulator logs.
- Portable, content-verified single-case repro bundles and fresh isolated replay with an
  explicitly selected worker. Repro artifacts retain private provenance; review before sharing.
- Bounded, failure-preserving reduction of captured inputs and decoded instruction sequences,
  with repeated fresh-worker confirmation, detailed failure predicates and a portable final case.
- Upstream pass entry/return traces, intentional prefix stops with IR evidence, and repeated
  prefix bisection of reproducible compiler assertions. This localizes failure, not semantic defects.

## Important coverage limits

“All regular files visited” is not “all shaders recovered.” Encryption, game-specific archive
indexes, Oodle/Kraken, ZIP64/multidisk ZIP, 7z/PSARC/Unreal container decoding, unsupported SELF block-table encodings, unknown-size
or dictionary-dependent Zstandard frames, unsupported nested formats, cross-file bare-header pairing,
patched/runtime-generated code and dynamically loaded shader libraries can leave shaders undiscovered.
Container warnings and rejected/unpaired candidates remain in the manifest and report. Use authorized,
already-unpacked data where necessary. No decryption keys, DRM bypass or game redistribution is needed.

The worker currently does **not** reproduce `AgcCreateShader`, complete PM4 state,
graphics-stage preparation, fetch-table construction, shader fusion or full NGG/mesh/tessellation setup.
Captured compute supports the bounded preparation path described below. Other stages are decoded
but normally report `missing_stage_context`. Full stage coverage remains future work. The current
focus is traceability, corpus identity, clear SELF handling, failure isolation and direct linkage
to the selected compiler.

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

From this project directory, with a KytyPS5 checkout in the sibling `../KytyPS5` directory
(adjust `KYTY_ROOT` for your checkout):

```powershell
cmake -S . -B build-kyty -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DKYTY_ROOT="../KytyPS5" -DSHADER_LAB_BUILD_KYTY_WORKER=ON
cmake --build build-kyty --target shader-lab shader-kyty-worker shader-vulkan-replay shader-lab-tests --parallel 8
ctest --test-dir build-kyty -R '^shader_lab_' --output-on-failure
```

Outputs include `build-kyty/shader-lab.exe`, `build-kyty/shader-cpu-reference.exe`,
`build-kyty/shader-kyty-worker.exe`, `build-kyty/shader-vulkan-replay.exe` and the required
`libwinpthread-1.dll`. Keep the DLL beside these workers. The scanner itself does not need that DLL.
The worker links the smaller compiler-only library, but the first configuration still visits
upstream dependency setup. Subsequent compiler edits rebuild incrementally. Pass checkpoints use
a generated build-tree copy of the upstream pipeline; the selected checkout's files are not edited.

Windows executables opt into long paths. Deep datasets/repro directories also require the Windows
`LongPathsEnabled` system policy; the application never changes this machine-wide setting.
See [Microsoft's long-path requirements](https://learn.microsoft.com/windows/win32/fileio/maximum-file-path-limitation).
CI enables the policy only on its disposable Windows runner and exercises long paths.
Windows still [limits the working directory used to launch a process](https://learn.microsoft.com/windows/win32/api/winbase/nf-winbase-setcurrentdirectory). Deep case directories
therefore launch from their nearest short ancestor; absolute request/output paths retain the
original evidence layout. Results disclose `worker_working_directory`. Third-party workers
must use the request's absolute paths, not assume their current directory is the case directory.

For a lightweight extraction/reporting-only build:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_C_COMPILER=clang-cl
cmake --build build --parallel 8
ctest --test-dir build --output-on-failure
```

Without a local dependency directory, CMake fetches pinned nlohmann/json and xxHash releases.
ZIP support links pinned zlib 1.3.2 statically in both build configurations. The lightweight build
does not include Zstandard decoding. Linux extraction/report/campaign/reference fixture checks
run in GitHub Actions; the source-linked worker remains Windows-validated.

The adapter was built against clean KytyPS5 revision
`2650478d92c394c092b36ee129962060c623ca16`. A changed upstream API may require adapter maintenance;
compilation should fail visibly instead of silently switching to an older compiler.

## Scan your games and compile the corpus

```powershell
.\build-kyty\shader-lab.exe scan --input "inputs" --output datasets/all-games
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
.\build-kyty\shader-lab.exe inspect --dataset datasets/all-games --hash KYTY_HASH --output runs/shader-origin.json
.\build-kyty\shader-lab.exe correlate --dataset datasets/all-games --log "captures/emulator.log" --output runs/runtime-coverage.json
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

Nested ZIP/Zstandard scanning retains each member/frame coordinate layer and versioned adapter evidence.
Per input file, expansion is bounded to 512 MiB total, 256 MiB per frame, four decoded layers
and 4,096 members/frames/SELF blocks across all layers. Intermediate decoded containers also consume the
budget; it is not reset by nesting. Limit findings disclose incomplete exploration. Clear SELF
normalization and supported clear block decompression also work inside decoded frames.

The `zip/1` adapter handles ZIP32 methods 0 (stored) and 8 (Deflate), with or without
data-descriptor signatures. Local headers must agree with the central directory, and
member CRCs must match before shader extraction. Encryption, ZIP64, multidisk layouts
and unsupported methods produce explicit findings. Archive names are evidence labels,
with non-ASCII bytes escaped, never output paths. Once a ZIP is recognized, rejected
members are not scanned again as raw bytes to bypass integrity/encryption checks.

The `self/2` adapter handles clear compressed data with a unique linked extent table
and compression-window encoding 4 (zlib window 12). It checks block extents, partial
final blocks, padding encoding and available SHA-256 block digests. Reconstructed
segment mappings distinguish physical payload ranges from decoded offsets. Encrypted
data/tables, compressed metadata tables, missing extents and unsupported windows are
reported, not guessed. Recognized SELF containers do not fall back to raw scanning
of rejected or encrypted segments. A block digest is an integrity check, not signature
authentication. Format references: [block writer](https://github.com/flatz/pkg_pfs_tool/blob/main/src/self.c)
and [entry definitions](https://github.com/flatz/pkg_pfs_tool/blob/main/src/self.h).

## Reading the HTML report

Open `report.html` directly in a browser; it is self-contained and makes no network requests.
After updating the tool, regenerate existing HTML with `report` to get the new layout. This reads
the manifest/results and does **not** rerun shaders or alter their verdicts.

Reports retain full source provenance, diagnostic text, captured profiles and artifact links for
offline investigation. No account, server, upload or publishing service is involved. Generated
reports and captures are excluded from version control; documentation examples use generic inputs.

The dashboard separates structurally valid modules, blocked/unsupported cases, failed attempts,
and semantic verification (not performed). The valid-SPIR-V percentage uses cases with recorded
results as its denominator; it is **not a correctness or game-compatibility score**. Summary counts
remain corpus-wide while the explorer filters by outcome, header stage, game/source group or text.
Results can be sorted by attention needed, hash, worker duration or code size. All matches are
available in one continuously scrollable list, without pagination or a load-more button. The list
renders only the visible rows to keep large corpora responsive. Use Up/Down or Home/End while the
list is focused; **Find selected** returns to the current shader without changing the selection.

The inspector has six direct-access tabs: **Overview**, **Diagnostics**, **Context**, **Sources**,
**Artifacts** and **Raw data**. Overview surfaces the verdict, recommended next step, key failure
and compiler progress. Logs, profiles and source evidence are visible directly in their respective
tabs, without nested collapsible panels. The active tab and selected shader are preserved while
comparing cases or changing filters, whenever the selected shader still matches. Tab headers also
support Left/Right and Home/End keyboard navigation.

Select a shader to see:

- A plain-language verdict, its limits and a recommended next investigation step.
- Compiler phase progress; markers mean execution reached a phase, not that its semantics passed.
- Instruction/IR/output sizes, elapsed process time, exit code and cached-result reuse.
- Validator diagnostics, unsupported instruction PCs, fatal messages and worker log tails.
- Selected profiles, returned assumptions, effective compute/pixel state and compiler fingerprints.
- Source origins, offsets, pairing evidence and the complete header+code identity.
- Links to available disassembly, CFG, IR, memory-read traces and SPIR-V artifacts.

Artifact links are relative where possible. Keep the run files in place; moving only the
HTML preserves embedded evidence but can break those links. Existing files may survive a failed
retry, so their presence alone is not proof that the latest attempt produced them. File findings,
traversal failures, partial scans/runs and results absent from the selected manifest are disclosed.
Unknown outcome codes are never treated as passes. Extracted cases without results remain untested.

Reports contain diagnostic evidence and source paths. Review their contents and redistribution
rights before sharing them; the tool does not transmit them anywhere.

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
Profiles select one context per case per run. The worker's checked-in profile parser is the
authoritative supported field list.

### Explicit pixel compiler inputs

`profiles/pixel-context.example.json` is a **fictional**, fully specified pixel input example.
It covers the compiler-input fields in upstream `ShaderPixelInputInfo`; the post-compilation
`stage` runtime pointers are not serialized or fabricated. This is not graphics `PrepareProgram`
or PM4 replay: supply the **effective** inputs after render-target mapping and blend-state
specialization. The worker does not derive them from framebuffer formats or blend factors.
The AGC header must identify a pixel shader. `wave_size` and `user_data` remain root profile fields.

| Pixel fields | Contract |
| --- | --- |
| `input_num`, `interpolator_settings` | 0–32 inputs; exactly one raw u32 interpolation setting per active input. |
| `custom_interpolation_mask` | One bit per active custom-interpolated input; bits outside `input_num` are rejected. |
| `ps_perspective_center_vgpr`, `ps_perspective_centroid_vgpr` | First register of a two-VGPR pair, 0–254; `4294967295` disables that pair. |
| `ps_system_input_base` | First system-input VGPR; enabled position components, front-face and ancillary follow in that order. Input extents must fit 256 VGPRs without overlaps. |
| `target_output_mode` | Eight raw four-bit export modes. This records encoded state, not a guarantee that every encoding compiles. |
| `target_export_mapping` | Eight packed u8 physical-to-logical channel mappings: two bits per channel. Identity RGBA is `228` (`0xe4`); reversed ABGR is `27` (`0x1b`). |
| `scratch_size_dwords` | Unsigned scratch extent from the supplied profile or header. |
| `ps_pos_x/y/z/w`, `ps_front_face`, `ps_ancillary`, `ps_no_perspective` | Boolean stage-input controls; field names for position are individually `ps_pos_x`, etc. |
| `ps_pixel_kill_enable`, `ps_depth_export_enable`, `ps_sample_mask_export_enable`, `ps_sample_shading`, `ps_early_z`, `ps_execute_on_noop` | Boolean fragment execution/export controls. Acceptance does not mean every upstream flag has a lowering effect. |
| `dual_source_blending`, `alpha_blend_source_remap` | Effective post-blend specialization flags, not automatic detection. |

Dual-source output requires matching active MRT0/MRT1 modes. Guest dual-source blending also
requires matching mappings. Alpha remapping requires dual-source mode, a non-integer MRT0 mode,
identity MRT1 mapping and no active MRT2–MRT7; the worker does not silently repair conflicting inputs.

Omitted fields retain probe defaults: header input count/scratch, zero interpolation settings,
system VGPR base zero, disabled barycentric pairs, identity export mappings, export mode 9 for
all eight targets, and false booleans. These can be inappropriate for a real draw. `effective_pixel`
in the response and report's Context tab records the complete normalized inputs and the exact
`defaulted_fields` list. `context_snapshot` does not authenticate them. Compiling an example
successfully is not evidence of matching pixels, blending, depth/stencil behavior or host support.

### Captured compute preparation

`profiles/captured-compute.example.json` is a **fictional** schema example for the
`captured_compute` profile mode. The `capture` object contains provenance, an explicit
`use_header_registers` choice, an initial SH-register snapshot and PM4 dwords ending at
one `DISPATCH_DIRECT`. Register offsets are relative to the SH register space, not byte
addresses or absolute hardware register indices. Header SH writes apply first when enabled,
then the initial snapshot, then packet writes in stream order. Header context registers are
not used by this compute path and that fact is recorded.

The `compute_pm4/1` adapter accepts only ordinary type-3 `SET_SH_REG` and a final
`DISPATCH_DIRECT`. Predication, indexed writes, indirect buffers/dispatches, memory writes,
custom NOP commands, synchronization and unknown packets are rejected rather than skipped.
Captures are limited to 256 initial register entries and 1,048,576 PM4 dwords. This is a
bounded dispatch-state slice, not a whole submission emulator, game tracer or GPU execution.

Required final state includes `COMPUTE_NUM_THREAD_X/Y/Z`, `COMPUTE_PGM_LO/HI`,
`COMPUTE_PGM_RSRC1/2/3`, and every user SGPR referenced by the encoded count. Missing
values are never implicitly zeroed. Supported direct compute user-data slots are 0–15.
The final program address must equal the explicit, aligned 48-bit `shader_base` alias
for this case. Group size is bounded to 1,024 invocations. Zero dispatch dimensions,
disabled dispatches and unknown initiator bits are rejected. Supply a complete snapshot
at the beginning of the slice; this tool does not reconstruct omitted earlier commands.

The worker applies the selected upstream compute-register decoder, registers the owned
code extent and scratch metadata in its private upstream shader map, and calls the actual
compute `PrepareProgram`. Guest wave size comes from the dispatch initiator; user SGPRs,
group/thread inputs, LDS and floating-point mode come from prepared state. The capture
profile cannot also specify probe `compute`, `wave_size` or `user_data` overrides.
`host_subgroup_size` remains an explicit host assumption, not a queried GPU feature set.
For feature/property declarations, use [host profiles](docs/HOST_PROFILES.md). Their separate
requirement assessment does not replace structural validation or establish runtime compatibility.

`captured-state.json` retains all writes, packet positions, final registers, dispatch and
prepared user data. Results record effective compiler metadata and the preparation path.
Provenance is user-supplied, not authenticated. Memory descriptors still require bounded
`memory` snapshots; no supplied guest address is directly dereferenced. A legacy hash
trailer requested by upstream preparation must lie inside the owned code bytes or the
case is rejected. This mode does not implement AGC header relocation or graphics fetch
tables, and valid SPIR-V still says nothing about output equivalence.

### Multi-context campaigns

Use a campaign to run the corpus under several explicit profiles or profile bundles:

```powershell
.\build-kyty\shader-lab.exe campaign --dataset datasets/all-games --worker build-kyty/shader-kyty-worker.exe --plan profiles/campaign.example.json --output runs/campaign --jobs 4
```

A schema-1 plan contains 1–128 `contexts`, each with a unique `name` and an inline
`profile` object. A profile can also be a `default`/`cases` bundle as shown above.
Contexts run sequentially, with `--jobs` isolated case workers inside each context.
The example compares wave32 and wave64 probes; neither is a captured ground truth.

`campaign.json` checkpoints context progress and links each context's ordinary
`results.json`, which can be passed to `report`, `compare` or `cluster`. It also
lists case IDs whose outcomes or SPIR-V hashes differ between contexts. Those
differences are observations, not proof of regressions or semantic correctness.
Worker failures remain individual results rather than terminating the campaign.

Resume caches include the complete plan, dataset manifest, worker hash, deadline
and case limit. Changed inputs create a new campaign directory without deleting
earlier evidence. `--no-resume` reruns the selected contexts. `--limit` applies to
each context, not the entire campaign, and is reported as partial coverage.
Names are labels, never paths. Output must not overlap the dataset or game inputs.
After a hard crash, confirm no campaign is running before removing `.campaign-lock`.

## Portable single-case reproductions

Export one complete case ID from a run (or a campaign context's `results.json`):

```powershell
.\build-kyty\shader-lab.exe repro --dataset datasets/library --results runs/baseline/results.json --case HEADER_SHA256-CODE_SHA256 --output runs/repro-case
.\build-kyty\shader-lab.exe replay --bundle runs/repro-case --worker .\build-kyty\shader-kyty-worker.exe --output runs/replayed-case
```

Use fresh output directories disjoint from the dataset, original run and game input.
The bundle contains `header.bin`, `code.bin`, the exact selected `profile.json`, original
result/provenance JSON and bounded diagnostic artifacts. It can be moved to another machine;
`replay` constructs new absolute request paths and a one-case dataset. No executable, DLL or
original game archive is copied. Supply a trusted worker and its required dependencies yourself.

Every replay runs a fresh isolated process without cached results. `replay.json` distinguishes
worker identity, deadline and outcome matches. A different worker is allowed for regression
investigation and explicitly reported. Matching status is **not** proof of the same root cause
or shader semantics. DLLs, host features and environment are not yet fingerprinted.
New runs record their deadline; replaying an older run requires explicit `--timeout-ms N`.
The replay's ordinary results can be passed to `report`, `compare`, `cluster` or another export.

Input hashes are checked before execution; they detect corruption, not maliciously rewritten
metadata or authentic game execution. Diagnostic copies use a fixed allowlist, canonical path
containment, 64 MiB per-file and 128 MiB total artifact limits, with omissions recorded. Existing
artifacts may be from earlier retries, so copied artifacts are never used as fresh verdicts.
The descriptor is written last; a failed export without `repro.json` is not replayable.

Bundles contain shader bytes and original paths, resource snapshots and diagnostics. Keep them
out of version control and review redistribution rights before sharing.

### Failure-preserving reduction

```powershell
.\build-kyty\shader-lab.exe minimize --bundle runs/repro-case --worker .\build-kyty\shader-kyty-worker.exe --output runs/reduced-case --max-attempts 128 --confirmations 2
```

The worker executable must match the original bundle's content hash. To investigate a different
compiler, replay with that compiler and export its new result first. The reducer checks that the
original failure repeats, proposes reductions, and accepts each proposal only after every requested
confirmation matches. Final confirmation attempts are reserved even if the search budget runs out.
The process limit includes baseline and final runs; confirmations must be 2–5. An old bundle
without a recorded deadline needs `--timeout-ms`. No cached compiler results are used.

Supported predicates are detailed unsupported-instruction diagnostics, SPIR-V validator messages
under the same validation environment, and crashes with an explicit Kyty assertion line, phase
and exit code. Only `hash=0x...` values in assertion lines are normalized; addresses, PCs and source
locations are not. Validator instruction indices may shift, but diagnostic text and severity must
match. A different failure with the same status is rejected. Success, timeout, missing-context,
generic crash and incomplete diagnostics are not minimization oracles. Matching the predicate is
an observation, not proof of identical root cause.

Transformations remove captured memory ranges, zero nonessential user-data/memory words, and replace
whole decoded instructions with `S_NOP 0` words. They do **not** shorten code or shift PCs. Literal
words belonging to an instruction are replaced together. Only a fresh, content-bound decoder
inventory supplies instruction boundaries; otherwise code reduction is explicitly unavailable.
Header bytes, code extent, register-array positions and captured memory addresses are retained.
Modified code is marked as derived evidence, not byte-identical source-game data. No transformation
claims to preserve shader outputs, timing, synchronization, resource behavior or gameplay.

`minimization.json` checkpoints every attempt, predicate and accepted change. Each proposal retains
its exact inputs and fresh run directories. `minimized-bundle` is emitted only after final checks
pass, and works with the ordinary `replay` command. A `fixed_point` means no single available
transformation was accepted at the final state, not a global minimum. `budget_exhausted` returns the
best confirmed case with `minimality: incomplete_search`; inspect the state even after exit 0.
Exit 4 means the baseline/final failure did not repeat, and 2 means setup or evidence was invalid.
Keep enough disk space for retained attempts: the process-count limit is not a disk quota.

### Compiler pass tracing and assertion bisection

Add `"diagnostics": {"pass_trace": true}` to a compiler profile to record ordered
entry/return events in `pass-trace.json`. `shader-kyty-worker --compiler-info` lists the
versioned catalog: frontend translation, SSA and cleanup passes, resource planning and
materialization, specialization, binding allocation and SPIR-V emission. The conditional
read-lane cleanup sequence is one grouped boundary. Decode/CFG setup before frontend
translation and validation after emission are not individually instrumented passes.

Adding `"stop_after_pass": N` inside `diagnostics` stops after that catalog index and
returns `pass_checkpoint_reached`, **not** a compilation/validation success. The worker
retains `pass-stop.ir` when IR exists and its text fits within 64 MiB. Index 0 is the
translation input boundary and has no IR. Pass return does not certify intermediate IR;
upstream cleanup stages can temporarily have invariants that only later stages restore.

To locate a reproducible fatal assertion from a normal, complete compiler attempt:

```powershell
.\build-kyty\shader-lab.exe bisect-passes --bundle runs/repro-case --worker .\build-kyty\shader-kyty-worker.exe --output runs/pass-search --confirmations 2
```

The bundle and worker must match, and the original detailed assertion must repeat before
instrumentation is enabled. A traced baseline must reproduce the same phase, assertion text
and exit code. The search runs each prefix in a fresh isolated process, requiring 2–5 identical
observations, then separately reconfirms both adjacent endpoints. It never skips or reorders
passes. Changed assertions, missing traces, malformed identities, timeouts and unstable event
sequences cannot establish a boundary. Source files and the original bundle remain untouched.

`pass-bisection.json` retains every probe and fresh run path. `localized` means the first
unreachable checkpoint and its preceding completed checkpoint repeated; inspect the final
entry/return event to distinguish a failure inside a pass from code between checkpoints.
`after_last_checkpoint` identifies a failure beyond all instrumented returns. Neither outcome
proves which earlier pass introduced the bug. Failures before the first checkpoint or changed
instrumented baselines remain unlocalized. Exit 0 means the boundary was confirmed, 4 means
inconclusive/unlocalized, and 2 means invalid setup/evidence. Probe count is bounded by the
fixed catalog and confirmation count; retained logs/IR are not subject to a total disk quota.

This is assertion-prefix bisection, not pass-disable experimentation or a shader-output oracle.
It does not bisect SPIR-V validator errors or numerical/rendering differences. Those need
intermediate-state predicates or reference execution; ordinary output comparison is not enough.

## Outcomes

### Isolated execution fixtures

`fixture-info` derives an identity from actual shader/header/profile/resource bytes.
`execute-fixture` snapshots known inputs, invokes an explicitly selected execution backend in a
fresh child process, validates its response, and compares buffers/images with an independently
supplied reference record. Guest wave/EXEC, workgroup/dispatch dimensions, bindings, initial
resource bytes and comparison policies are recorded. GPU backends require both `--backend-kind gpu`
and `--allow-gpu`; scanning and compiler runs never opt in automatically.

See [the complete execution protocol](docs/EXECUTION.md) for fixture/worker schemas, budgets,
reference provenance, commands, results and isolation limits. The shipped compiler worker is
**not an execution backend**. Protocol fixtures use a test double; a separate
[CPU reference backend](docs/CPU_REFERENCE.md) now interprets a bounded RDNA2 integer/global-buffer
subset without Kyty code. Its actual subprocess tests use fixed expected bit patterns and an
independent assembler check. These are not hardware certification.
The separate [Vulkan backend](docs/VULKAN_REPLAY.md) implements opt-in buffer-compute dispatch
and readback. Graphics, broader reference coverage and broader GPU replay remain open milestone work.

### Recorded reference-output comparison

`verify` is the output-comparison component of the reference-execution milestone.
It does not yet dispatch GPU work or capture hardware reference data:

```powershell
.\build-kyty\shader-lab.exe verify --reference captures/reference.json --observed captures/observed.json --output runs/comparison.json
```

Use a fresh output filename. Exit `0` means the recorded fixture outputs match;
`4` means mismatch or incomparable fixture/layout, and `2` means invalid evidence
or setup. A record has this shape (the hash placeholders must be replaced with
actual lowercase SHA-256 values):

```json
{
  "schema": 1,
  "fixture": {
    "id": "compute-fixture-1",
    "shader_sha256": "SHADER_SHA256",
    "input_sha256": "INPUT_SHA256",
    "profile_sha256": "PROFILE_SHA256",
    "wave_size": 32,
    "exec_mask": "00000000ffffffff"
  },
  "reference_source": {
    "kind": "independent_model",
    "identifier": "model-and-version",
    "method": "Explain how expected outputs were obtained independently"
  },
  "outputs": {
    "result": {
      "file": "result.bin",
      "sha256": "RESULT_SHA256",
      "kind": "buffer",
      "type": "u32",
      "comparison": {"mode": "exact"}
    }
  }
}
```

The observed record uses the same fixture identity and resource layout but names
its own output files and hashes. `reference_source` is required on the reference
record only; `hardware_capture` is also an accepted source kind. The comparator
records this provenance as **user-supplied, not independently authenticated**.
Fixture fingerprints and wave/EXEC state must agree exactly. They describe the
claimed execution inputs; this comparison command cannot attest that an external
producer actually executed them. Output file contents are hash-checked directly.

Integer resources (`u8`, `u32`, `i32`) are compared exactly in little-endian order.
For `f32`, the reference must specify all of these rules; the observed record
cannot loosen the reference's tolerances:

```json
{
  "mode": "float32",
  "absolute_tolerance": 0.0,
  "relative_tolerance": 0.0,
  "max_ulps": 0,
  "nan_policy": "reject",
  "signed_zero_policy": "distinct"
}
```

Finite values match if their difference is at most `absolute_tolerance +
relative_tolerance * max(abs(expected), abs(observed))`, or their ordered float32
ULP distance is within `max_ulps`. Infinities require identical sign. NaNs never
match under `reject`; `equal_bits` instead requires identical NaN bit patterns.
Signed zeros require identical sign unless `signed_zero_policy` is `equal`.
These policies are explicit fixture choices, not claimed RDNA floating-point rules.

An image uses `kind: "image"` plus `shape: [width, height, depth, channels]` and
tightly packed row-major scalar components. Padded, tiled, compressed and opaque
image formats must first be normalized by their capture/replay backend.
Each record supports 1–128 outputs; total reference plus observed bytes are
bounded to 128 MiB. Resource paths must resolve inside their record directory.
The report counts every mismatch and retains the first 64 examples per output.
A match is scoped to these recorded outputs, never a general shader-correctness claim.

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
| `pass_checkpoint_reached` | An explicitly requested compiler prefix returned; no validation or semantic verdict. |
| `runner_error`, `worker_protocol_error` | Orchestration/artifact problem rather than a shader verdict. |

CLI exit codes: `0` means the command completed (individual shader failures remain in results),
`2` is a command/setup error, `3` is an explicitly limited scan or traversal error. Inspect all
manifest file statuses for read failures/coverage gaps even after exit 0. No status means “100% correct.”
Use result JSON rather than a successful process exit as your regression gate.

## Next development milestones

Work is tracked against all five items below. Multi-context campaign orchestration
is implemented, together with portable repro export/replay and failure-preserving reduction.
The worker now has a smaller compiler-library source/link boundary, pass traces and
assertion-prefix bisection; standalone upstream configuration and semantic pass bisection
are still open.
ZIP32 and compressed clear SELF
adapters, nested payload budgets, reference-output comparison, the isolated execution-fixture
protocol and a bounded independent integer CPU model are added
components of milestones 2 and 3; neither milestone is complete. Bounded captured compute
preparation and explicit pixel compiler metadata are implemented as parts of milestone 1;
declared host-feature checks are also implemented. A hash-bound
[compiler resource-layout artifact](docs/COMPILER_LAYOUT.md) exports finalized descriptor arrays,
materialized resource metadata and unresolved runtime offset slots for future replay work.
The separate experimental Vulkan backend consumes that artifact, queries the selected device and
executes a restricted buffer-compute subset. Graphics preparation/partners, broader runtime host
validation and physical-hardware reference coverage remain open. The remaining
parts of milestone 5 and milestones 1–4 remain open. New milestone work stays on
the development branch pending GitHub validation. Compiler-only results are not
execution conformance.

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

Build directories, datasets and generated runs are excluded from version control. When moving a
checkout, configure a **fresh build directory** with the appropriate `KYTY_ROOT` rather than
reusing absolute-path CMake caches. Do not contribute proprietary shader captures or generated
artifacts without the necessary redistribution rights.

Inspired by [ps5rs](https://github.com/claimore22/ps5rs). AGC extraction is checked against Kyty's
shader structures; SELF mapping against Kyty `src/loader/elf.{h,cpp}` and
[ps5rs constants](https://github.com/claimore22/ps5rs/blob/master/crates/ps5-format/src/self_constants.rs).
Application source is GPL-2.0-only, compatible with the source-linked Kyty worker; dependencies retain
their own licenses. See `LICENSE` and `THIRD_PARTY.md`.
