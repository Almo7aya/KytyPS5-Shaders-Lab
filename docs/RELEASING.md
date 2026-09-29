# Builds and releases

The **Build and release** workflow runs **only when manually dispatched**. Pushes,
pull requests and tags do not trigger it. All CI downloads, compilation and tests
run on GitHub-hosted runners, using synthetic fixtures, not game datasets.

Open **Actions → Build and release → Run workflow** and choose:

| Input | Default | Behavior |
| --- | --- | --- |
| `kyty_repository` | `KytyPS5/KytyPS5` | Public GitHub compiler repository in `owner/repository` form; for example `Almo7aya/kytyps5` |
| `kyty_ref` | Empty | Latest commit on that repository's default branch; optionally specify a branch, tag or full commit SHA |

The workflow resolves the chosen ref once, then checks out that exact commit and
its recursive submodules. The selected repository and resolved SHA appear in the
run summary and package's `BUILD-INFO.json`.

## Official builds versus forks

- **Official `KytyPS5/KytyPS5`:** after both build/test jobs and packaging succeed,
  automatically publish a GitHub Release with tag `build-<run-number>-<attempt>`.
  The tag identifies the Shader Lab commit; release notes also identify the exact
  Kyty compiler commit. Reruns create a new tag rather than overwrite a release.
- **Any other repository:** upload downloadable workflow artifacts only. The
  release job is skipped, regardless of the chosen compiler branch or tag.

The decision uses GitHub's canonical repository name, compared case-insensitively,
not the name of the selected branch. Only the isolated official-build release job
receives `contents: write`; compiler builds have read-only repository permissions.
No personal access token or additional secret is required. Select only trusted
forks: their build scripts execute on the temporary runner. Private repositories
are not supported.

## What runs

- Ubuntu builds and tests the standalone extraction/reporting application.
- Windows x64 builds the application and real Kyty compiler worker, then runs all
  `shader_lab_` tests, including synthetic shader translation and SPIR-V validation.
- Packaging smoke-tests the packaged CLI and compiler worker, gathers notices and
  corresponding sources, and generates SHA-256 checksums.
- The Windows bundle is uploaded as `windows-x64-release` and retained for 14 days.
  Official builds additionally attach the same assets to their GitHub Release.

Latest upstream or fork changes can introduce worker API incompatibilities. Such
builds fail visibly instead of falling back to an older compiler or publishing
untested binaries. Passing structural validation does not establish shader
semantic equivalence.

## Release contents

| Asset | Contents |
| --- | --- |
| `ps5-shader-lab-windows-x64.zip` | Scanner, compiler worker, required pthread DLL, profiles, documentation, dependency notices and `BUILD-INFO.json` |
| `ps5-shader-lab-sources.tar.gz` | Tracked project sources, exact Kyty checkout including recursive submodules, and CMake-fetched dependency sources |
| `ffmpeg-source-windows-x64.tar.gz` | Upstream's corresponding patched FFmpeg sources, build configuration and recipes, verified against upstream's checksums |
| `SHA256SUMS` | SHA-256 checksums for all three archives |

Extract the binary ZIP and keep its files together. A current Microsoft Visual
C++ x64 runtime may be required. No Qt launcher, Vulkan device, game files or
network connection is required to use the packaged application. Building and
packaging download public dependencies and sources; runtime remains offline.

The packaging script exports tracked files only. Ignored datasets, scans, reports,
caches and Git configuration are not included. Never commit private inputs or
generated reports. Third-party notices retain their source-relative paths in
`licenses/`. Keep the corresponding source assets alongside redistributed binaries.

## Source bundle

The source archive places `ps5-shader-lab` and `KytyPS5` as siblings. Consult the
README for build prerequisites and the source-linked build configuration.
CMake overrides `FETCHCONTENT_SOURCE_DIR_XBYAK`, `FETCHCONTENT_SOURCE_DIR_ZYDIS`,
`FETCHCONTENT_SOURCE_DIR_ZSTD` and `FETCHCONTENT_SOURCE_DIR_ZARCHIVE_SOURCE` can
point to their matching `dependencies/*-src` directories.

The archive deliberately excludes Git metadata. For Kyty versions whose FFmpeg
download selection depends on Git metadata, supply `FFMPEG_PREBUILT_DIR` explicitly
with libraries rebuilt using the separate FFmpeg source archive's recipe, or use
the matching upstream prebuilt package recorded by `ffmpeg_recipe_revision`.
Alternatively, check out the recorded project/compiler revisions with Git and
recursive submodules to use their normal dependency download behavior.

This is a source and provenance bundle, not a guarantee of bit-for-bit reproducible
host toolchains or an offline installer for MSVC, SDKs and other build tools.
