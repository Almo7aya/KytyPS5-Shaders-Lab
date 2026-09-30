# Compiler resource-layout artifact

After SPIR-V emission and validation, the worker attempts to export `compiler-layout.json`.
This is a schema-1 snapshot of the actual post-specialization Kyty program and its materialized
resource descriptors, not an inferred layout from fixture binding IDs. It is a prerequisite
for a replay backend, not a runnable pipeline, runtime cache or correctness verdict.

`response.json.compiler_layout` records `exported`, the artifact name and its SHA-256, or
`unavailable` with a reason. Export failure does not overwrite the independent compiler or
SPIR-V validation result. The HTML report exposes this artifact alongside the emitted module.

## Identity and interpretation

`identity` contains the shader ID, configured upstream revision, compiler provenance, header,
code and SPIR-V hashes, stage, and `profile_json_sha256`. The latter hashes the UTF-8 bytes of
the effective profile serialized by nlohmann JSON's compact `dump()`, not the original profile
file's whitespace. `spirv_valid` records structural validation independently. Hashes bind
artifacts together; they do not authenticate capture provenance or reference correctness.

All upstream numeric enums are revision-specific. Consumers must check schema and compiler
identity and reject layouts they do not understand. The JSON is a private-to-public boundary;
no mutable Kyty IR objects escape the compiler library.

| Field | Meaning |
| --- | --- |
| `descriptors` | Actual set, stage-adjusted native binding, upstream kind, descriptor type/count and ordered resource indices. Singleton internal bindings have an empty index list and count one. |
| `buffers` | Materialized descriptor words, guest address, descriptor size, stride, specialization metadata and read/write/atomic flags. Array index is the resource index, not a fixture binding ID. |
| `images`, `samplers` | Materialized words and finalized compiler metadata, including image class, dimensions, mip mode, swizzle, indirect mapping and sampler specialization. These are not host image layouts or upload plans. |
| `shader_data` | Actual data location, live SGPR mapping and values, push-data start, packed-offset region and native push-constant block size. |
| `buffer_offsets` | One slot per live buffer array element: resource index, shader-data word, bit offset and eight-bit width. Runtime-selected byte offsets remain null. |
| `flattened_srt` | Materialized flattened resource-table words in upstream order. |

Image descriptor arrays retain repeated resource indices: dynamic storage-image mip views
must not be deduplicated. Resource vectors can contain entries not used by the final module;
only the exported descriptor lists specify the live binding arrays. Buffer descriptor size
is the guest descriptor's extent, not proof that that many bytes were captured or allocated.
Image aliases and BDA/internal bindings need additional runtime machinery.

Shader-data `words` is a template, not upload-ready bytes. Actual user-data values are copied
from the snapshot using the program's user-data base. Packed buffer-offset words are null
until runtime allocation establishes offsets. A backend must preserve the live array order,
validate the eight-bit offset constraint and populate every offset before upload. It must
not treat null as zero. Push offsets are in dwords; convert explicitly to byte offsets.

## Replay still requires

1. Match the header, code, effective profile, module and compiler identity to an execution fixture.
2. Reject invalid SPIR-V or unsupported host requirements and query the actual selected device.
3. Resolve guest ranges against bounded fixture snapshots, including access permissions and aliases.
4. Allocate resources and views, fill runtime offsets/internal data and construct the exact layout.
5. Establish execution state, synchronization and dispatch dimensions, then compare captured outputs
   against independent reference data under the fixture's comparison policy.

The current artifact always reports `runtime_bindings: not_created` and
`semantic_correctness: not_tested`. It does not implement any of those execution steps.
BDA page tables/fault handling, GDS, image tiling/mip uploads, driver compatibility, graphics
partners and full replay remain open. Uniform-fill optimizations are not an execution result.

Exports are bounded: 64 buffers/images, 32 samplers, 64 descriptor groups, 4096 elements per
group/indirect list, 256 user-data slots and one million flattened-table words. Mismatched
snapshot widths/counts or missing live user data fail the auxiliary export explicitly instead
of fabricating data. Generated artifacts can contain supplied addresses/descriptors; keep
private runs out of public documentation and source commits.

Real-worker CI fixtures exercise no-resource code, a buffer store with live user data, two
ordered buffer resources and a relocated descriptor whose SPIR-V remains unchanged. These
are compiler/layout checks, not GPU execution tests or exhaustive image-layout coverage.
