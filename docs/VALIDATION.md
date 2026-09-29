# Validation record

Validated on Windows x64 on 2026-09-29, using clang-cl, Ninja and the learning
repository's clean KytyPS5 checkout at
`2650478d92c394c092b36ee129962060c623ca16`.
These are offline extraction/compiler checks, not GPU or guest conformance tests.

## Automated checks

The source-linked Release build passed all three `shader_lab_` CTest tests:

- `shader_lab_unit`: 517 checks, including malformed/truncated inputs, ELF/SELF/Zstandard
  fixtures, hashes, provenance, resume, reports, subprocess errors and timeout isolation.
- `shader_lab_help`: CLI startup and help.
- `shader_lab_real_kyty`: the fixture checks plus a synthetic compute shader compiled
  through the real Kyty worker and validated with SPIRV-Tools (518 checks total).

The integration test repeats fixture checks; these counts are not additive unique tests.
Run the build and CTest commands in the README to reproduce against your checkout.
The lightweight build (without the Kyty worker or Zstandard adapter) was also rebuilt
successfully and passed its two CTest tests.

## Real input campaigns

The Dreaming Sarah directory under `D:\PS5_Games` was scanned recursively: 816 files,
119 unique header-and-code cases. Clear SELF reconstruction also recovered 42 cases
from its executable; these were already represented by other origins in the full-game
dataset, so they did not increase the unique total.

The supplied `shaders_wolf` reference directory produced 1,656 cases from 3,313 files.
The final worker binary processed both complete extracted datasets:

| Outcome | Dreaming Sarah | Supplied wolf corpus |
| --- | ---: | ---: |
| `spirv_valid_under_profile` | 111 | 337 |
| `missing_stage_context` | 8 | 181 |
| `resource_context_unresolved` | 0 | 824 |
| `unsupported_instruction` | 0 | 158 |
| `unsupported_ray_tracing` | 0 | 8 |
| `worker_crash_or_error` | 0 | 148 |
| Total | 119 | 1,656 |

Compiler failures remained isolated to their subprocesses; neither campaign was
aborted by an individual worker failure. Neither full campaign recorded a timeout.
Failures under probe assumptions are reproduction candidates, not confirmed game bugs.
Likewise, a valid module does not establish semantic equivalence.

Local, Git-ignored evidence:

- `runs/dreaming-sarah/results.json` and `report.html`
- `runs/wolf-full/results.json`, `report.html` and `failures.json`
- Per-case worker logs, phase records, profiles and available compiler artifacts
- `build-kyty/Testing/Temporary/LastTest.log`

The HTML report generation is exercised by automated tests; the real reports have
not received a visual browser review. Private shader bytes and generated artifacts
are not included in the project source or committed to Git.

## Not established by this validation

The entire `D:\PS5_Games` library has **not** been scanned. The README provides the
full-library command. Visiting every file does not guarantee extraction from every
container or recovery of dynamically generated shaders.

No game was launched, no GPU shader was executed, and no reference output comparison
was performed. Full PM4/state replay, all graphics stages, arbitrary archives and
semantic correctness remain outside the current implementation's coverage. Generated
SPIR-V is not a directly importable emulator pipeline cache. Linux paths are unverified.

The learning repository's tracked files and the selected KytyPS5 source checkout were
left unchanged. This project has its own Git repository; no commit or push was made.
