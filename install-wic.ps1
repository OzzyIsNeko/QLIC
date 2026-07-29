param(
  [string]$Dll = "",
  [ValidateSet("User", "Machine")]
  [string]$Scope = "User"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (!$Dll) { $Dll = Join-Path $root "qlic-wic.dll" }
if (!(Test-Path $Dll)) {
  $built = Get-ChildItem (Join-Path $root "build") -Filter qlic-wic.dll -File -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.Directory.Name -eq "Release" } |
    Sort-Object LastWriteTimeUtc -Descending |
    Select-Object -First 1
  if ($built) { $Dll = $built.FullName }
}
if (!(Test-Path $Dll)) { throw "qlic-wic.dll was not found. Run .\build.ps1 or pass -Dll." }
$Dll = (Resolve-Path $Dll).Path
$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if ($Scope -eq "Machine" -and !$isAdmin) {
  throw "Machine-wide installation requires an elevated PowerShell."
}

$regsvr = Join-Path $env:WINDIR "System32\regsvr32.exe"
if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
  $regsvr = Join-Path $env:WINDIR "Sysnative\regsvr32.exe"
}

$installScope = $Scope.ToLowerInvariant()
$p = Start-Process -FilePath $regsvr `
  -ArgumentList "/s /n /i:$installScope `"$Dll`"" -Wait -PassThru
if ($p.ExitCode -ne 0) { throw "regsvr32 failed with exit code $($p.ExitCode)" }

Write-Host "Installed the QLIC WIC decoder for scope $Scope."
Write-Host $Dll
