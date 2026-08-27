param(
  [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
  [string]$Config = "Release",
  [string]$BuildDir = "build\clang-nmake",
  [ValidateSet("Off", "Generate", "Use")]
  [string]$Pgo = "Off",
  [string]$PgoProfile = "",
  [switch]$Native,
  [switch]$StreamTrace,
  [switch]$BenchmarkTrial,
  [switch]$Sanitize,
  [switch]$SkipTests
)

$ErrorActionPreference = "Stop"
$processEnvironment = [Environment]::GetEnvironmentVariables()
# Windows can expose Path and PATH together, normalize them before invoking CMake
$pathKeys = @($processEnvironment.Keys | Where-Object { $_ -ieq "Path" })
if ($pathKeys.Count -gt 1) {
  $pathValue = [string]$processEnvironment[$pathKeys[0]]
  foreach ($pathKey in $pathKeys) {
    [Environment]::SetEnvironmentVariable(
      [string]$pathKey, $null, [EnvironmentVariableTarget]::Process)
  }
  [Environment]::SetEnvironmentVariable(
    "Path", $pathValue, [EnvironmentVariableTarget]::Process)
}
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$out = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $root $BuildDir }
$clangCommand = Get-Command clang-cl.exe -ErrorAction SilentlyContinue
$clang = if ($clangCommand) { $clangCommand.Source } else { Join-Path $env:ProgramFiles "LLVM\bin\clang-cl.exe" }
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"

if (!(Test-Path $clang)) {
  throw "clang-cl.exe was not found. Install LLVM and add it to PATH."
}
if (!(Test-Path $vswhere)) {
  throw "vswhere.exe was not found. Install Visual Studio with the C++ workload."
}

$installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $installation "VC\Auxiliary\Build\vcvars64.bat"
if (!$installation -or !(Test-Path $vcvars)) {
  throw "Visual Studio C++ build tools were not found."
}

$testCommand = if ($SkipTests) { "" } else { " && ctest --test-dir `"$out`" -C `"$Config`" --output-on-failure" }
$sanitizeOption = if ($Sanitize) { "ON" } else { "OFF" }
$pgoOption = $Pgo.ToUpperInvariant()
$profileOption = if ($PgoProfile) { " -D QLIC_PGO_PROFILE=`"$PgoProfile`"" } else { "" }
$nativeOption = if ($Native) { "ON" } else { "OFF" }
$streamTraceOption = if ($StreamTrace) { "ON" } else { "OFF" }
$benchmarkTrialOption = if ($BenchmarkTrial) { "ON" } else { "OFF" }
$command = "call `"$vcvars`" >nul && cmake -S `"$root`" -B `"$out`" -G `"NMake Makefiles`" -D CMAKE_C_COMPILER=`"$clang`" -D CMAKE_BUILD_TYPE=`"$Config`" -D BUILD_TESTING=ON -D QLIC_SANITIZE=$sanitizeOption -D QLIC_PGO=$pgoOption -D QLIC_NATIVE=$nativeOption -D QLIC_STREAM_TRACE=$streamTraceOption -D QLIC_BENCHMARK_TRIAL=$benchmarkTrialOption$profileOption && cmake --build `"$out`" --parallel$testCommand"
cmd /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "Clang build or tests failed." }

Write-Host "Build output: $out"
