# Semantic validation from the main command

```powershell
shader-lab "inputs" "runs/analysis" --semantic --allow-gpu
```

The same command scans games, compiles shaders, assesses semantic evidence and writes
`report.html`. No separate execution command or manually authored JSON is required for supported
automatic tests. Everything stays offline; generated files go into the output folder only.

- Without flags: compiler validation only; semantic results say **not requested**.
- `--semantic`: generate supported synthetic fixtures and independent CPU reference outputs.
  Optional supplied references are checked too. No GPU is opened.
- `--semantic --allow-gpu`: additionally replay supported fixtures through the bundled
  `shader-vulkan-replay` and compare observable outputs with the independent references.
- `--allow-gpu` alone is rejected. Generated state is explicitly synthetic, never claimed to
  be reconstructed game state. Missing implementation never becomes a pass.

Keep `shader-cpu-reference`, `shader-vulkan-replay` and their runtime dependencies beside `shader-lab` and
`shader-kyty-worker`, from the same build. Build the `shader-vulkan-replay` target for execution.
Vulkan execution is experimental; unknown shaders can hang a driver. A worker timeout is
not a GPU reset or a security sandbox. Use trusted fixtures and an appropriate test machine.

## What is generated automatically?

For compute cases without a supplied test, a separate CPU worker decodes the original shader
using the independent integer model. It does not use Kyty's decoder, IR or translated output
to compute the expected answer.

Eligible shaders get six planned variants: wave32 and wave64, each with zero-filled, integer
boundary-pattern and deterministic seeded input buffers. Tests use one full-EXEC workgroup,
up to four direct linear 4 KiB integer buffers, and explicit synthetic user-data registers.
The model executes each variant to produce expected bytes for every buffer, including unchanged
regions. These files, profiles, hashes and model traces are retained for inspection.

The first generator supports the [CPU model's integer operations](CPU_REFERENCE.md) with direct
buffer descriptors in `s[0:15]` and at least one observable buffer store. It does not generate
global/BDA addresses, graphics state, textures, floating-point operations, branches, LDS, scratch
or indirect descriptor tables. Code is bounded to 4,096 dwords. Undefined registers, out-of-range
accesses and conflicting writes are refused by the model. Failed variants remain visible rather
than being discarded to obtain an overall match. Cases with no observable stores are not passed
vacuously. A CPU generator process has a ten-second deadline; GPU comparisons remain isolated
and sequential, with a thirty-second process deadline per variant.

The profile is a **synthetic experiment**, not an inference about how the game launches its shader.
Passing six tests is limited differential evidence, not full path coverage, hardware certification
or proof that a game renders correctly. Most complex game shaders may still be unsupported.

## Optional: provide captured or hand-authored tests

A game folder usually does not contain the runtime buffers, descriptors, register state and
independent expected outputs needed to reproduce its actual execution. Synthetic tests do not
replace those captures. To test specific captured state instead, optionally supply an index.

Place a local index at `INPUT_FOLDER/.shader-lab/semantics.json`:

```json
{
  "schema": 1,
  "tests": [
    {
      "case_id": "HEADER_SHA256-CODE_SHA256",
      "fixture": "example/fixture.json",
      "reference": "example/reference.json"
    }
  ]
}
```

This is a shape example, not a runnable fixture. Use the complete case ID from the report or
`OUTPUT_FOLDER/dataset/manifest.json`. Paths are relative to `.shader-lab`, must stay inside
it, and must identify regular files. The index is limited to 1 MiB and 4,096 tests.
Multiple tests may target the same case with different inputs and profiles. Supplied tests take
precedence for their case, including invalid/incomplete entries: the tool does not hide a broken
capture by substituting an automatic test. Other cases can still be generated automatically.

Each entry uses the existing [execution fixture format](EXECUTION.md) and reference-output
record. It needs verified shader bytes, an explicit profile, dispatch state, initial resources,
expected output bytes, comparison policies and reference provenance. The header and code must
match the extracted case; references must identify that exact fixture and its inputs.
The execution harness validates complete output sets and layouts before launching a worker.

Expected outputs must come from independently justified evidence, not the Kyty translation
being tested. References are supplied by the user: hashes establish consistency, **not**
authenticity or model correctness. The [CPU model](CPU_REFERENCE.md) can support reference
research for its limited instruction subset; this command does not manufacture references
from a shader's own translated outputs.

The [Vulkan backend](VULKAN_REPLAY.md) currently supports a restricted buffer-compute contract.
Graphics, missing memory, unsupported instructions/resources and unsupported execution state
remain untested. Executing arbitrary extracted shaders is not automatically possible offline.
The backend recompiles the matched shader under its fixture profile; that can differ from the
normal header-probe compilation displayed elsewhere in the report.

## Read the results

Each game has semantic counts and a semantic filter. Each shader has a **Semantic evidence**
tab with reasons, individual test records and links to comparison/execution artifacts.

| Result | Meaning |
| --- | --- |
| Matched tested inputs | All planned tests for this case matched their references; limited evidence only |
| Output mismatch | At least one test differed from its supplied reference; investigate both translation and reference/state |
| Missing evidence | No usable fixture/reference is available in the recorded assessment |
| Unsupported | The stage or execution contract is not implemented |
| GPU permission required | Evidence was checked, but translated execution was not authorized |
| Invalid evidence | Malformed data, missing files, identity mismatch or an invalid reference |
| Backend missing/error/timeout | Execution could not produce a reliable comparison |

**No result means 100% correctness.** A match applies only to those inputs, state, observable
outputs and comparison tolerances. It does not prove every path, every resource configuration
or in-game rendering. Aggregate results preserve mismatches and incomplete tests rather than
hiding them behind a successful test. Index entries that cannot be assigned to an extracted
case appear as scan-wide semantic issues.

Results are saved in `semantic/results.json` and embedded in `run/results.json`. Execution
artifacts for supplied tests live under `semantic/attempt-N/`. Automatic fixtures, references,
generator logs and GPU comparisons live under `semantic/generated-N/`. The report links both
generation and execution evidence. Compiler/extraction caches still resume, but semantic
execution is fresh on each opted-in run and previous attempts are preserved. Re-running without
`--semantic` marks current results not requested; historical attempts are not silently reused.
Exit code 0 means the workflow completed, not that comparisons passed. Exit code 3 still means
scan limits or I/O gaps. Consult semantic statuses and issues for test results.

## Scanner limits versus diagnostics

Repeated diagnostics are bounded and summarized with suppressed counts; they no longer stop
shader extraction. Nested-container diagnostics are also bounded. Real candidate-memory,
search-work, archive-member, nesting and expansion limits remain enforced and reported separately.
Limited files use `extraction_limit`; older manifests may contain `candidate_limit`.

Unknown-size Zstandard frames use bounded streaming decompression instead of being skipped just
because their size is unknown. Resource limits and unsupported/encrypted formats remain possible
coverage gaps. A completed scan still does not guarantee discovery of every shader.
