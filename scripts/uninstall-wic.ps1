param(
  [string]$Dll = "",
  [ValidateSet("Machine")]
  [string]$Scope = "Machine"
)

$ErrorActionPreference = "Stop"
$scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $scriptDirectory
$classId = "{5CE9F7D8-140B-43FC-8762-B8E72FF6B765}"
$classKey =
  "Registry::HKEY_LOCAL_MACHINE\Software\Classes\CLSID\$classId\InProcServer32"
if (!$Dll -and (Test-Path -LiteralPath $classKey)) {
  $Dll = (Get-ItemProperty -LiteralPath $classKey).'(default)'
}
if (!$Dll) {
  $Dll = Join-Path $scriptDirectory "qlic-wic.dll"
  if (!(Test-Path $Dll)) {
    $installed = [IO.Path]::GetFullPath(
      (Join-Path $scriptDirectory "..\..\bin\qlic-wic.dll"))
    if (Test-Path $installed) { $Dll = $installed }
  }
}
if (!(Test-Path $Dll)) {
  $built = Get-ChildItem (Join-Path $root "build") -Filter qlic-wic.dll -File -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.Directory.Name -eq "Release" } |
    Sort-Object LastWriteTimeUtc -Descending |
    Select-Object -First 1
  if ($built) { $Dll = $built.FullName }
}
if (!(Test-Path $Dll)) { throw "qlic-wic.dll was not found. Pass -Dll C:\path\qlic-wic.dll." }
$Dll = (Resolve-Path $Dll).Path
$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (!$isAdmin) {
  $hostExecutable = if ($PSVersionTable.PSEdition -eq "Core") {
    Join-Path $PSHOME "pwsh.exe"
  } else {
    Join-Path $PSHOME "powershell.exe"
  }
  $scriptPath = (Resolve-Path -LiteralPath $MyInvocation.MyCommand.Path).Path
  $arguments = @(
    "-NoProfile",
    "-NonInteractive",
    "-ExecutionPolicy", "Bypass",
    "-File", ('"' + $scriptPath + '"'),
    "-Dll", ('"' + $Dll + '"'),
    "-Scope", $Scope
  )
  Write-Host "QLIC WIC removal needs administrator permission."
  Write-Host "Approve the Windows prompt to remove Explorer and app support."
  try {
    $elevated = Start-Process -FilePath $hostExecutable -Verb RunAs `
      -ArgumentList $arguments -WindowStyle Hidden -Wait -PassThru
  } catch {
    throw "Administrator permission was not granted. QLIC WIC was not changed."
  }
  if ($elevated.ExitCode -ne 0) {
    throw "QLIC WIC removal failed in the elevated uninstaller (exit code $($elevated.ExitCode))."
  }
  if (Test-Path -LiteralPath $classKey) {
    throw "QLIC WIC removal finished but the machine registration remains."
  }
  Write-Host "QLIC WIC was removed and verified."
  return
}

$regsvr = Join-Path $env:WINDIR "System32\regsvr32.exe"
if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
  $regsvr = Join-Path $env:WINDIR "Sysnative\regsvr32.exe"
}

$installScope = "machine"
$registeredDll = $Dll
$p = Start-Process -FilePath $regsvr -WindowStyle Hidden `
  -ArgumentList "/u /s /n /i:$installScope `"$Dll`"" -Wait -PassThru
if ($p.ExitCode -ne 0) { throw "regsvr32 unregister failed with exit code $($p.ExitCode)" }

if (Test-Path -LiteralPath $classKey) {
  throw "QLIC WIC registration remains after uninstall."
}
$managedBase = Join-Path $env:ProgramFiles "QLIC\WIC"
$managedPrefix = [IO.Path]::GetFullPath($managedBase).TrimEnd(
  [IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
$registeredPath = [IO.Path]::GetFullPath($registeredDll)
if ($registeredPath.StartsWith($managedPrefix,
    [StringComparison]::OrdinalIgnoreCase)) {
  $registeredDirectory = Split-Path -Parent $registeredPath
  $registeredDirectoryPath = [IO.Path]::GetFullPath($registeredDirectory)
  $leaf = Split-Path -Leaf $registeredDirectoryPath
  $parent = [IO.Path]::GetFullPath((Split-Path -Parent $registeredDirectoryPath))
  if ($parent -ne [IO.Path]::GetFullPath($managedBase) -or
      $leaf -notmatch '^\d+\.\d+\.\d+-[0-9a-f]{12}$' -or
      $registeredPath -ne
        [IO.Path]::GetFullPath((Join-Path $registeredDirectoryPath "qlic-wic.dll"))) {
    throw "Refusing to remove an unexpected managed QLIC directory: $registeredDirectoryPath"
  }
  Remove-Item -LiteralPath $registeredDirectoryPath -Recurse -Force
}

Write-Host "Removed the QLIC WIC decoder for scope $Scope."
