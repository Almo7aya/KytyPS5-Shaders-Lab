# Independent RDNA2 integer reference backend

`shader-cpu-reference` executes a bounded subset of actual shader instructions on the CPU.
It has its own decoder/interpreter and does not include or link Kyty's decoder, translator, IR,
SPIR-V emitter or Vulkan runtime. It shares only Shader Lab's file, hash and fixture-protocol
utilities. This is a research reference model, **not a hardware-certified oracle or full emulator**.

Use it as the explicitly selected CPU backend in [the execution harness](EXECUTION.md):

```powershell
shader-lab execute-fixture --fixture fixtures/example/fixture.json --reference fixtures/example/reference.json --worker shader-cpu-reference.exe --output runs/cpu-attempt
```

The reference record must contain independently justified expected bytes. The model does not
silently create expected results from its own execution and compare them with itself. To use
model-produced bytes as evidence for another backend, preserve the model binary/request/output
identities and document how its supported semantics and expected results were reviewed.
Hardware capture remains stronger independent evidence where available.

## Supported execution contract

Only explicit `context_snapshot` compute profiles are accepted. Required root fields are
`schema: 1`, `mode: "context_snapshot"`, `stage: "CS"`, `wave_size`, `user_data`, and `compute`.
No header-derived defaults or captured-compute preparation are inferred. User data supplies 0–16
direct SGPR values; unsupplied registers are undefined, never invented as zero. `compute` requires:

- `threads`: exactly the fixture's three workgroup dimensions.
- `group_id`: three booleans controlling supplied workgroup X/Y/Z SGPRs.
- `workgroup_register`: exactly the user-data count, so enabled group IDs follow contiguously.
- `thread_ids_num`: 1–3, for separate X/Y/Z local thread IDs starting at VGPR0.
- `tg_size_en: false`, `lds_size_dwords: 0`, `scratch_size_dwords: 0`.
- `float_mode`: an explicit byte value, recorded but irrelevant to this integer-only subset.

This is a declared compute-entry contract, not proof of a game's actual launch state. Unknown
profile fields and unsupported state are refused. The generic fixture runner first verifies
header/code/profile/resource hashes; the backend verifies the copied request again before use.

The model supports wave32/wave64, multidimensional workgroups and dispatches, and a fixed initial
EXEC mask per wave. A partial final wave also excludes lanes outside the workgroup. Only enabled
lanes execute vector instructions. SGPR group IDs and VGPR local IDs are initialized from the
declared geometry; other VGPRs remain undefined until written. There is no scalar arithmetic,
branching, dynamic EXEC, VCC, DPP/SDWA, special-register or cross-lane implementation yet.

Supported ordinary e32 vector operations are `V_MOV_B32`; signed/unsigned min/max; logical and
arithmetic reverse shifts; AND/OR/XOR/XNOR; and unsigned no-carry add/subtract/reverse-subtract.
Sources can be defined SGPRs/VGPRs, integer inline constants or one literal dword. Arithmetic is
32-bit, with bit-pattern comparison for outputs. Floating inline constants and floating operations
are explicitly unsupported rather than approximated with host arithmetic.

Memory support is aligned `GLOBAL_LOAD_DWORD` / `GLOBAL_STORE_DWORD` with a scalar base plus vector
offset or a vector address pair, and a signed immediate offset. Only declared `u32`/`i32` buffers
are supported. Cache/other modifiers, flat/scratch modes, images, descriptors, atomics and vector
memory widths other than one dword are rejected. Addresses must resolve wholly into supplied
resources with compatible access permissions; guest addresses never become host pointers.

`S_NOP`, `S_WAITCNT 0` and a final `S_ENDPGM` are supported. A loaded VGPR stays pending until
the explicit wait; reading or overwriting it earlier is rejected. Loads are modeled from stable
buffer bytes, not host scheduling. Cross-lane/workgroup conflicts involving a write, repeated
stores to the same dword, and store-to-load feedback are rejected because the model does not
provide a general memory-order oracle. It does not simulate cache timing or asynchronous hardware.

There are at most 4,096 code dwords and 16,777,216 lane-instructions, in addition to the harness's
input, resource and process limits. Unknown encodings, incomplete instructions, missing termination,
trailing code, undefined registers, pending-load hazards, overflow and out-of-range memory produce
`unsupported` with a reason. Partial results never become a completed reference execution.

## Evidence and verification

Completed runs retain `backend/model-trace.json` with fixture identity, decoded instruction count,
executed vector lane-instruction count, load/store counts and explicit hardware-conformance limits.
The ordinary harness retains source snapshots, output records, comparisons and executable identity.
The trace is diagnostic; it cannot establish model independence or hardware fidelity by assertion.

`tests/cpu_tests.cpp` runs the real backend as a subprocess through the harness. Fixed expected
bit patterns cover wraparound, sign boundaries, shifts and bitwise operations. Wave32/wave64 and
masked/empty EXEC tests preserve untouched resource bytes. Negative fixtures exercise unsupported
floating operations, truncation, missing waits, undefined values, bounds and races.

`tests/integer-buffer.s` describes the test kernel. Linux CI assembles it with LLVM's GFX10.3
assembler and compares the exact encoding; C++ tests also compare their input bytes with that
checked-in encoding. Assembly agreement checks instruction identity, not execution semantics.
Golden values are separately specified constants, not output generated by Kyty or by the model.
New tests remain unverified until Actions succeeds at their commit.

Primary references used for this separate implementation:

- [AMD RDNA2 ISA guide announcement and manual link](https://gpuopen.com/news/rdna2-isa-available/).
- [LLVM 20.1 compute initial-register contract](https://releases.llvm.org/20.1.0/docs/AMDGPUUsage.html#initial-kernel-execution-state).
- [LLVM 20.1.8 global-memory bitfields](https://github.com/llvm/llvm-project/blob/llvmorg-20.1.8/llvm/lib/Target/AMDGPU/FLATInstructions.td).
- [LLVM global-memory encoding tests](https://github.com/llvm/llvm-project/blob/llvmorg-20.1.8/llvm/test/MC/AMDGPU/flat-global.s),
  [VOP1 tests](https://github.com/llvm/llvm-project/blob/llvmorg-20.1.8/llvm/test/MC/AMDGPU/gfx10_asm_vop1.s),
  [VOP2 tests](https://github.com/llvm/llvm-project/blob/llvmorg-20.1.8/llvm/test/MC/AMDGPU/gfx10_asm_vop2.s).

The model is intentionally not a substitute for GPU replay, graphics fixtures, a larger independently
validated ISA model or trusted hardware captures. Those remain part of the development milestones.
