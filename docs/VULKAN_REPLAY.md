# Experimental Vulkan buffer-compute replay

`shader-vulkan-replay` is an opt-in execution backend, separate from the compiler worker.
It compiles the actual fixture shader through the configured Kyty library, validates the emitted
SPIR-V, consumes the [compiler layout](COMPILER_LAYOUT.md), dispatches Vulkan compute work and
returns complete output bytes to the existing independent-reference comparator.
It does not substitute a hand-written SPIR-V kernel or inspect expected outputs.

Build with `SHADER_LAB_BUILD_KYTY_WORKER=ON` and include the `shader-vulkan-replay` target.
The backend uses cached upstream Vulkan headers and dynamically loads the system Vulkan loader;
it never downloads drivers. Keep the compiler runtime DLL beside the executable on Windows.
On Linux, the compiler-worker CMake target is `shader_cfg_tests` and its executable is
`shader-kyty-worker`; the Windows-only convenience target avoids a suffix-free Ninja name collision.

```powershell
.\build-kyty\shader-lab.exe execute-fixture --fixture fixtures/store/fixture.json --reference fixtures/store/reference.json --worker .\build-kyty\shader-vulkan-replay.exe --output runs/store-vulkan --backend-kind gpu --allow-gpu --timeout-ms 60000
```

These are illustrative synthetic paths. Supply a real version-1 [execution fixture](EXECUTION.md)
and independent reference. Outputs are local/private runtime artifacts, not repository examples.
The application is fully offline. The parent and backend both enforce explicit GPU opt-in.
Even a software Vulkan device uses the GPU protocol category and requires that opt-in.

## Implemented execution path

1. Validate copied fixture identity and immutable initial snapshots before compiler/device work.
2. Compile once in the isolated child, retain `backend/compilation` and hash-check the exported layout.
3. Check the explicit compute profile, full initial EXEC, matching workgroup/wave state and supported bindings.
4. Query a Vulkan 1.3 compute device, its features, float controls, subgroup support and relevant limits.
5. Allocate coherent host-visible buffers initialized from every fixture resource. Resolve actual
   compiler resources by guest address, not the fixture's arbitrary set/binding IDs. Preserve aliases
   referring to ranges within the same captured buffer and the compiler's ordered descriptor array.
6. Bind the compiled module's storage buffers and internal flattened-table/shader-data buffers.
   Each captured buffer has one host allocation. Interior descriptor offsets are aligned down to
   the queried storage-buffer alignment; the remaining byte adjustment is packed into Kyty's
   shader-data slot. The descriptor range includes that prefix. Adjustments above 255 or ranges
   exceeding the capture/device limit are refused. This preserves aliasing without copying slices.
   Populate live user-data words and the actual push-constant offset when used.
7. Submit the requested workgroup counts, synchronize shader writes to host reads and wait for a fence.
   Read back all writable resource bytes, including unwritten sentinels, without modifying input snapshots.

The compiler lowering currently accepts host subgroup sizes 32/64. Subgroup-dependent modules
must have that physical width, using required subgroup-size control when available. A structurally
validated module with no subgroup capabilities can execute at the device's native width; no guest
wave state is changed and no subgroup-dependent module is silently remapped. The trace records
both the compiler assumption and selected device width. Unknown module requirements are refused.

## Evidence and limits

`backend/replay-trace.json` records compiler/input identities, selected device and driver IDs,
queried properties and enabled modeled features, required-size selection, dispatch dimensions,
resource mapping and uploaded shader-data words. The parent retains actual output hashes and
compares under the reference's policy. `match` means only that this fixture's observable outputs
matched this supplied reference. Neither replay nor structural validity proves general correctness.
Device names and reference provenance are not cryptographic authentication.

Current restrictions are explicit: compute `context_snapshot` profiles, full initial EXEC,
no LDS/scratch allocation, buffer resources wholly contained in individual captures, and descriptor ranges fully
backed by snapshots. Images/samplers, image aliases, GDS, BDA page tables/fault recovery, graphics,
partial EXEC and captured-compute dispatch mode require additional implementation. The backend
does not reconstruct missing memory, infer game state, authenticate hardware captures, prevent
shader data races or provide a general driver-conformance verifier. Some Vulkan capabilities
remain unknown to the requirement checker and are refused even if a driver could support them.

Only system loader/device initialization occurs after the explicit gate. Vulkan call failures
are backend errors, not shader mismatches. Fence waiting is bounded to ten seconds; a timeout or error
terminates the worker without destroying pending resources. The outer process deadline still
applies to compilation, pipeline creation and execution. **Process termination is not a GPU reset
or security sandbox.** Drivers can hang outside process control. Use disposable environments for
unknown shaders; no disk/GPU allocation quota or device-recovery guarantee is provided.

## Verification route

The ordinary test suite runs only no-device opt-in guards for this backend. Explicit
`shader-lab-tests --vulkan-worker ABSOLUTE_WORKER_PATH` executes real Vulkan fixtures; do not run
that mode without intending device work. CI runs it on a hosted Mesa software device with Vulkan
validation enabled, separately from Windows compilation/packaging. The test covers wave32/wave64
indexed stores, interior buffer descriptors, unchanged surrounding bytes, an intentionally wrong reference and refusal of partial
EXEC. Successful software execution is not physical-GPU or PS5 hardware validation. CI results,
not the existence of these tests, determine whether a revision has passed.
