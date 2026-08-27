param(
  [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
  [string]$Config = "Release",
  [string]$BuildDir = "build",
  [ValidateSet("x64", "ARM64")]
  [string]$Architecture = "x64",
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
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$cmake = Get-Command cmake -ErrorAction Stop
$capabilities = & $cmake.Source -E capabilities | ConvertFrom-Json
$preferred = @(
  @{ Name = "Visual Studio 18 2026"; Dir = "vs18" },
  @{ Name = "Visual Studio 17 2022"; Dir = "vs17" }
)
$available = @($capabilities.generators | ForEach-Object { $_.name })
$generator = $preferred | Where-Object { $available -contains $_.Name } | Select-Object -First 1

if (!$generator) {
  throw "Visual Studio 2022 or newer with the C++ workload is required."
}

$base = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $root $BuildDir }
$out = Join-Path $base "$($generator.Dir)-$($Architecture.ToLowerInvariant())"
& $cmake.Source -S $root -B $out -G $generator.Name -A $Architecture `
  -D BUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }
& $cmake.Source --build $out --config $Config --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

if (!$SkipTests) {
  & ctest --test-dir $out -C $Config --output-on-failure
  if ($LASTEXITCODE -ne 0) { throw "Tests failed." }
}

Write-Host "Build output: $(Join-Path $out $Config)"
