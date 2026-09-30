# Dependencies and provenance

No game binaries or extracted game shaders are part of this source repository.
Do not redistribute private datasets merely because their output directory is portable.

| Dependency | Use | License / source |
| --- | --- | --- |
| KytyPS5, user-selected checkout | Real shader compiler and worker link closure | GPL-2.0; original Kyty code also carries MIT notices; preserve upstream notices |
| nlohmann/json | Local manifests and worker protocol | MIT; https://github.com/nlohmann/json |
| xxHash | Emulator-compatible XXH3-64 code identity | BSD-2-Clause; https://github.com/Cyan4973/xxHash |
| zlib 1.3.2 | ZIP Deflate decoding and CRC32 | zlib license; https://github.com/madler/zlib |
| Zstandard, from selected Kyty build | Bounded frame decompression | BSD-3-Clause or GPL-2.0; https://github.com/facebook/zstd |
| SPIRV-Tools / SPIRV-Headers, from Kyty | Structural validation and disassembly | Apache-2.0; Khronos repositories |

The worker reuses Kyty's full standalone-test dependency closure. Its binary also links the
dependencies selected by that checkout (including fmt, SDL, FFmpeg, Tracy, Zydis and others).
Before distributing a worker binary, collect and comply with **all** notices from the exact
checkout/build, and provide corresponding source where required. This table is not a replacement
for those upstream distribution obligations. `libwinpthread-1.dll` retains its own license.

The extraction implementation is original C++ informed by format research and reference-tool
behavior. It does not embed or redistribute a third-party batch compiler.
ps5rs is a research reference, not a linked/runtime dependency. No Rust code is required.
