param(
  [ValidateSet("Release", "RelWithDebInfo", "MinSizeRel")]
  [string]$Config = "Release",
  [string]$BuildDir = "build\package"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
& (Join-Path $root "scripts\build-clang.ps1") -Config $Config -BuildDir $BuildDir
& (Join-Path $root "scripts\build-web.ps1") -OutDir "web\dist"

$build = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $root $BuildDir }
$qlicBinary = Get-Item (Join-Path $build "qlic.exe") -ErrorAction SilentlyContinue
if (!$qlicBinary) { throw "The QLIC build output was not found under $build." }
$versionOutput = (& $qlicBinary.FullName version | Out-String).Trim()
if ($versionOutput -notmatch '\d+\.\d+\.\d+') { throw "Could not read the QLIC version." }
$version = $Matches[0]

$dist = Join-Path $root "dist"
$cli = Join-Path $dist "qlic-cli"
$gui = Join-Path $dist "qlic-gui"
$wic = Join-Path $dist "qlic-wic"
$sdk = Join-Path $dist "qlic-sdk"
$web = Join-Path $dist "qlic-web"
$archives = Join-Path $dist "archives"
$install = Join-Path $dist "cmake-install"
$consumerBuild = Join-Path $dist "package-consumer-build"
$distPath = [IO.Path]::GetFullPath($dist).TrimEnd(
  [IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
foreach ($path in @($cli,$gui,$wic,$sdk,$web,$archives,$install,$consumerBuild)) {
  $fullPath = [IO.Path]::GetFullPath($path)
  if (!$fullPath.StartsWith($distPath, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to clear a path outside dist: $fullPath"
  }
  Remove-Item -LiteralPath $fullPath -Recurse -Force `
    -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force $cli,$gui,$wic,$sdk,$web | Out-Null

& cmake --install $build --prefix $install --config $Config
if ($LASTEXITCODE -ne 0) { throw "CMake install failed." }

function Copy-Metadata($Destination, [switch]$IncludeReadme) {
  foreach ($name in @("LICENSE", "NOTICE", "SECURITY.md")) {
    Copy-Item (Join-Path $root $name) $Destination -Force
  }
  $lzmsLicense = Join-Path $Destination "third_party\lzms"
  $wimlibNotice = Join-Path $Destination "third_party\wimlib"
  New-Item -ItemType Directory -Force $lzmsLicense,$wimlibNotice | Out-Null
  Copy-Item (Join-Path $root "third_party\lzms\LICENSE") $lzmsLicense -Force
  Copy-Item (Join-Path $root "third_party\wimlib\README.md") $wimlibNotice -Force
  if ($IncludeReadme) {
    Copy-Item (Join-Path $root "README.md") $Destination -Force
    $benchmarkDirectory = Join-Path $Destination "docs"
    New-Item -ItemType Directory -Force $benchmarkDirectory | Out-Null
    Copy-Item (Join-Path $root "docs\benchmark.json") $benchmarkDirectory -Force
  }
}

Copy-Item (Join-Path $build "qlic.exe") $cli -Force
Copy-Item (Join-Path $build "image-codecs") $cli -Recurse -Force
Copy-Metadata $cli -IncludeReadme

Copy-Item (Join-Path $build "qlic.exe") $gui -Force
Copy-Item (Join-Path $build "qlic-gui.exe") $gui -Force
Copy-Item (Join-Path $build "image-codecs") $gui -Recurse -Force
Copy-Metadata $gui -IncludeReadme

Copy-Item (Join-Path $build "qlic-wic.dll") $wic -Force
Copy-Item (Join-Path $root "scripts\install-wic.ps1") $wic -Force
Copy-Item (Join-Path $root "scripts\uninstall-wic.ps1") $wic -Force
Copy-Metadata $wic -IncludeReadme

$sdkBin = Join-Path $sdk "bin"
$sdkLib = Join-Path $sdk "lib"
$sdkInclude = Join-Path $sdk "include"
New-Item -ItemType Directory -Force $sdkBin,$sdkLib,$sdkInclude | Out-Null
Copy-Item (Join-Path $install "bin\qlic.dll") $sdkBin -Force
Copy-Item (Join-Path $install "lib\qlic.lib") $sdkLib -Force
Copy-Item (Join-Path $install "lib\qlic_static.lib") $sdkLib -Force
Copy-Item (Join-Path $install "lib\cmake") $sdkLib -Recurse -Force
Copy-Item (Join-Path $install "include\qlic") $sdkInclude -Recurse -Force
Copy-Item (Join-Path $root "docs") $sdk -Recurse -Force
Copy-Item (Join-Path $root "examples") $sdk -Recurse -Force
Copy-Metadata $sdk -IncludeReadme

$capabilities = & cmake -E capabilities | ConvertFrom-Json
$preferredGenerators = @("Visual Studio 18 2026", "Visual Studio 17 2022")
$availableGenerators = @($capabilities.generators | ForEach-Object { $_.name })
$consumerGenerator = $preferredGenerators |
  Where-Object { $availableGenerators -contains $_ } |
  Select-Object -First 1
if (!$consumerGenerator) {
  throw "Visual Studio 2022 or newer with the C++ workload is required."
}
& cmake -S (Join-Path $root "tests\package_consumer") -B $consumerBuild `
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

Copy-Item (Join-Path $root "web\dist\qlic-web.wasm") $web -Force
Copy-Item (Join-Path $root "web\dist\qlic-web.js") $web -Force
Copy-Item (Join-Path $root "web\dist\qlic-worker.js") $web -Force
Copy-Item (Join-Path $root "web\dist\demo.html") $web -Force
Copy-Item (Join-Path $root "web\dist\README.md") $web -Force
Copy-Item (Join-Path $build "QLIC Demo.exe") $web -Force
Copy-Metadata $web

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
  @{ Path = $gui; Name = "qlic-$version-demo-windows-x64.zip" },
  @{ Path = $wic; Name = "qlic-$version-wic-windows-x64.zip" },
  @{ Path = $sdk; Name = "qlic-$version-sdk-windows-x64.zip" },
  @{ Path = $web; Name = "qlic-$version-web.zip" }
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

Write-Host "CLI package: $cli"
Write-Host "GUI package: $gui"
Write-Host "WIC codec package: $wic"
Write-Host "SDK package: $sdk"
Write-Host "Web package: $web"
Write-Host "Release archives: $archives"
Write-Host "Checksums: $(Join-Path $dist 'SHA256SUMS.txt')"
