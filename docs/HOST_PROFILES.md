# Declared host feature profiles

Compiler profiles can include a `host` object describing a Vulkan 1.3 target.
This is supplied evidence, **not an automatic device query**. Use actual enabled
logical-device features/extensions and queried physical-device properties when available;
do not copy a GPU marketing specification or assume every supported feature is enabled.
The [example profile](../profiles/host-vulkan13.example.json) is illustrative, not a device capture.

`host.subgroup_size` reaches the real compiler's compute input, including paired-wave lowering.
The old root `host_subgroup_size` remains supported; supplying both requires equal values.
Without either, the existing 32-lane assumption remains. A subgroup number alone does not
constitute a feature profile. No subgroup size is enforced on a GPU by this compiler-only tool.

## Schema

Required: `schema: 1`, `api_version: "1.3"`, and power-of-two `subgroup_size` in 1..128.
The compiler still accepts only 32 or 64. Other values support assessment of queried devices,
including software Vulkan; they do not enable new Kyty lowering modes.
The assessment currently targets the same Vulkan 1.3 environment as SPIRV-Tools validation.
Other versions are rejected rather than silently interpreted as Vulkan 1.3.

Optional fields:

| Field | Meaning |
| --- | --- |
| `enabled_features` | Named Vulkan booleans enabled on the logical device |
| `enabled_extensions` | Complete list of enabled Vulkan extension names, such as `VK_KHR_fragment_shader_barycentric` |
| `properties` | Queried floating-point capability booleans for each bit width |
| `subgroup_operations` | Complete supported-operation list: `basic`, `vote`, `arithmetic`, `ballot`, `shuffle`, `shuffle_relative`, `clustered`, `quad` |
| `subgroup_stages` | Complete supported-stage list: `VS`, `TCS`, `TES`, `GS`, `PS`, `CS`, `Task`, `Mesh` |
| `limits` | Optional `maxComputeWorkGroupSize` three-axis array and `maxComputeWorkGroupInvocations` integer |

Recognized feature keys are `geometryShader`, `tessellationShader`, `shaderFloat64`,
`shaderInt64`, `shaderBufferInt64Atomics`, `shaderSharedInt64Atomics`, `shaderImageInt64Atomics`,
`shaderImageGatherExtended`, `shaderClipDistance`, `shaderCullDistance`, `sampleRateShading`,
`shaderStorageImageReadWithoutFormat`, `shaderStorageImageWriteWithoutFormat`,
`shaderOutputLayer`, `shaderOutputViewportIndex`, `bufferDeviceAddress`,
`workgroupMemoryExplicitLayout`, `meshShader`, `fragmentShaderBarycentric`,
`computeDerivativeGroupQuads`, and `computeDerivativeGroupLinear`.

Property keys combine one of `shaderDenormPreserveFloat`, `shaderDenormFlushToZeroFloat`,
`shaderSignedZeroInfNanPreserveFloat`, `shaderRoundingModeRTEFloat`, or
`shaderRoundingModeRTZFloat` with `16`, `32`, or `64`.

Missing values remain unknown. Explicit `false` means disabled/unsupported for that check;
an explicitly supplied list is treated as complete. Boolean strings, fractional/negative
integers, unknown field names, duplicate list entries, and conflicting subgroup declarations
are rejected. Feature support is never inferred from the presence of an extension alone.

## Results

After successful structural validation, the worker inventories the emitted module's capabilities,
extensions, entry stage, float-control execution modes and actual local workgroup dimensions.
It writes `host-assessment.json`, also returned as `details.host_assessment`. The HTML report's
Context tab shows the assessment and Artifacts links to the record. The input profile is part of
the existing case/cache identity, so different declared hosts produce separate cached attempts.

| Assessment | Meaning |
| --- | --- |
| `not_supplied` | No host object was supplied; required items are still inventoried |
| `unsupported` | At least one modeled requirement contradicts the declarations |
| `unknown` | No modeled contradiction, but at least one required item is missing or unmodeled |
| `satisfied` | All inventoried, modeled declaration checks succeeded |
| `not_assessed` | Structural validation failed, so host assessment was not performed |

Every assessment says `runtime_compatibility: not_established` and
`scope: declared_spirv_requirements_only` (except the short invalid-module record).
A valid shader can have an unsupported host assessment. The compiler outcome remains
`spirv_valid_under_profile`; it does not become a semantic failure or a successful GPU run.
Neither `satisfied` nor a supplied profile authenticates the underlying device.

Unknown capabilities, extensions and execution modes are visible unknowns. Vulkan 1.3 core
promotion is recognized for the emitted float-controls and physical-storage-buffer SPIR-V
extensions; the latter still requires `bufferDeviceAddress`. Required float widths and
subgroup stages are checked separately. Compute limits use the **emitted** local size, which
can differ from the guest workgroup when paired-wave lowering is selected.

The checker does not resolve atomic storage-class usage; `Int64Atomics` remains unknown.
Storage-image access without format remains unknown when it needs per-format evidence rather
than an enabled global feature. Portability-subset restrictions and quad operations outside
compute/fragment likewise remain unknown. Resource layouts/formats, float-control independence,
pipeline-wide limits, actual subgroup-size enforcement and driver execution still need the replay
backend's checks. This is a prerequisite for GPU replay, not its replacement.

## Verification and references

Synthetic inventory tests cover missing/false declarations, unknown extensions/capabilities,
subgroup operation/stage mismatches, float widths, dimensions, malformed types and instruction
envelopes. Real-worker fixtures compile the same wave64 shader with host32 and host64 profiles,
check distinct emitted modules, preserve structural success when a host property is disabled,
and reject conflicting subgroup state. Run evidence comes from CI at the relevant commit.

The inventory parser does not implement a SPIR-V validator: SPIRV-Tools validates real modules
before the worker performs this narrower assessment.

Primary specifications:

- [Khronos Vulkan environment for SPIR-V](https://docs.vulkan.org/spec/latest/appendices/spirvenv.html)
  supplies capability/feature mappings and width-specific floating-point requirements.
- [Khronos subgroup guide](https://docs.vulkan.org/guide/latest/subgroups.html) explains the
  distinct stage and operation properties used by subgroup checks.
- [Khronos SPIR-V headers](https://github.com/KhronosGroup/SPIRV-Headers/blob/main/include/spirv/unified1/spirv.hpp)
  define the numeric instruction, capability and execution-mode identifiers.
