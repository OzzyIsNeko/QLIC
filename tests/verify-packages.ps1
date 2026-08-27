param(
  [string]$DistDir = "dist",
  [switch]$CommunityRelease,
  [switch]$RequireSignatures
)

$ErrorActionPreference = "Stop"
if ($CommunityRelease -and $RequireSignatures) {
  throw "Package verification cannot require both community and production tiers."
}
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$dist = if ([IO.Path]::IsPathRooted($DistDir)) {
  $DistDir
} else {
  Join-Path $root $DistDir
}
$archives = Join-Path $dist "archives"
$sbomDir = Join-Path $dist "sbom"

function Test-HashManifest([string]$Manifest, [string]$Directory) {
  $lines = @(Get-Content -LiteralPath $Manifest)
  if ($lines.Count -ne 7) {
    throw "Expected seven entries in $Manifest."
  }
  foreach ($line in $lines) {
    if ($line -notmatch '^([0-9a-f]{64})  (.+)$') {
      throw "Invalid checksum line in $Manifest."
    }
    $file = Join-Path $Directory $Matches[2]
    $actual = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $Matches[1]) {
      throw "Checksum mismatch: $($Matches[2])"
    }
  }
}

function Get-OnlyArchive([string]$Pattern) {
  $matches = @(Get-ChildItem -LiteralPath $archives -Filter $Pattern -File)
  if ($matches.Count -ne 1) {
    throw "Expected one archive matching $Pattern."
  }
  return $matches[0]
}

function Get-ArchiveEntries([IO.FileInfo]$File) {
  Add-Type -AssemblyName System.IO.Compression
  $zip = [IO.Compression.ZipFile]::OpenRead($File.FullName)
  try {
    return @($zip.Entries | ForEach-Object FullName)
  } finally {
    $zip.Dispose()
  }
}

function Get-ArchiveText([IO.FileInfo]$File, [string]$Name) {
  Add-Type -AssemblyName System.IO.Compression
  $zip = [IO.Compression.ZipFile]::OpenRead($File.FullName)
  try {
    $entry = $zip.GetEntry($Name)
    if (!$entry) { throw "$($File.Name) is missing $Name." }
    $reader = [IO.StreamReader]::new($entry.Open(), [Text.Encoding]::UTF8, $true)
    try {
      return $reader.ReadToEnd()
    } finally {
      $reader.Dispose()
    }
  } finally {
    $zip.Dispose()
  }
}

function Get-ArchiveBytes([IO.FileInfo]$File, [string]$Name) {
  Add-Type -AssemblyName System.IO.Compression
  $zip = [IO.Compression.ZipFile]::OpenRead($File.FullName)
  try {
    $entry = $zip.GetEntry($Name)
    if (!$entry) { throw "$($File.Name) is missing $Name." }
    $input = $entry.Open()
    $output = [IO.MemoryStream]::new()
    try {
      $input.CopyTo($output)
      return $output.ToArray()
    } finally {
      $output.Dispose()
      $input.Dispose()
    }
  } finally {
    $zip.Dispose()
  }
}

function Get-EmbeddedBytes([string]$Html, [string]$Id) {
  $pattern = '<script id="' + [Text.RegularExpressions.Regex]::Escape($Id) +
    '"[^>]*>([A-Za-z0-9+/=]+)</script>'
  $match = [Text.RegularExpressions.Regex]::Match($Html, $pattern)
  if (!$match.Success) { throw "Standalone Web app is missing $Id." }
  try {
    return [Convert]::FromBase64String($match.Groups[1].Value)
  } catch {
    throw "Standalone Web app contains invalid $Id data."
  }
}

function Get-ByteHash([byte[]]$Bytes) {
  $sha = [Security.Cryptography.SHA256]::Create()
  try {
    return [BitConverter]::ToString($sha.ComputeHash($Bytes)).Replace("-", "").ToLowerInvariant()
  } finally {
    $sha.Dispose()
  }
}

function ConvertTo-StandaloneClassic([string]$Module) {
  $classic = $Module.Replace(
    "export async function createQlic", "async function createQlic")
  $classic = $classic.Replace(
    'new URL("qlic-web.wasm", import.meta.url)', '"qlic-web.wasm"')
  $classic = $classic.Replace("export default createQlic;", "")
  if ($classic -eq $Module -or $classic -match '(?m)^\s*export\s' -or
      $classic.Contains("import.meta")) {
    throw "The packaged Web module cannot form the standalone classic worker."
  }
  return $classic
}

function Get-SourceArchiveRevision([IO.FileInfo]$File) {
  Add-Type -AssemblyName System.IO.Compression
  $temporary = Join-Path ([IO.Path]::GetTempPath()) `
    ("qlic-source-check-" + [guid]::NewGuid().ToString("N"))
  New-Item -ItemType Directory -Force $temporary | Out-Null
  try {
    [IO.Compression.ZipFile]::ExtractToDirectory($File.FullName, $temporary)
    return (& (Join-Path $root "scripts\get-source-revision.ps1") `
      -SourceDir $temporary | Out-String).Trim()
  } finally {
    Remove-Item -LiteralPath $temporary -Recurse -Force -ErrorAction SilentlyContinue
  }
}

if (@(Get-ChildItem -LiteralPath $archives -Filter *.zip -File).Count -ne 7) {
  throw "Expected seven package archives."
}
Test-HashManifest (Join-Path $dist "SHA256SUMS.txt") $archives

$webArchive = Get-OnlyArchive "qlic-*-web.zip"
$web = Get-ArchiveEntries $webArchive
foreach ($required in @(
  "qlic-web.js", "qlic-web.d.ts", "qlic-web.wasm", "qlic-worker.js",
  "index.html", "package.json",
  "LICENSE", "LICENSE-LZMS", "SECURITY.md", "SUPPORT.md"
)) {
  if ($web -notcontains $required) {
    throw "Web archive is missing $required."
  }
}
if (@($web | Where-Object { $_ -match '\.(exe|dll)$' }).Count) {
  throw "The browser-native Web archive contains a native executable or DLL."
}
$webPackage = Get-ArchiveText $webArchive "package.json" | ConvertFrom-Json
if ($webPackage.private -or $webPackage.module -ne "./qlic-web.js" -or
    $webPackage.types -ne "./qlic-web.d.ts") {
  throw "The Web archive is not a publishable ESM package."
}
$webIndex = Get-ArchiveText $webArchive "index.html"
if ($webIndex -notmatch 'id="qlic-standalone-module"' -or
    $webIndex -notmatch 'id="qlic-standalone-wasm"' -or
    $webIndex -notmatch 'id="qlic-standalone-worker"' -or
    $webIndex -notmatch 'id="previewCanvas"' -or
    $webIndex -notmatch 'id="savePng"' -or
    $webIndex -notmatch 'id="pixelInfo"' -or
    $webIndex -notmatch 'accept="\.qlic' -or
    $webIndex -notmatch 'touch-action:\s*none' -or
    $webIndex -match '__QLIC_WEB_(MODULE|WASM|WORKER)_BASE64__' -or
    $webIndex -match 'QLIC Web\.exe') {
  throw "The Web archive does not contain a complete standalone browser app."
}
$embeddedModule = Get-EmbeddedBytes $webIndex "qlic-standalone-module"
$embeddedWasm = Get-EmbeddedBytes $webIndex "qlic-standalone-wasm"
$embeddedWorker = [Text.Encoding]::UTF8.GetString(
  (Get-EmbeddedBytes $webIndex "qlic-standalone-worker"))
$packagedModule = [Text.Encoding]::UTF8.GetString(
  (Get-ArchiveBytes $webArchive "qlic-web.js"))
$expectedStandaloneModule = [Text.Encoding]::UTF8.GetBytes(
  (ConvertTo-StandaloneClassic $packagedModule))
if ((Get-ByteHash $embeddedModule) -ne
      (Get-ByteHash $expectedStandaloneModule) -or
    (Get-ByteHash $embeddedWasm) -ne
      (Get-ByteHash (Get-ArchiveBytes $webArchive "qlic-web.wasm")) -or
    [Text.Encoding]::UTF8.GetString($embeddedModule) -match
      '(?m)^\s*export\s|import\.meta' -or
    $embeddedWorker -notmatch 'createQlic\(\{\s*wasmBinary:\s*qlicStandaloneWasm\s*\}\)' -or
    $embeddedWorker -notmatch 'operation\s*===\s*"decode"' -or
    $embeddedWorker -notmatch 'operation\s*===\s*"png"' -or
    $embeddedWorker -match 'import\s+.*qlic-web\.js' -or
    $webIndex -notmatch
      'new Worker\(workerObjectUrl,\s*\{\s*name:\s*"QLIC codec"\s*\}\)') {
  throw "The standalone browser app does not embed a complete classic worker."
}

$sdk = Get-ArchiveEntries (Get-OnlyArchive "qlic-*-sdk-windows-x64.zip")
if ($sdk -notcontains "lib/pkgconfig/qlic.pc") {
  throw "SDK archive is missing lib/pkgconfig/qlic.pc."
}
foreach ($required in @(
  "docs/sdk.md", "docs/profiles.md", "docs/profiles.json", "docs/format.md",
  "docs/core-still.md", "docs/hdr-format.md", "docs/predictor-math.md",
  "docs/qst1-model-annex.md", "docs/conformance.lock.json"
)) {
  if ($sdk -notcontains $required) {
    throw "SDK archive is missing $required."
  }
}
foreach ($removed in @(
  "docs/adoption.md", "docs/architecture.md", "docs/benchmark-current.md",
  "docs/encoder-design.md", "docs/release-gate.md"
)) {
  if ($sdk -contains $removed) {
    throw "SDK archive contains internal documentation: $removed."
  }
}

$rust = Get-ArchiveEntries (Get-OnlyArchive "qlic-*-rust-decoder.zip")
foreach ($required in @(
  "Cargo.toml", "Cargo.lock", "README.md", "LICENSE", "LICENSE-LZMS",
  "SECURITY.md", "SUPPORT.md", "examples/decode.rs"
)) {
  if ($rust -notcontains $required) {
    throw "Rust archive is missing $required."
  }
}
foreach ($pattern in @(
  "qlic-*-cli-windows-x64.zip",
  "qlic-*-gui-windows-x64.zip",
  "qlic-*-wic-windows-x64.zip"
)) {
  $entries = Get-ArchiveEntries (Get-OnlyArchive $pattern)
  if ($entries -contains "docs/benchmark-current.md") {
    throw "$pattern contains a copied benchmark report."
  }
}
$wic = Get-ArchiveEntries (Get-OnlyArchive "qlic-*-wic-windows-x64.zip")
foreach ($required in @(
  "install-wic.cmd", "install-wic.ps1",
  "uninstall-wic.cmd", "uninstall-wic.ps1"
)) {
  if ($wic -notcontains $required) {
    throw "WIC archive is missing $required."
  }
}

$source = Get-ArchiveEntries (Get-OnlyArchive "qlic-*-source.zip")
if ($source | Where-Object { $_ -match '(^|/)build/' }) {
  throw "Source archive contains generated build output."
}
foreach ($required in @(
  "CMakeLists.txt", "README.md", "codec/include/qlic/qlic.h",
  "rust/qlic-decoder/Cargo.toml", "web/qlic-web.js",
  "tests/fixtures/retained/mode45-current-streams/0013.qlic",
  "benchmark/tools/qlic_mode_trial_bench.c",
  "benchmark/tools/summarize_gradient_topology.py",
  "benchmark/gradient-topology-replay.md"
)) {
  if ($source -notcontains $required) {
    throw "Source archive is missing $required."
  }
}

$sboms = @(Get-ChildItem -LiteralPath $sbomDir -Filter *.spdx.json -File)
if ($sboms.Count -ne 7) {
  throw "Expected seven SPDX SBOMs."
}
foreach ($file in $sboms) {
  $document = Get-Content -Raw -LiteralPath $file.FullName | ConvertFrom-Json
  if ($document.spdxVersion -ne "SPDX-2.3" -or
      $document.dataLicense -ne "CC0-1.0" -or
      $document.packages.Count -ne 1 -or
      !$document.files) {
    throw "Invalid SPDX document: $($file.Name)"
  }
}
Test-HashManifest (Join-Path $dist "SBOMSUMS.txt") $sbomDir

$provenancePath = Join-Path $dist "PROVENANCE.intoto.json"
$provenance = Get-Content -Raw -LiteralPath $provenancePath | ConvertFrom-Json
if ($provenance._type -ne "https://in-toto.io/Statement/v1" -or
    $provenance.predicateType -ne "https://slsa.dev/provenance/v1" -or
    @($provenance.subject).Count -ne 7) {
  throw "Invalid SLSA/in-toto provenance statement."
}
foreach ($subject in @($provenance.subject)) {
  $archive = Join-Path $archives $subject.name
  $actual = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($actual -ne $subject.digest.sha256) {
    throw "Provenance digest mismatch: $($subject.name)"
  }
}
$provenanceSourceRevision =
  $provenance.predicate.buildDefinition.resolvedDependencies[0].digest.sha256
$expectedChannel = if ($RequireSignatures) {
  "production"
} elseif ($CommunityRelease) {
  "community"
} else {
  "development"
}
if ($provenance.predicate.buildDefinition.externalParameters.releaseChannel -ne
    $expectedChannel) {
  throw "Provenance release channel is not $expectedChannel."
}
if ($provenanceSourceRevision -notmatch '^[0-9a-fA-F]{64}$') {
  throw "Provenance is missing its source-tree revision."
}
$packagedSourceRevision = Get-SourceArchiveRevision `
  (Get-OnlyArchive "qlic-*-source.zip")
if ($packagedSourceRevision -ne $provenanceSourceRevision) {
  throw "Source archive differs from the provenance build input."
}

if ($RequireSignatures) {
  $qualificationPath = Join-Path $dist "QUALIFICATION.json"
  if (!(Test-Path -LiteralPath $qualificationPath)) {
    throw "Production package set is missing QUALIFICATION.json."
  }
  $qualificationDigest = (Get-FileHash -LiteralPath $qualificationPath `
    -Algorithm SHA256).Hash.ToLowerInvariant()
  $provenanceQualification =
    $provenance.predicate.buildDefinition.externalParameters.qualification
  if (!$provenance.predicate.buildDefinition.externalParameters.production -or
      $provenanceQualification.sha256 -ne $qualificationDigest -or
      $provenanceQualification.sourceRevision -notmatch '^[0-9a-fA-F]{64}$' -or
      $provenanceQualification.sourceRevision -ne
        $provenance.predicate.buildDefinition.resolvedDependencies[0].digest.sha256) {
    throw "Production provenance is not tied to its qualification record."
  }
  $temporary = Join-Path ([IO.Path]::GetTempPath()) `
    ("qlic-signature-check-" + [guid]::NewGuid().ToString("N"))
  New-Item -ItemType Directory -Force $temporary | Out-Null
  try {
    foreach ($archive in @(Get-ChildItem -LiteralPath $archives -Filter *.zip -File)) {
      $destination = Join-Path $temporary $archive.BaseName
      [IO.Compression.ZipFile]::ExtractToDirectory($archive.FullName, $destination)
      $isSourceArchive = $archive.Name -like "qlic-*-source.zip"
      foreach ($file in @(Get-ChildItem -LiteralPath $destination -File -Recurse |
          Where-Object {
            $_.Extension -in @(".exe", ".dll") -or
            (!$isSourceArchive -and $_.Extension -eq ".ps1")
          })) {
        $signature = Get-AuthenticodeSignature -LiteralPath $file.FullName
        if ($signature.Status -ne "Valid") {
          throw "Production archive contains an invalid signature: $($file.FullName)"
        }
      }
    }
  } finally {
    Remove-Item -LiteralPath $temporary -Recurse -Force -ErrorAction SilentlyContinue
  }
} else {
  $marker = if ($CommunityRelease) {
    "UNSIGNED-COMMUNITY-RELEASE.txt"
  } else {
    "UNSIGNED-DEVELOPMENT-BUILD.txt"
  }
  foreach ($archive in @(Get-ChildItem -LiteralPath $archives -Filter *.zip -File)) {
    $entries = Get-ArchiveEntries $archive
    $otherMarker = if ($CommunityRelease) {
      "UNSIGNED-DEVELOPMENT-BUILD.txt"
    } else {
      "UNSIGNED-COMMUNITY-RELEASE.txt"
    }
    if ($entries -notcontains $marker -or $entries -contains $otherMarker) {
      throw "Unsigned archive is missing $marker`: $($archive.Name)"
    }
    $notice = Get-ArchiveText $archive $marker
    if ($CommunityRelease -and
        ($notice -notmatch 'SmartScreen' -or $notice -notmatch 'SHA-256')) {
      throw "Community release notice is incomplete: $($archive.Name)"
    }
  }
}

Write-Host "QLIC package manifests and required contents passed."
