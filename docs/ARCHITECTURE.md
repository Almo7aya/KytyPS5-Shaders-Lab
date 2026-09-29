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
`containers.cpp` handles clear SELF normalization and optional bounded Zstandard frames.
`process.cpp` owns OS process/handle lifetime; it never invokes a shell.
`run.cpp` owns worker scheduling, per-case identity, checkpoints, results and comparisons.
`analysis.cpp` implements local research/triage operations.
`kyty_worker.cpp` is the only source tied to Kyty's compiler API.

## Source linkage

The root CMake project adds the selected Kyty checkout as an external source directory into this
project's isolated build tree. In that build graph only, it substitutes the `shader_cfg_tests`
entry point with `kyty_worker.cpp` and changes its output name. This reuses upstream compile flags,
generated headers and link dependencies without maintaining a divergent list of compiler files.
Upstream CTest registration is disabled; the resulting binary is **not** the upstream test suite.
The user-facing build target is `shader-kyty-worker`. No source file in Kyty is patched.

This deliberately trades a larger first build for integration reliability. A future upstream
compiler-library target should replace this adapter. The worker reports the generated upstream
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
