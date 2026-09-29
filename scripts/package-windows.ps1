# Build-time packaging only. The application and generated reports remain offline.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BuildDirectory,
    [Parameter(Mandatory)][string]$KytyRoot,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$KytyRepository = 'KytyPS5/KytyPS5'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$projectRoot = (Resolve-Path "$PSScriptRoot/..").Path
$buildRoot = (Resolve-Path $BuildDirectory).Path
$kytyPath = (Resolve-Path $KytyRoot).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path $output) { throw 'OutputDirectory must not already exist; choose a fresh directory.' }
$stage = Join-Path $buildRoot ("package-" + [guid]::NewGuid().ToString('N'))
$binary = Join-Path $stage 'ps5-shader-lab-windows-x64'
$sources = Join-Path $stage 'sources'
New-Item -ItemType Directory -Path $binary, $sources, $output | Out-Null

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed ($LASTEXITCODE)" }
}

# Export only version-controlled files, including nested submodules. Never copy
# developer datasets, reports, Git credentials, caches or arbitrary worktree files.
function Export-Tracked([string]$Repository, [string]$Destination) {
    $files = @(& git -C $Repository -c core.quotepath=false ls-files --recurse-submodules)
    if ($LASTEXITCODE -ne 0 -or $files.Count -eq 0) { throw "Cannot enumerate sources: $Repository" }
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    $list = Join-Path $stage ([guid]::NewGuid().ToString('N') + '.txt')
    [IO.File]::WriteAllLines($list, $files, [Text.UTF8Encoding]::new($false))
    $archive = "$list.tar"
    Invoke-Checked tar @('-cf', $archive, '-C', $Repository, '-T', $list)
    Invoke-Checked tar @('-xf', $archive, '-C', $Destination)
}

foreach ($file in @('shader-lab.exe', 'shader-kyty-worker.exe', 'libwinpthread-1.dll')) {
    Copy-Item -LiteralPath (Join-Path $buildRoot $file) -Destination $binary
}
foreach ($file in @('LICENSE', 'README.md', 'THIRD_PARTY.md', 'docs', 'profiles')) {
    Copy-Item -LiteralPath (Join-Path $projectRoot $file) -Destination $binary -Recurse
}
Invoke-Checked (Join-Path $binary 'shader-lab.exe') @('--help')
Invoke-Checked (Join-Path $buildRoot 'shader-lab-tests.exe') @('--real-worker', (Join-Path $binary 'shader-kyty-worker.exe'))

Export-Tracked $projectRoot (Join-Path $sources 'ps5-shader-lab')
Export-Tracked $kytyPath (Join-Path $sources 'KytyPS5')
$dependencyRevisions = [ordered]@{}
foreach ($dependency in Get-ChildItem -LiteralPath "$buildRoot/_deps" -Directory -Filter '*-src') {
    Export-Tracked $dependency.FullName (Join-Path $sources "dependencies/$($dependency.Name)")
    $revision = & git -C $dependency.FullName rev-parse HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Cannot determine dependency revision' }
    $dependencyRevisions[$dependency.Name] = $revision
}

# Preserve notices with their source-relative paths, including vendored libraries.
$noticeRoot = Join-Path $binary 'licenses'
foreach ($file in Get-ChildItem -LiteralPath $sources -Recurse -File) {
    if ($file.Name -match '^(LICENSE|LICENCE|COPYING|COPYRIGHT|NOTICE|AUTHORS)([._-].*)?$') {
        $relative = [IO.Path]::GetRelativePath($sources, $file.FullName)
        $destination = Join-Path $noticeRoot $relative
        New-Item -ItemType Directory -Path (Split-Path $destination) -Force | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $destination
    }
}

$ffmpegRevision = & git -C "$kytyPath/3rdparty/ffmpeg-core" rev-parse --short=12 HEAD
if ($LASTEXITCODE -ne 0) { throw 'Cannot determine FFmpeg recipe revision' }
$ffmpegPackage = Join-Path $buildRoot "externals/ffmpeg-$ffmpegRevision"
Copy-Item -LiteralPath "$ffmpegPackage/share/ffmpeg" -Destination "$noticeRoot/ffmpeg" -Recurse
$ffmpegSource = 'ffmpeg-source-windows-x64.tar.gz'
$ffmpegUrl = "https://github.com/KytyPS5/ext-ffmpeg-core/releases/download/$ffmpegRevision"
Invoke-WebRequest "$ffmpegUrl/$ffmpegSource" -OutFile "$output/$ffmpegSource"
$checksums = (Invoke-WebRequest "$ffmpegUrl/SHA256SUMS").Content
if ($checksums -is [byte[]]) { $checksums = [Text.Encoding]::UTF8.GetString($checksums) }
$expected = [regex]::Match($checksums, '(?m)^([a-fA-F0-9]{64})\s+\*?ffmpeg-source-windows-x64\.tar\.gz\s*$')
if (-not $expected.Success -or
    (Get-FileHash "$output/$ffmpegSource" -Algorithm SHA256).Hash -ne $expected.Groups[1].Value) {
    throw 'FFmpeg corresponding-source checksum mismatch'
}

$projectRevision = & git -C $projectRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'Cannot determine project revision' }
$kytyRevision = & git -C $kytyPath rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'Cannot determine Kyty revision' }
$metadata = [ordered]@{
    project_revision = $projectRevision
    kyty_repository = $KytyRepository
    kyty_revision = $kytyRevision
    ffmpeg_recipe_revision = $ffmpegRevision
    dependency_revisions = $dependencyRevisions
    platform = 'windows-x64'
    compiler = (& clang-cl --version | Select-Object -First 1)
    cmake = (& cmake --version | Select-Object -First 1)
    configuration = 'Release'
    semantic_correctness = 'not_tested'
}
$metadata | ConvertTo-Json -Depth 5 | Set-Content "$binary/BUILD-INFO.json" -Encoding utf8
Copy-Item -LiteralPath "$binary/BUILD-INFO.json" -Destination $sources
Invoke-Checked tar @('-a', '-cf', "$output/ps5-shader-lab-windows-x64.zip", '-C', $stage, 'ps5-shader-lab-windows-x64')
Invoke-Checked tar @('-czf', "$output/ps5-shader-lab-sources.tar.gz", '-C', $sources, '.')
$hashLines = @(Get-ChildItem -LiteralPath $output -File | Sort-Object Name | ForEach-Object {
    "$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant())  $($_.Name)"
})
[IO.File]::WriteAllLines("$output/SHA256SUMS", $hashLines, [Text.UTF8Encoding]::new($false))
Write-Host "Release assets written to $output"
