param(
  [ValidateSet("Debug", "Release")]
  [string]$Config = "Release",
  [switch]$SkipTests
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$manifest = Join-Path $root "rust\qlic-decoder\Cargo.toml"
$cargo = (Get-Command cargo.exe -ErrorAction Stop).Source

& $cargo fmt --manifest-path $manifest --check
if ($LASTEXITCODE -ne 0) { throw "Rust formatting check failed." }

$profileArguments = if ($Config -eq "Release") { @("--release") } else { @() }
$clippyArguments = @("clippy", "--manifest-path", $manifest) +
  $profileArguments + @("--all-targets", "--", "-Dwarnings")
& $cargo @clippyArguments
if ($LASTEXITCODE -ne 0) { throw "Rust lint check failed." }

if ($SkipTests) {
  $buildArguments = @("build", "--manifest-path", $manifest) +
    $profileArguments
  & $cargo @buildArguments
  if ($LASTEXITCODE -ne 0) { throw "Rust build failed." }
} else {
  $testArguments = @("test", "--manifest-path", $manifest) +
    $profileArguments
  & $cargo @testArguments
  if ($LASTEXITCODE -ne 0) { throw "Rust tests failed." }
}

Write-Host "Rust decoder: $(Split-Path -Parent $manifest)"
