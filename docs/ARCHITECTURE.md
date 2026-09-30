# Architecture and contracts

## Data flow

```text
read-only game tree
  -> mapped files -> format adapters -> evidence-bearing shader candidates
  -> SHA-256(header) + SHA-256(code) object store -> manifest
  -> profile selection -> isolated source-linked compiler subprocess
  -> decode / CFG / IR / resource reads / SPIR-V / validation artifacts
  -> resumable results -> comparison, grouping, origin lookup, log correlation, HTML
```

`core.cpp` owns checked byte reads, file mapping, SHA-256, XXH3, path conversion and atomic reports.
`scan.cpp` owns ELF/AGC detection, candidate validation, provenance and dataset freshness.
`containers.cpp` handles clear SELF normalization and linked compressed blocks, ZIP32 stored/Deflate archives, and
optional Zstandard frames. Nested adapters share expansion, member-count and depth budgets.
ZIP members are traversed from validated central-directory metadata; CRC failures and
encrypted members cannot fall back to raw scanning. Archive member names remain labels.
SELF block tables are resolved by entry identity, independently of the ELF program-header
index. Extents and optional SHA-256 digests are checked before using decoded blocks.
Rejected/encrypted SELF data also has no raw fallback. The adapter does not authenticate
signed executables or decrypt them.
`process.cpp` owns OS process/handle lifetime; it never invokes a shell.
`run.cpp` owns worker scheduling, per-case identity, checkpoints, results and comparisons.
`repro.cpp` owns portable single-case export and fresh replay. It binds both input hashes,
the selected profile and original result; archived diagnostics are never execution inputs.
Replay uses a caller-selected worker and reconstructs requests after relocation. The export
does not embed an executable or authenticate the source evidence. Result statuses are observations,
not a failure-preserving minimization oracle.
`minimize.cpp` uses a stricter diagnostic predicate and repeated isolated runs to reduce repro
inputs. It operates only on fresh output copies, reserves final confirmations and checkpoints
accepted/rejected trials. Code reduction requires fresh hash-bound decoder extents and substitutes
whole instructions with NOP words without relocating code. This preserves observed failure evidence,
not program semantics; reductions and incomplete search are recorded explicitly.
`analysis.cpp` implements local research/triage operations.
`kyty_compiler.cpp` is the source adapter tied to Kyty's compiler API.
`kyty_worker.cpp` is a thin process entry point using the versioned source interface in
`shader_lab/compiler.hpp`; it does not include upstream headers.

## Source linkage

The root CMake project configures the selected Kyty checkout in an isolated build tree for its
dependency targets and platform settings, without patching it. `shader_lab_kyty_compiler` builds
the real upstream recompiler source tree plus its format, descriptor and shader-metadata helpers.
It does not include the game loader, guest libraries, renderer, audio/video code or `shader.cpp`.
There are no fake runtime stubs. `ShaderInit()` is not called: it only initializes the game-runtime
shader mapping table, which this explicit-profile compilation path does not use.

The existing `shader_cfg_tests` executable target supplies platform entry-point flags and the
pthread runtime copy, but both its sources and its link closure are replaced by the thin worker
and compiler library. Upstream CTest registration is disabled; this is **not** the upstream test
suite. The user-facing target remains `shader-kyty-worker`. Upstream compile/include definitions
are inherited at the library boundary, and missing required support files fail configuration.

`compiler_info_v1()` reports the interface/protocol versions, configured revision and source counts;
the CLI exposes it through `--compiler-info`. `execute_compiler_request_v1()` accepts worker-protocol
1 through a request file. This is a versioned **source** interface, not a cross-toolchain binary ABI.
Only one request per process is permitted because upstream lifecycle and fatal-error handling are
process-scoped. Run it on the worker main thread; do not link it into a scanner/UI execution path.
Shader failures remain response statuses, and fatal assertions remain isolated worker exits.

`shader-compiler-sources.txt` in the build/package lists the exact relative upstream sources selected.
This reduces compilation and linking, but upstream configuration still visits its dependency setup,
including unrelated download/configure steps. Removing that configure-time dependency remains work
for a standalone upstream library build entry point; no download-speed claim is made here.

The worker reports the generated upstream
revision, configured clean/dirty identity and its executable content hash (in the parent result).
That executable hash, not a possibly stale human build label, is the compilation cache identity.
The configured clean/dirty label can become stale until CMake is rerun; preserve the exact source
checkout for reproducible investigations. Dependency DLL identity is currently a documented gap.

## Dataset layout

```text
manifest.json
objects/
  HEADER_SHA256-CODE_SHA256/
    header.bin
    code.bin
```

Each shader has its type, declared lengths, Kyty code hash and all `origins`. An origin records
the game grouping, root-relative file path, header/code offset, offset coordinate system,
extraction method and evidence. SELF origins include source-segment mappings; Zstandard origins
include the compressed frame offset/extent and uncompressed size.

ELF extraction checks file bounds before accessing section entries/names and shader data.
Signed self-relative AGC table offsets cannot escape the validated header. Bare pairing is weaker
evidence than a named ELF section: it correlates a register value with a trailer checksum and
requires a unique code content match. It does not verify an undocumented checksum algorithm.
Candidate and comparison-work limits are visible, not silent success.

Scan resume hashes source bytes, compares extractor build identity, and checks paired `.code`
freshness separately. The runner verifies object contents even on cache hits. A manually corrupted
object therefore cannot silently reuse a successful result. Existing corrupt objects are not
overwritten automatically: move the specific affected object aside and rescan into a fresh dataset
when investigating integrity problems.

## Worker protocol 1

The parent executes `WORKER --request ABSOLUTE_REQUEST_JSON`. No shell expansion occurs.
The request includes schema, full case ID, absolute header/code paths, selected profile and
absolute case output directory. Each process gets a fresh working directory.

The worker atomically updates `phase.json`, and writes a final `response.json` carrying the same
schema/ID plus status and diagnostics. Exit 0 without a matching response is a protocol error.
A timeout or nonzero exit cannot be converted into a pass by a partial or stale response.
Previous partial response files are separated before an explicit retry.

Windows uses `CreateProcessW`, a restricted inherited-handle list and a kill-on-close job object;
only the selected worker process tree is terminated at the deadline. A 2 GiB worker memory limit
is enforced. POSIX uses an isolated process group, `execv`, deadlines and an address-space limit;
that backend is unverified on this Windows development run. These are reliability boundaries,
**not a security sandbox** for deliberately hostile compiler exploits. Run trusted local builds.

The log file is not currently quota-limited. Large diagnostics can consume disk until the worker
deadline. The result retains only the final 8 KiB, while the full log stays in the case directory.
Do not scan into a small or shared critical volume without budgeting output space.

## Resource context

Resource materialization may follow pointers in SGPR data and compile-time descriptor graphs.
The worker's callback resolves only bounded captured ranges or its known shader-code bytes;
it never treats a guest integer as a directly readable host pointer. Failed reads remain failures.
All attempted reads are recorded up to a bounded trace count.

For basic compute shaders, selected header registers supply workgroup, LDS, float mode and system
register information. Other omitted state is explicit in assumptions. Pixel and simple vertex
profiles are probes, not complete PM4 reconstruction. Unknown/fused/NGG stages are not guessed into
a different stage merely to increase the success count. Compiler aborts remain isolated evidence.

Per-case profile bundles let one corpus contain different descriptor snapshots. Changing a
default profile does not invalidate cases that select an unchanged explicit profile, while the
top-level run records the complete bundle identity. Do not reuse one captured context for a
different shader merely because both share a stage.

## Correctness ladder

1. **Discovery:** a recognized, range-valid candidate exists. This can still be heuristic.
2. **Decode:** current Kyty understands the observed instruction sequence, subject to its decoder.
3. **Translate:** its CFG/IR/resource analyses complete under this profile.
4. **Materialize:** supplied values satisfy the resource plan, not necessarily the real game.
5. **Validate:** emitted SPIR-V satisfies selected structural rules.
6. **Execute against a reference:** inputs and observable outputs agree under a specified contract.
7. **Game integration:** actual resource lifetime, synchronization, graphics state and visible results agree.

The implemented tool reaches level 5. Levels 6–7 remain future work. A validator accepting a
module cannot prove the implementation of an RDNA instruction, ordering, precision, lane behavior,
or resource address translation is correct. This distinction must survive every report/dashboard.

## Report evidence model

`src/report.cpp` joins manifest cases to results by the complete case ID. Cases without results
remain untested; unmatched run entries are counted as a dataset/run mismatch instead of included
in the success denominator. The report does not invoke the compiler, infer semantic equivalence,
or convert an unknown worker status into a pass.

The C++ generator embeds full diagnostic details and source provenance as HTML-safe JSON alongside
the static UI from `src/report_ui.hpp`. The report is a self-contained offline HTML file: no server,
account, upload or publishing service is involved. Embedded data escapes HTML delimiters, replaces
invalid UTF-8 in logs and preserves origin offsets as hexadecimal strings to avoid JavaScript
integer precision loss. Dynamic text is HTML-escaped before rendering. A content security policy
blocks external resources/network requests. Artifact links use a fixed filename allowlist,
canonical containment under the run's `cases` tree and URL-encoded paths.

Reports are diagnostic artifacts, not anonymized exports. They do not alter source manifests,
results or captures. Keep generated evidence out of the source repository and use synthetic or
generic examples in project documentation.

The UI uses a fixed-row-height virtualized list: every filtered case has a scroll position, while
only the visible rows plus a small buffer exist in the DOM. There are no page boundaries or
load-more actions. Selection is independent of the rendered window; keyboard navigation can reach
the entire filtered set. Filtering and sorting retain selection when it remains a match.

An on-demand tabbed inspector preserves the active evidence category across case changes, avoiding
nested disclosures or constructing every shader's evidence DOM at startup. Source findings have a
searchable list and a directly visible record pane. A phase
checkpoint means the phase started; later checkpoints establish advancement, not instruction-level
semantic correctness. Existing artifacts can survive an earlier retry and are not used as verdicts.
Elapsed time is worker-process wall time; a cached result retains its original attempt duration.

## Extending the tool

Add format adapters with a declared input contract, per-layer provenance, explicit expanded-size
and nesting budgets, known-good fixtures, every-prefix truncation tests and malformed-offset tests.
Never extract archive members to arbitrary paths from an archive's own filename. Return bytes and
a virtual-member identity through the parser instead.

Add compiler-stage support by reproducing the verified upstream preparation path and supplying
its actual runtime dependencies. Add tests for missing state and preserve unsupported outcomes.
Do not add a fake descriptor fallback solely to make validation green.

Add semantic replay only with independently meaningful reference results, complete input resource
snapshots and a documented observation contract. Distinguish exact integer comparison, floating
point/NaN/signed-zero policy, execution divergence, unsupported host features and timeouts. A CPU
oracle derived from the same mistaken formula as the compiler is not independent evidence.
