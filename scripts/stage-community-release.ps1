[CmdletBinding()]
param(
  [ValidateSet("Release", "RelWithDebInfo", "MinSizeRel")]
  [string]$Config = "Release",
  [string]$BuildDir = "build\package",
  [switch]$SkipPackage
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$project = Get-Content -Raw -LiteralPath (Join-Path $root "CMakeLists.txt")
if ($project -notmatch 'project\(qlic\s+VERSION\s+(\d+\.\d+\.\d+)') {
  throw "Could not read the QLIC version."
}
$version = $Matches[1]
if (!$SkipPackage) {
  & (Join-Path $root "scripts\package.ps1") -Config $Config `
    -BuildDir $BuildDir -CommunityRelease
  if (!$?) { throw "Community package build failed." }
}

$dist = Join-Path $root "dist"
$provenancePath = Join-Path $dist "PROVENANCE.intoto.json"
$provenance = Get-Content -Raw -LiteralPath $provenancePath | ConvertFrom-Json
$parameters = $provenance.predicate.buildDefinition.externalParameters
$sourceRevision =
  $provenance.predicate.buildDefinition.resolvedDependencies[0].digest.sha256
if ($parameters.releaseChannel -ne "community" -or $parameters.production -ne $false -or
    $sourceRevision -notmatch '^[0-9a-f]{64}$') {
  throw "dist was not built as an unsigned community release."
}
$currentSourceRevision = (& (Join-Path $root "scripts\get-source-revision.ps1") `
  -SourceDir $root | Out-String).Trim()
if ($sourceRevision -ne $currentSourceRevision) {
  throw "dist was built from a different source tree. Rebuild without -SkipPackage."
}

$releaseRoot = [IO.Path]::GetFullPath((Join-Path $root "release"))
$stage = [IO.Path]::GetFullPath(
  (Join-Path $releaseRoot "qlic-$version-community"))
$prefix = $releaseRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) +
  [IO.Path]::DirectorySeparatorChar
if (!$stage.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
  throw "Refusing to stage outside $releaseRoot"
}
if (Test-Path -LiteralPath $stage) {
  Remove-Item -LiteralPath $stage -Recurse -Force
}
$upload = Join-Path $stage "upload"
$metadata = Join-Path $stage "metadata"
$sbom = Join-Path $metadata "sbom"
New-Item -ItemType Directory -Force $upload, $sbom | Out-Null

$archives = @(Get-ChildItem -LiteralPath (Join-Path $dist "archives") `
  -Filter *.zip -File | Sort-Object Name)
if ($archives.Count -ne 7) { throw "Expected seven package archives." }
foreach ($archive in $archives) {
  Copy-Item -LiteralPath $archive.FullName -Destination $upload
}
Copy-Item -LiteralPath (Join-Path $dist "SHA256SUMS.txt") `
  -Destination (Join-Path $metadata "PACKAGE-SHA256SUMS.txt")
Copy-Item -LiteralPath (Join-Path $dist "SBOMSUMS.txt") -Destination $metadata
Copy-Item -LiteralPath $provenancePath -Destination $metadata
foreach ($file in @(Get-ChildItem -LiteralPath (Join-Path $dist "sbom") `
    -Filter *.spdx.json -File)) {
  Copy-Item -LiteralPath $file.FullName -Destination $sbom
}
$notes = Join-Path $root "docs\releases\$version.md"
Copy-Item -LiteralPath $notes -Destination (Join-Path $stage "RELEASE-NOTES.md")
Copy-Item -LiteralPath $notes -Destination (Join-Path $metadata "RELEASE-NOTES.md")
$verifier = Join-Path $root "scripts\verify-community-release.ps1"
Copy-Item -LiteralPath $verifier -Destination (Join-Path $stage "VERIFY-RELEASE.ps1")
Copy-Item -LiteralPath $verifier -Destination (Join-Path $metadata "VERIFY-RELEASE.ps1")

Add-Type -AssemblyName System.IO.Compression
function New-DeterministicArchive([string]$Source, [string]$Destination) {
  $sourcePath = (Resolve-Path -LiteralPath $Source).Path
  $stream = [IO.File]::Open(
    $Destination, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite,
    [IO.FileShare]::None)
  $zip = [IO.Compression.ZipArchive]::new(
    $stream, [IO.Compression.ZipArchiveMode]::Create, $false)
  try {
    foreach ($file in @(Get-ChildItem -LiteralPath $sourcePath -File -Recurse |
        Sort-Object FullName)) {
      $relative = [IO.Path]::GetRelativePath(
        $sourcePath, $file.FullName).Replace("\", "/")
      $entry = $zip.CreateEntry(
        $relative, [IO.Compression.CompressionLevel]::Optimal)
      $entry.LastWriteTime = [DateTimeOffset]::new(
        1980, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
      $entry.ExternalAttributes = 0
      $input = $file.OpenRead()
      $output = $entry.Open()
      try {
        $input.CopyTo($output)
      } finally {
        $output.Dispose()
        $input.Dispose()
      }
    }
  } finally {
    $zip.Dispose()
    $stream.Dispose()
  }
}

$metadataArchive = Join-Path $upload "qlic-$version-release-metadata.zip"
New-DeterministicArchive $metadata $metadataArchive
$releaseHashFiles = @(
  Get-ChildItem -LiteralPath $upload -Filter *.zip -File | Sort-Object Name)
$releaseHashLines = foreach ($file in $releaseHashFiles) {
  $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
  "$hash  $($file.Name)"
}
$releaseHashLines | Set-Content `
  (Join-Path $upload "RELEASE-SHA256SUMS.txt") -Encoding ascii

$purpose = @{
  "cli-windows-x64" = "Windows command line"
  "gui-windows-x64" = "Windows desktop app"
  "wic-windows-x64" = "Explorer and WIC integration"
  "sdk-windows-x64" = "C and C++ SDK"
  "web" = "Browser and JavaScript package"
  "rust-decoder" = "Safe Rust decoder"
  "source" = "Complete source"
  "release-metadata" = "Checksums, SBOMs, provenance, notes, and verifier"
}
$uploadAssets = foreach ($file in @(Get-ChildItem -LiteralPath $upload -File |
    Sort-Object Name)) {
  $kind = if ($file.Name -eq "RELEASE-SHA256SUMS.txt") {
    "Release checksums"
  } else {
    $match = @($purpose.Keys | Where-Object { $file.BaseName.EndsWith($_) })
    if ($match.Count -ne 1) { throw "Unknown upload asset: $($file.Name)" }
    $purpose[$match[0]]
  }
  [ordered]@{
    name = $file.Name
    bytes = $file.Length
    sha256 = (Get-FileHash -LiteralPath $file.FullName `
      -Algorithm SHA256).Hash.ToLowerInvariant()
    purpose = $kind
  }
}
$assets = [ordered]@{
  schema = 1
  version = $version
  channel = "community"
  signed = $false
  sourceRevision = $sourceRevision
  upload = @($uploadAssets)
}
$json = $assets | ConvertTo-Json -Depth 6
[IO.File]::WriteAllText((Join-Path $stage "RELEASE-ASSETS.json"),
  $json + "`n", [Text.UTF8Encoding]::new($false))

& (Join-Path $stage "VERIFY-RELEASE.ps1") -ReleaseDir $stage
if (!$?) { throw "Release-candidate verification failed." }
Write-Host "Release notes: $(Join-Path $stage 'RELEASE-NOTES.md')"
Write-Host "Exact upload set: $upload"
Write-Host "Nothing was signed, committed, tagged, uploaded, or published."
