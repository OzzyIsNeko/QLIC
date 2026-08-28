[CmdletBinding()]
param(
  [Alias("StageDir")]
  [string]$ReleaseDir = ""
)

$ErrorActionPreference = "Stop"
$release = if ($ReleaseDir) {
  (Resolve-Path -LiteralPath $ReleaseDir).Path
} else {
  $PSScriptRoot
}
$upload = Join-Path $release "upload"
$metadata = Join-Path $release "metadata"
$assetsPath = Join-Path $release "RELEASE-ASSETS.json"
$releaseSums = Join-Path $upload "RELEASE-SHA256SUMS.txt"
foreach ($path in @($upload, $metadata, $assetsPath, $releaseSums)) {
  if (!(Test-Path -LiteralPath $path)) {
    throw "Release bundle is missing $path"
  }
}

function Read-HashManifest([string]$Path, [string]$Directory) {
  $result = [ordered]@{}
  foreach ($line in @(Get-Content -LiteralPath $Path)) {
    if ($line -notmatch '^([0-9a-f]{64})  ([^/\\]+)$') {
      throw "Invalid checksum line in $Path`: $line"
    }
    $name = $Matches[2]
    if ($result.Contains($name)) { throw "Duplicate checksum entry: $name" }
    $file = Join-Path $Directory $name
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) {
      throw "Checksum target is missing: $file"
    }
    $actual = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $Matches[1]) { throw "Checksum mismatch: $name" }
    $result[$name] = $actual
  }
  return $result
}

function Assert-NameSet([object[]]$Actual, [object[]]$Expected, [string]$Label) {
  $actualNames = @($Actual | ForEach-Object { [string]$_ } | Sort-Object)
  $expectedNames = @($Expected | ForEach-Object { [string]$_ } | Sort-Object)
  if ($actualNames.Count -ne $expectedNames.Count -or
      [string]::Join("`n", $actualNames) -cne
        [string]::Join("`n", $expectedNames)) {
    throw "$Label has unexpected file names."
  }
}

Add-Type -AssemblyName System.IO.Compression
function Get-ZipEntries([string]$Path) {
  $zip = [IO.Compression.ZipFile]::OpenRead($Path)
  try {
    return @($zip.Entries | ForEach-Object FullName)
  } finally {
    $zip.Dispose()
  }
}

$assets = Get-Content -Raw -LiteralPath $assetsPath | ConvertFrom-Json
if ($assets.schema -ne 1 -or $assets.channel -ne "community" -or
    $assets.signed -ne $false -or
    $assets.version -notmatch '^\d+\.\d+\.\d+$' -or
    $assets.sourceRevision -notmatch '^[0-9a-f]{64}$') {
  throw "Release asset identity is invalid."
}
$expectedPackages = @(
  "qlic-$($assets.version)-cli-windows-x64.zip",
  "qlic-$($assets.version)-gui-windows-x64.zip",
  "qlic-$($assets.version)-wic-windows-x64.zip",
  "qlic-$($assets.version)-sdk-windows-x64.zip",
  "qlic-$($assets.version)-web.zip",
  "qlic-$($assets.version)-rust-decoder.zip",
  "qlic-$($assets.version)-source.zip"
)
$metadataArchive = "qlic-$($assets.version)-release-metadata.zip"
$expectedUpload = @($expectedPackages) + @(
  $metadataArchive, "RELEASE-SHA256SUMS.txt")
$uploadFiles = @(Get-ChildItem -LiteralPath $upload -File | Sort-Object Name)
Assert-NameSet @($uploadFiles.Name) $expectedUpload "Upload directory"
Assert-NameSet @($assets.upload.name) $expectedUpload "RELEASE-ASSETS.json"
foreach ($asset in @($assets.upload)) {
  if ([IO.Path]::GetFileName($asset.name) -ne $asset.name -or
      $asset.sha256 -notmatch '^[0-9a-f]{64}$' -or $asset.bytes -lt 1) {
    throw "Invalid release asset entry: $($asset.name)"
  }
  $file = Get-Item -LiteralPath (Join-Path $upload $asset.name) `
    -ErrorAction SilentlyContinue
  if (!$file -or $file.Length -ne $asset.bytes -or
      (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() -ne
        $asset.sha256) {
    throw "Release asset differs: $($asset.name)"
  }
}

$releaseHashes = Read-HashManifest $releaseSums $upload
Assert-NameSet @($releaseHashes.Keys) `
  (@($expectedPackages) + @($metadataArchive)) "RELEASE-SHA256SUMS.txt"
$provenancePath = Join-Path $metadata "PROVENANCE.intoto.json"
$provenance = Get-Content -Raw -LiteralPath $provenancePath | ConvertFrom-Json
$parameters = $provenance.predicate.buildDefinition.externalParameters
$sourceRevision =
  $provenance.predicate.buildDefinition.resolvedDependencies[0].digest.sha256
if ($parameters.releaseChannel -ne "community" -or $parameters.production -ne $false -or
    $sourceRevision -ne $assets.sourceRevision -or
    @($provenance.subject).Count -ne 7) {
  throw "Community provenance identity is invalid."
}

$packageHashes = Read-HashManifest `
  (Join-Path $metadata "PACKAGE-SHA256SUMS.txt") $upload
Assert-NameSet @($packageHashes.Keys) $expectedPackages "PACKAGE-SHA256SUMS.txt"
Assert-NameSet @($provenance.subject.name) $expectedPackages "Provenance subjects"
foreach ($subject in @($provenance.subject)) {
  if (!$packageHashes.Contains($subject.name) -or
      $packageHashes[$subject.name] -ne $subject.digest.sha256) {
    throw "Provenance subject differs: $($subject.name)"
  }
  $entries = Get-ZipEntries (Join-Path $upload $subject.name)
  if ($entries -notcontains "UNSIGNED-COMMUNITY-RELEASE.txt" -or
      $entries -contains "UNSIGNED-DEVELOPMENT-BUILD.txt") {
    throw "Package has the wrong unsigned-release marker: $($subject.name)"
  }
}

$sbomHashes = Read-HashManifest `
  (Join-Path $metadata "SBOMSUMS.txt") (Join-Path $metadata "sbom")
$expectedSboms = @($expectedPackages | ForEach-Object {
  "$([IO.Path]::GetFileNameWithoutExtension($_)).spdx.json"
})
Assert-NameSet @($sbomHashes.Keys) $expectedSboms "SBOMSUMS.txt"
if (!$releaseHashes.Contains($metadataArchive)) {
  throw "Release checksums omit $metadataArchive."
}
$metadataEntries = Get-ZipEntries (Join-Path $upload $metadataArchive)
$expectedMetadata = @(
  "PACKAGE-SHA256SUMS.txt", "SBOMSUMS.txt", "PROVENANCE.intoto.json",
  "RELEASE-NOTES.md", "VERIFY-RELEASE.ps1"
) + @($expectedSboms | ForEach-Object { "sbom/$_" })
Assert-NameSet $metadataEntries $expectedMetadata "Release metadata archive"

Write-Host "QLIC $($assets.version) unsigned community release verified."
Write-Host "Source revision: $sourceRevision"
Write-Host "Upload directory: $upload"
