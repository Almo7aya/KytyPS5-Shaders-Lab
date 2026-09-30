# Offline execution fixture protocol (version 1)

`execute-fixture` is an isolated runner and reference comparator for an **explicitly selected,
trusted execution backend**. It is separate from the compiler-worker protocol. The shipped Kyty
compiler worker does not implement execution. A separate [CPU integer model](CPU_REFERENCE.md)
implements a documented ISA subset; no Vulkan replay backend or authenticated hardware reference
corpus is supplied yet. The protocol test double remains separate from that real CPU backend.

## Preparing a fixture

Use synthetic shaders or captures you are authorized to analyze. Put the fixture, shader,
compiler profile and initial resource files in one directory tree. All resource paths must be
relative and resolve inside that tree. The following is a **fictional shape example**, not a
runnable fixture; replace hash placeholders with actual SHA-256 values and supply real files.

```json
{
  "schema": 1,
  "kind": "shader_lab_execution_fixture",
  "id": "synthetic-buffer-operation-v1",
  "shader": {
    "header": {"file": "header.bin", "sha256": "HEADER_SHA256"},
    "code": {"file": "code.bin", "sha256": "CODE_SHA256"}
  },
  "profile": {"file": "profile.json", "sha256": "PROFILE_FILE_SHA256"},
  "execution": {
    "stage": "CS",
    "wave_size": 32,
    "exec_mask": "00000000ffffffff",
    "workgroup_size": [32, 1, 1],
    "dispatch_size": [1, 1, 1]
  },
  "resources": {
    "result": {
      "file": "initial-result.bin",
      "sha256": "INITIAL_RESULT_SHA256",
      "access": "read_write",
      "kind": "buffer",
      "type": "u32",
      "binding": [0, 0],
      "guest_address": "0000000000001000"
    }
  }
}
```

Every resource includes **initial bytes**, even `write_only` outputs. This makes unwritten output
bytes observable and avoids implicitly invented zero initialization. `access` is `read_only`,
`read_write` or `write_only`; the latter two form the exact expected output set. `binding` is
`[descriptor_set, binding]`, bounded to set 0–31 and binding 0–65535, with no duplicates. These
are fixture declarations, not automatic matching to emitted Kyty resource bindings. The backend
must resolve and validate the real bindings before executing, or return `unsupported`.

`guest_address` is a nonzero 16-digit lowercase hexadecimal byte address. Resource address ranges
must not overflow or overlap. The runner never dereferences these addresses. A backend needs its
own checked address/resource mapping; pointers into capture files must not become host pointers.
Aliased resources and descriptor arrays require a future protocol extension rather than guessed
semantics.

Types are `u8`, `u32`, `i32` and `f32`, stored little-endian. An `image` resource also needs
`shape: [width, height, depth, channels]` with tightly packed row-major scalar components. The
protocol does not infer tiled/compressed formats, image views, sampling state or format conversion.
Unsupported image semantics must be reported by the backend rather than treated as linear buffers.

Version 1 accepts compute AGC header/code pairs only. Workgroup dimensions are guest thread counts;
dispatch dimensions are workgroup counts. Workgroups have at most 1,024 invocations; total dispatch
work is limited to 16,777,216 invocations. Guest wave size is 32 or 64, independently of a host
subgroup. `exec_mask` has exactly 16 lowercase hex digits (the high eight must be zero for wave32).
It specifies the initial EXEC mask for each guest wave. A backend that cannot reproduce partial
EXEC, multiple waves, resource types or any other supplied state must reject that fixture explicitly.
The runner cannot enforce that a backend implements guest execution semantics correctly.

The fixture/profile/response JSON limit is 1 MiB each. There are 1–128 resources and at least one
output. Header, code, profile and initial resource bytes share a 64 MiB budget. Reference outputs
share another 64 MiB budget; observed outputs must have the same exact resource sizes. Unknown
fields, coerced numeric types, invalid hashes, malformed layouts and missing state are rejected.

## Binding independent reference evidence

First obtain the actual fixture identity without executing anything:

```powershell
shader-lab fixture-info --fixture fixtures/example/fixture.json --output fixture-identity.json
```

This validates and reads all source bytes. The result includes header/code hashes, the supplied
fixture ID, wave/EXEC, and two canonical JSON hashes. `profile_sha256` hashes the compact profile
with recursively sorted object keys; it is not the raw profile file hash. `input_sha256` hashes
`{"execution": EXECUTION, "resources": METADATA}` in the same canonical form. Resource metadata
is the fixture entry without `file`, plus its verified `bytes` length. Object order and relocation
do not change these hashes; resource values, layouts, bindings and guest addresses do.

Use that complete object as `fixture` in the existing schema-1 reference-output record described
in the README. Obtain expected bytes from an independently justified model or hardware capture;
do not generate the oracle from the Kyty translation being evaluated. Record source identifier,
method, actual output hashes/layouts and exact integer or explicit float32 comparison policies.
All output resources, including unchanged portions, must be present. A reference containing NaNs
under the `reject` policy is rejected before execution because it cannot satisfy its own rules.

The runner verifies hashes and consistency; it does **not** authenticate the source, establish
model independence, or certify that claimed hardware execution happened. Review provenance and
the reference implementation separately. A trustworthy reference corpus is still required for
meaningful shader conformance evidence.

## Running a backend

```powershell
shader-lab execute-fixture --fixture fixtures/example/fixture.json --reference fixtures/example/reference.json --worker backends/reference-backend.exe --output runs/example-attempt --timeout-ms 30000
```

The output must be new and disjoint from the fixture tree, reference tree and worker file. Every
attempt is fresh: no cache or resume. CPU is the default backend contract. To intentionally select
a GPU backend, use **both** `--backend-kind gpu` and `--allow-gpu`. Missing opt-in is rejected before
creating output or starting a process. The runner does not discover or silently substitute a GPU.
Selecting a backend means trusting that executable to honor its declared kind; this flag is an
application contract, not a driver-access security boundary.

The parent copies verified shader/profile/resources into `inputs/`, copies the reference record
and outputs into `reference/`, and writes `request.json`. It runs exactly:

```text
SELECTED_WORKER --execute-fixture ABSOLUTE_REQUEST_JSON
```

No shell, game launcher or network service is invoked. The request is schema 1 with kind
`shader_lab_execution_request`, the complete `fixture` identity, `execution`, absolute `header`,
`code`, `profile` paths, normalized `resources` (including absolute initial-file paths), absolute
`output` directory, `backend_kind` and `allow_gpu`. Expected bytes and tolerances are not included
in the request. The backend must use absolute paths, not assume its working directory equals
`output`; Windows can use a shorter ancestor for deep paths.

A backend must:

1. Validate request schema, all relevant shader/profile/state constraints and resource bindings.
2. Honor CPU/GPU selection; require GPU opt-in before creating a device or submitting work.
3. Read snapshots without changing them, initialize outputs from supplied initial bytes, and
   execute only the supported fixture. Refuse unsupported instructions/state/features explicitly.
4. Synchronize and read back complete output buffers/images before returning `completed`.
5. Write relative output files inside its `output` directory and `response.json` last.

Example response shape:

```json
{
  "schema": 1,
  "kind": "shader_lab_execution_response",
  "request_sha256": "SHA256_OF_EXACT_REQUEST_FILE_BYTES",
  "status": "completed",
  "backend": {
    "kind": "cpu",
    "identifier": "independent-model-and-version",
    "method": "Describe the executed subset and observation method"
  },
  "outputs": {
    "result": {"file": "result.bin", "sha256": "ACTUAL_OUTPUT_SHA256"}
  }
}
```

An unsupported case returns `status: "unsupported"` with a nonempty `reason`, the same identity
and backend fields, and may omit `outputs`. Backend identifiers/methods are self-reported, not
authenticated. The parent retains the selected executable's hash separately. Worker binaries or
commands are never chosen by a fixture file. Shared-library/driver dependencies are not yet hashed.

## Results, isolation and limitations

The parent checks the request hash, backend kind, exact output set, content hashes and sizes.
It re-hashes its input/reference snapshots and selected executable after a successful process
exit; modified evidence cannot yield a match. Output layout and comparison policy come from the
validated fixture/reference, never from backend-supplied overrides. Outputs are copied to
`observed/`, then compared with the existing comparator. Copies support later offline re-comparison.

`execution.json` records phase, deadline, worker/request/response hashes, exit/timing/working
directory, backend declaration and comparison. `comparison.json`, `reference/record.json`,
`observed/record.json`, `request.json`, initial snapshots and `backend/worker.log` retain evidence.
Do not publish these generated artifacts without checking proprietary data and private paths.

| Status | Meaning |
| --- | --- |
| `match` / `mismatch` | These outputs agree/disagree under the reference policy; not general shader correctness. |
| `unsupported` | Backend explicitly refused the fixture; no comparison verdict. |
| `backend_timeout` / `backend_error` | Deadline or nonzero child exit; not a semantic mismatch. |
| `execution_error` | Snapshot, launch, protocol, integrity, layout or comparison error; inspect phase/reason. |

CLI exit 0 requires `match`; 4 means an unsuccessful attempt; 2 means invalid setup before a run
or an unrecoverable runner error. Incomplete evidence remains on disk and is never silently deleted.

The child has the existing process deadline and 2 GiB memory limit. This is **not a filesystem,
network or GPU security sandbox**: only run trusted backends. There is no log/disk quota, device
reset mechanism or guarantee that killing a process can recover a stuck GPU driver. Use a
disposable isolated environment for GPU experiments. No GPU device is created by the shipped
compiler worker, CPU model or protocol tests. Graphics/GPU execution, broader independently
validated ISA coverage and authenticated reference data remain required work; protocol fixtures
cannot substitute for them.
