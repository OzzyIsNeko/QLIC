[CmdletBinding()]
param(
  [ValidateSet("Release", "RelWithDebInfo", "MinSizeRel")]
  [string]$Config = "Release",
  [string]$BuildDir = "build\package",
  [string]$CleanSourceDir = "",
  [switch]$CommunityRelease,
  [switch]$Production,
  [string]$SigningCertificateThumbprint = "",
  [string]$TimestampUrl = "http://timestamp.digicert.com",
  [string]$QualificationRecord = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$signingCertificate = $null
$signTool = $null
if ($CommunityRelease -and $Production) {
  throw "Choose either -CommunityRelease or -Production."
}
if ($Production) {
  if (!$CleanSourceDir) {
    throw "Production packaging requires -CleanSourceDir."
  }
  if ($SigningCertificateThumbprint -notmatch '^[0-9a-fA-F]{40}$') {
    throw "Production packaging requires a SHA-1 certificate thumbprint."
  }
  if ($TimestampUrl -notmatch '^https?://') {
    throw "Production packaging requires an HTTP(S) RFC 3161 timestamp URL."
  }
  if (!$QualificationRecord) {
    throw "Production packaging requires an approved -QualificationRecord."
  }
  & (Join-Path $root "scripts\check-qualification.ps1") `
    -Record $QualificationRecord -RequireApproval
  $certificates = @(
    Get-ChildItem Cert:\CurrentUser\My,Cert:\LocalMachine\My |
      Where-Object {
        $_.Thumbprint -eq $SigningCertificateThumbprint -and $_.HasPrivateKey -and
        $_.NotBefore -le [DateTime]::Now -and $_.NotAfter -gt [DateTime]::Now
      }
  )
  if ($certificates.Count -ne 1) {
    throw "Exactly one valid code-signing certificate with the requested thumbprint is required."
  }
  $signingCertificate = $certificates[0]
  $signToolCommand = Get-Command signtool.exe -ErrorAction SilentlyContinue
  if ($signToolCommand) {
    $signTool = $signToolCommand.Source
  } else {
    $kitRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
    $signTool = Get-ChildItem $kitRoot -Filter signtool.exe -File -Recurse `
      -ErrorAction SilentlyContinue |
      Where-Object { $_.Directory.Name -eq "x64" } |
      Sort-Object FullName -Descending | Select-Object -First 1 `
      -ExpandProperty FullName
  }
  if (!$signTool -or !(Test-Path -LiteralPath $signTool)) {
    throw "signtool.exe is required for production packaging."
  }
}
$cleanSource = $null
if ($CleanSourceDir) {
  $cleanSource = (Resolve-Path -LiteralPath $CleanSourceDir).Path
  foreach ($required in @("CMakeLists.txt", "codec\src\stream.c", "codec\src\qlic.c")) {
    if (!(Test-Path -LiteralPath (Join-Path $cleanSource $required) -PathType Leaf)) {
      throw "Clean source tree is missing $required`: $cleanSource"
    }
  }
}
$buildSource = if ($Production) { $cleanSource } else { $root }
$archiveSource = if ($cleanSource) { $cleanSource } else { $buildSource }
$sourceRevision = (& (Join-Path $buildSource "scripts\get-source-revision.ps1") `
  -SourceDir $buildSource | Out-String).Trim()
if ($sourceRevision -notmatch '^[0-9a-f]{64}$') {
  throw "Could not calculate the source-tree revision."
}
if ($Production) {
  $qualifiedRevision = (Get-Content -Raw -LiteralPath `
    (Resolve-Path -LiteralPath $QualificationRecord).Path |
      ConvertFrom-Json).source_revision.ToLowerInvariant()
  if ($sourceRevision -ne $qualifiedRevision) {
    throw "The qualification record does not match the production source tree."
  }
}
& (Join-Path $buildSource "scripts\build-clang.ps1") -Config $Config `
  -BuildDir $BuildDir
& (Join-Path $buildSource "scripts\build-web.ps1") -OutDir "web\dist"

$build = if ([System.IO.Path]::IsPathRooted($BuildDir)) {
  $BuildDir
} else {
  Join-Path $buildSource $BuildDir
}
$qlicBinary = Get-Item (Join-Path $build "qlic.exe") -ErrorAction SilentlyContinue
if (!$qlicBinary) { throw "The QLIC build output was not found under $build." }
& (Join-Path $buildSource "tests\test-wasm.ps1") -Config $Config `
  -Qlic $qlicBinary.FullName -SkipWebBuild
$versionOutput = (& $qlicBinary.FullName version | Out-String).Trim()
if ($versionOutput -notmatch '\d+\.\d+\.\d+') { throw "Could not read the QLIC version." }
$version = $Matches[0]

$dist = Join-Path $root "dist"
$cli = Join-Path $dist "qlic-cli"
$gui = Join-Path $dist "qlic-gui"
$wic = Join-Path $dist "qlic-wic"
$sdk = Join-Path $dist "qlic-sdk"
$web = Join-Path $dist "qlic-web"
$rust = Join-Path $dist "qlic-rust-decoder"
$source = Join-Path $dist "qlic-source"
$archives = Join-Path $dist "archives"
$sbom = Join-Path $dist "sbom"
$install = Join-Path $dist "cmake-install"
$consumerBuild = Join-Path $dist "package-consumer-build"
$distPath = [IO.Path]::GetFullPath($dist).TrimEnd(
  [IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
foreach ($path in @(
  $cli,$gui,$wic,$sdk,$web,$rust,$source,$archives,$sbom,$install,$consumerBuild
)) {
  $fullPath = [IO.Path]::GetFullPath($path)
  if (!$fullPath.StartsWith($distPath, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to clear a path outside dist: $fullPath"
  }
  Remove-Item -LiteralPath $fullPath -Recurse -Force `
    -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force $cli,$gui,$wic,$sdk,$web,$rust,$source,$sbom | Out-Null

& cmake --install $build --prefix $install --config $Config
if ($LASTEXITCODE -ne 0) { throw "CMake install failed." }

function Copy-PackageBasics($Destination, [string]$Readme) {
  foreach ($name in @("LICENSE", "NOTICE", "SECURITY.md", "SUPPORT.md")) {
    Copy-Item (Join-Path $buildSource $name) $Destination -Force
  }
  $lzmsLicense = Join-Path $Destination "third_party\lzms"
  $wimlibNotice = Join-Path $Destination "third_party\wimlib"
  New-Item -ItemType Directory -Force $lzmsLicense,$wimlibNotice | Out-Null
  Copy-Item (Join-Path $buildSource "third_party\lzms\LICENSE") $lzmsLicense -Force
  Copy-Item (Join-Path $buildSource "third_party\wimlib\README.md") $wimlibNotice -Force
  if ($Readme) {
    Copy-Item (Join-Path $buildSource $Readme) (Join-Path $Destination "README.md") -Force
  }
}

Copy-Item (Join-Path $build "qlic.exe") $cli -Force
Copy-Item (Join-Path $build "image-codecs") $cli -Recurse -Force
Copy-PackageBasics $cli "packaging\README-cli.md"

Copy-Item (Join-Path $build "qlic.exe") $gui -Force
Copy-Item (Join-Path $build "qlic-gui.exe") $gui -Force
Copy-Item (Join-Path $build "image-codecs") $gui -Recurse -Force
Copy-PackageBasics $gui "packaging\README-gui.md"

Copy-Item (Join-Path $build "qlic-wic.dll") $wic -Force
Copy-Item (Join-Path $build "qlic-gui.exe") $wic -Force
Copy-Item (Join-Path $build "qlic.exe") $wic -Force
Copy-Item (Join-Path $build "image-codecs") $wic -Recurse -Force
Copy-Item (Join-Path $buildSource "scripts\install-wic.ps1") $wic -Force
Copy-Item (Join-Path $buildSource "scripts\install-wic.cmd") $wic -Force
Copy-Item (Join-Path $buildSource "scripts\uninstall-wic.ps1") $wic -Force
Copy-Item (Join-Path $buildSource "scripts\uninstall-wic.cmd") $wic -Force
Copy-PackageBasics $wic "packaging\README-wic.md"

$sdkBin = Join-Path $sdk "bin"
$sdkLib = Join-Path $sdk "lib"
$sdkInclude = Join-Path $sdk "include"
New-Item -ItemType Directory -Force $sdkBin,$sdkLib,$sdkInclude | Out-Null
Copy-Item (Join-Path $install "bin\qlic.dll") $sdkBin -Force
Copy-Item (Join-Path $install "lib\qlic.lib") $sdkLib -Force
Copy-Item (Join-Path $install "lib\qlic_static.lib") $sdkLib -Force
Copy-Item (Join-Path $install "lib\cmake") $sdkLib -Recurse -Force
Copy-Item (Join-Path $install "lib\pkgconfig") $sdkLib -Recurse -Force
Copy-Item (Join-Path $install "include\qlic") $sdkInclude -Recurse -Force
$sdkDocs = Join-Path $sdk "docs"
New-Item -ItemType Directory -Force $sdkDocs | Out-Null
foreach ($name in @(
  "sdk.md", "profiles.md", "profiles.json", "format.md", "core-still.md",
  "hdr-format.md", "predictor-math.md", "qst1-model-annex.md",
  "conformance.lock.json"
)) {
  Copy-Item (Join-Path $buildSource "docs\$name") $sdkDocs -Force
}
Copy-Item (Join-Path $buildSource "examples") $sdk -Recurse -Force
Copy-PackageBasics $sdk "packaging\README-sdk.md"

$capabilities = & cmake -E capabilities | ConvertFrom-Json
$preferredGenerators = @("Visual Studio 18 2026", "Visual Studio 17 2022")
$availableGenerators = @($capabilities.generators | ForEach-Object { $_.name })
$consumerGenerator = $preferredGenerators |
  Where-Object { $availableGenerators -contains $_ } |
  Select-Object -First 1
if (!$consumerGenerator) {
  throw "Visual Studio 2022 or newer with the C++ workload is required."
}
& cmake -S (Join-Path $buildSource "tests\package_consumer") -B $consumerBuild `
  -G $consumerGenerator -A x64 `
  "-DCMAKE_PREFIX_PATH=$sdk" `
  "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded"
if ($LASTEXITCODE -ne 0) { throw "SDK consumer configure failed." }
& cmake --build $consumerBuild --config Release
if ($LASTEXITCODE -ne 0) { throw "SDK consumer build failed." }
# build outside the source tree so broken installed targets cannot pass unnoticed
& (Join-Path $consumerBuild "Release\qlic-package-consumer-static.exe")
if ($LASTEXITCODE -ne 0) { throw "SDK consumer test failed." }
$previousPath = $env:PATH
try {
  $env:PATH = (Join-Path $sdk "bin") + [IO.Path]::PathSeparator + $env:PATH
  & (Join-Path $consumerBuild "Release\qlic-package-consumer-shared.exe")
  if ($LASTEXITCODE -ne 0) { throw "Shared SDK consumer test failed." }
} finally {
  $env:PATH = $previousPath
}

Copy-Item (Join-Path $buildSource "web\dist\qlic-web.wasm") $web -Force
Copy-Item (Join-Path $buildSource "web\dist\qlic-web.js") $web -Force
Copy-Item (Join-Path $buildSource "web\dist\qlic-web.d.ts") $web -Force
Copy-Item (Join-Path $buildSource "web\dist\qlic-worker.js") $web -Force
Copy-Item (Join-Path $buildSource "web\dist\index.html") $web -Force
Copy-Item (Join-Path $buildSource "web\dist\README.md") $web -Force
Copy-Item (Join-Path $buildSource "web\dist\package.json") $web -Force
Copy-Item (Join-Path $buildSource "web\dist\LICENSE-LZMS") $web -Force
Copy-PackageBasics $web

$rustSource = Join-Path $buildSource "rust\qlic-decoder"
foreach ($name in @("Cargo.toml", "Cargo.lock", "README.md", "LICENSE-LZMS")) {
  Copy-Item (Join-Path $rustSource $name) $rust -Force
}
Copy-Item (Join-Path $rustSource "src") $rust -Recurse -Force
Copy-Item (Join-Path $rustSource "examples") $rust -Recurse -Force
foreach ($name in @("LICENSE", "NOTICE", "SECURITY.md", "SUPPORT.md")) {
  Copy-Item (Join-Path $buildSource $name) $rust -Force
}
$cargo = (Get-Command cargo.exe -ErrorAction Stop).Source
$previousCargoTarget = $env:CARGO_TARGET_DIR
try {
  $env:CARGO_TARGET_DIR = Join-Path $consumerBuild "rust-target"
  & $cargo check --release --examples --manifest-path `
    (Join-Path $rust "Cargo.toml")
  if ($LASTEXITCODE -ne 0) { throw "Rust package check failed." }
} finally {
  $env:CARGO_TARGET_DIR = $previousCargoTarget
}

# Package the reviewed build input without generated output.
$sourceRoot = $archiveSource
$excludedSourceRoots = @(
  (Join-Path $sourceRoot "build"),
  (Join-Path $sourceRoot "dist"),
  (Join-Path $sourceRoot "release"),
  (Join-Path $sourceRoot "rust\qlic-decoder\target")
) | ForEach-Object {
  [IO.Path]::GetFullPath($_).TrimEnd([IO.Path]::DirectorySeparatorChar) +
    [IO.Path]::DirectorySeparatorChar
}
$sourceFiles = @(Get-ChildItem -LiteralPath $sourceRoot -File -Recurse | Where-Object {
  $full = $_.FullName
  $inExcludedRoot = $false
  foreach ($excluded in $excludedSourceRoots) {
    if ($full.StartsWith($excluded, [StringComparison]::OrdinalIgnoreCase)) {
      $inExcludedRoot = $true
      break
    }
  }
  !$inExcludedRoot -and
    $full -notmatch '[\\/]build[\\/]' -and
    $full -notmatch '[\\/]web[\\/]dist(?:-[^\\/]+)?[\\/]' -and
    $full -notmatch '[\\/]__pycache__[\\/]'
})
foreach ($file in $sourceFiles) {
  $relative = [IO.Path]::GetRelativePath($sourceRoot, $file.FullName)
  $destination = Join-Path $source $relative
  $directory = Split-Path -Parent $destination
  New-Item -ItemType Directory -Force $directory | Out-Null
  Copy-Item -LiteralPath $file.FullName -Destination $destination -Force
}

$packageDirectories = @($cli,$gui,$wic,$sdk,$web,$rust,$source)
if ($Production) {
  foreach ($directory in $packageDirectories) {
    foreach ($binary in @(Get-ChildItem -LiteralPath $directory -File -Recurse |
        Where-Object { $_.Extension -in @(".exe", ".dll") })) {
      & $signTool sign /sha1 $SigningCertificateThumbprint /fd SHA256 `
        /tr $TimestampUrl /td SHA256 $binary.FullName
      if ($LASTEXITCODE -ne 0) { throw "Signing failed: $($binary.FullName)" }
      & $signTool verify /pa /all $binary.FullName | Out-Null
      if ($LASTEXITCODE -ne 0) {
        throw "Authenticode verification failed: $($binary.FullName)"
      }
    }
    # Preserve source bytes; provenance covers source scripts.
    if ([IO.Path]::GetFullPath($directory) -ne [IO.Path]::GetFullPath($source)) {
      foreach ($script in @(Get-ChildItem -LiteralPath $directory -Filter *.ps1 `
          -File -Recurse)) {
        $signature = Set-AuthenticodeSignature -LiteralPath $script.FullName `
          -Certificate $signingCertificate -TimestampServer $TimestampUrl `
          -HashAlgorithm SHA256
        if ($signature.Status -ne "Valid") {
          throw "PowerShell signing failed: $($script.FullName): $($signature.StatusMessage)"
        }
      }
    }
  }
  Copy-Item -LiteralPath (Resolve-Path -LiteralPath $QualificationRecord).Path `
    -Destination (Join-Path $dist "QUALIFICATION.json") -Force
} else {
  $marker = if ($CommunityRelease) {
    "UNSIGNED-COMMUNITY-RELEASE.txt"
  } else {
    "UNSIGNED-DEVELOPMENT-BUILD.txt"
  }
  if ($CommunityRelease) {
    $warning = @"
QLIC $version unsigned community release

This package passed the automated release and package checks, but its Windows
binaries and PowerShell scripts have no Authenticode publisher identity.
Windows may show an Unknown publisher or SmartScreen warning. Verify the
published SHA-256 checksum before running it.
"@
  } else {
    $warning = @"
QLIC development package

This package is unsigned and is not a public release. Use -CommunityRelease to
stage an unsigned public release, or -Production with the required signing and
qualification inputs.
"@
  }
  foreach ($directory in $packageDirectories) {
    [IO.File]::WriteAllText(
      (Join-Path $directory $marker), $warning,
      [Text.UTF8Encoding]::new($false))
  }
}

New-Item -ItemType Directory -Force $archives | Out-Null
Add-Type -AssemblyName System.IO.Compression
function New-DeterministicArchive([string]$Source, [string]$Destination) {
  $sourcePath = (Resolve-Path -LiteralPath $Source).Path
  if (Test-Path -LiteralPath $Destination) {
    Remove-Item -LiteralPath $Destination -Force
  }
  $stream = [IO.File]::Open(
    $Destination, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite,
    [IO.FileShare]::None)
  $archive = [IO.Compression.ZipArchive]::new(
    $stream, [IO.Compression.ZipArchiveMode]::Create, $false)
  try {
    # file order and timestamps are fixed so archive hashes can be reproduced
    $files = @(Get-ChildItem -LiteralPath $sourcePath -File -Recurse |
        Sort-Object FullName)
    foreach ($file in $files) {
      $relative = [IO.Path]::GetRelativePath(
        $sourcePath, $file.FullName).Replace("\", "/")
      $entry = $archive.CreateEntry(
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
    $archive.Dispose()
    $stream.Dispose()
  }
}

$packages = @(
  @{ Path = $cli; Name = "qlic-$version-cli-windows-x64.zip" },
  @{ Path = $gui; Name = "qlic-$version-gui-windows-x64.zip" },
  @{ Path = $wic; Name = "qlic-$version-wic-windows-x64.zip" },
  @{ Path = $sdk; Name = "qlic-$version-sdk-windows-x64.zip" },
  @{ Path = $web; Name = "qlic-$version-web.zip" },
  @{ Path = $rust; Name = "qlic-$version-rust-decoder.zip" },
  @{ Path = $source; Name = "qlic-$version-source.zip" }
)
foreach ($package in $packages) {
  $destination = Join-Path $archives $package.Name
  New-DeterministicArchive $package.Path $destination
}

$hashFiles = @(Get-ChildItem $archives -Filter *.zip -File) | Sort-Object Name
$hashLines = foreach ($file in $hashFiles) {
  $hash = (Get-FileHash $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
  "$hash  $($file.Name)"
}
$hashLines | Set-Content (Join-Path $dist "SHA256SUMS.txt") -Encoding ascii

$sbomScript = Join-Path $buildSource "scripts\write-sbom.ps1"
foreach ($package in $packages) {
  $name = [IO.Path]::GetFileNameWithoutExtension($package.Name)
  & $sbomScript -PackageDir $package.Path -PackageName $name `
    -PackageVersion $version -OutputPath (Join-Path $sbom "$name.spdx.json")
}
$sbomFiles = @(Get-ChildItem $sbom -Filter *.spdx.json -File) | Sort-Object Name
$sbomHashLines = foreach ($file in $sbomFiles) {
  $hash = (Get-FileHash $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
  "$hash  $($file.Name)"
}
$sbomHashLines | Set-Content (Join-Path $dist "SBOMSUMS.txt") -Encoding ascii
$provenanceArguments = @{
  ArchiveDir = $archives
  OutputPath = Join-Path $dist "PROVENANCE.intoto.json"
  Version = $version
  Configuration = $Config
  SourceRevision = $sourceRevision
  CommunityRelease = $CommunityRelease
  Production = $Production
}
if ($Production) {
  $provenanceArguments.QualificationRecord = Join-Path $dist "QUALIFICATION.json"
}
& (Join-Path $buildSource "scripts\write-provenance.ps1") @provenanceArguments
& (Join-Path $buildSource "tests\verify-packages.ps1") -DistDir $dist `
  -CommunityRelease:$CommunityRelease -RequireSignatures:$Production

Write-Host "CLI package: $cli"
Write-Host "GUI package: $gui"
Write-Host "WIC codec package: $wic"
Write-Host "SDK package: $sdk"
Write-Host "Web package: $web"
Write-Host "Rust decoder package: $rust"
Write-Host "Source package: $source"
Write-Host "Package archives: $archives"
Write-Host "Checksums: $(Join-Path $dist 'SHA256SUMS.txt')"
Write-Host "SPDX SBOMs: $sbom"
Write-Host "SBOM checksums: $(Join-Path $dist 'SBOMSUMS.txt')"
Write-Host "Provenance: $(Join-Path $dist 'PROVENANCE.intoto.json')"
