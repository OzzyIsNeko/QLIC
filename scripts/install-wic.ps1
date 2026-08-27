param(
  [string]$Dll = "",
  [ValidateSet("Machine")]
  [string]$Scope = "Machine"
)

$ErrorActionPreference = "Stop"
$scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $scriptDirectory
if ($Dll -and !(Test-Path -LiteralPath $Dll -PathType Leaf)) {
  throw "The requested qlic-wic.dll was not found: $Dll"
}
if (!$Dll) {
  $Dll = Join-Path $scriptDirectory "qlic-wic.dll"
  if (!(Test-Path $Dll)) {
    $package = Join-Path $root "dist\qlic-wic\qlic-wic.dll"
    if (Test-Path -LiteralPath $package -PathType Leaf) { $Dll = $package }
  }
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
if (!(Test-Path $Dll)) { throw "qlic-wic.dll was not found. Run .\build.ps1 or pass -Dll." }
$Dll = (Resolve-Path $Dll).Path
$bundleDirectory = Split-Path -Parent $Dll
foreach ($required in @("qlic-gui.exe", "qlic.exe")) {
  if (!(Test-Path -LiteralPath (Join-Path $bundleDirectory $required) `
      -PathType Leaf)) {
    throw "QLIC WIC setup needs the complete bundle; $required is missing beside qlic-wic.dll."
  }
}
$bundleEntries = [Collections.Generic.List[object]]::new()
function Add-BundleFile([string]$Source, [string]$Relative) {
  if (!(Test-Path -LiteralPath $Source -PathType Leaf)) { return }
  $resolved = (Resolve-Path -LiteralPath $Source).Path
  $bundleEntries.Add([pscustomobject]@{
    Source = $resolved
    Relative = $Relative
    Hash = (Get-FileHash -LiteralPath $resolved -Algorithm SHA256).Hash.ToLowerInvariant()
  })
}
Add-BundleFile $Dll "qlic-wic.dll"
Add-BundleFile (Join-Path $bundleDirectory "qlic-gui.exe") "qlic-gui.exe"
Add-BundleFile (Join-Path $bundleDirectory "qlic.exe") "qlic.exe"
$codecDirectory = Join-Path $bundleDirectory "image-codecs"
if (Test-Path -LiteralPath $codecDirectory -PathType Container) {
  $codecPrefix = [IO.Path]::GetFullPath($codecDirectory).TrimEnd(
    [IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
  Get-ChildItem -LiteralPath $codecDirectory -File -Recurse |
    Sort-Object FullName | ForEach-Object {
      $relative = $_.FullName.Substring($codecPrefix.Length)
      Add-BundleFile $_.FullName (Join-Path "image-codecs" $relative)
    }
}
$records = @($bundleEntries | Sort-Object Relative | ForEach-Object {
  "$($_.Hash)  $($_.Relative.Replace('\', '/'))"
})
$manifestBytes = [Text.UTF8Encoding]::new($false).GetBytes(
  ([string]::Join("`n", $records) + "`n"))
$sha = [Security.Cryptography.SHA256]::Create()
try {
  $bundleHash = ([BitConverter]::ToString(
    $sha.ComputeHash($manifestBytes))).Replace("-", "").ToLowerInvariant()
} finally {
  $sha.Dispose()
}
$classId = "{5CE9F7D8-140B-43FC-8762-B8E72FF6B765}"
$classKey =
  "Registry::HKEY_LOCAL_MACHINE\Software\Classes\CLSID\$classId\InProcServer32"
function Remove-StaleManagedBundles([string]$CurrentDll, [string]$ManagedBase) {
  if (!(Test-Path -LiteralPath $ManagedBase -PathType Container)) { return }
  $managedPath = [IO.Path]::GetFullPath($ManagedBase)
  $currentDirectory = [IO.Path]::GetFullPath((Split-Path -Parent $CurrentDll))
  foreach ($directory in @(Get-ChildItem -LiteralPath $managedPath -Directory)) {
    $directoryPath = [IO.Path]::GetFullPath($directory.FullName)
    if ([IO.Path]::GetFullPath((Split-Path -Parent $directoryPath)) -ne
          $managedPath -or
        $directory.Name -notmatch '^\d+\.\d+\.\d+-[0-9a-f]{12}$' -or
        $directoryPath -eq $currentDirectory) {
      continue
    }
    try {
      Remove-Item -LiteralPath $directoryPath -Recurse -Force
    } catch {
      Write-Warning "An older QLIC bundle could not be removed: $directoryPath"
    }
  }
}

$isAdmin = ([Security.Principal.WindowsPrincipal] `
  [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
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
  Write-Host "QLIC WIC setup needs administrator permission."
  Write-Host "Approve the Windows prompt to install Explorer and app support."
  try {
    $elevated = Start-Process -FilePath $hostExecutable -Verb RunAs `
      -ArgumentList $arguments -WindowStyle Hidden -Wait -PassThru
  } catch {
    throw "Administrator permission was not granted. QLIC WIC was not changed."
  }
  if ($elevated.ExitCode -ne 0) {
    throw "QLIC WIC setup failed in the elevated installer (exit code $($elevated.ExitCode))."
  }
  if (!(Test-Path -LiteralPath $classKey)) {
    throw "QLIC WIC setup finished without creating the machine registration."
  }
  $registered = (Get-ItemProperty -LiteralPath $classKey).'(default)'
  if (!$registered -or !(Test-Path -LiteralPath $registered -PathType Leaf) -or
      (Get-FileHash -LiteralPath $registered -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath $Dll -Algorithm SHA256).Hash) {
    throw "QLIC WIC setup could not verify the registered decoder."
  }
  Write-Host "QLIC WIC is installed and verified."
  Write-Host "Registered decoder: $registered"
  Write-Host "Explorer and WIC-aware apps can now open .qlic images."
  return
}

$previousDll = $null
if (Test-Path -LiteralPath $classKey) {
  $previousDll = (Get-ItemProperty -LiteralPath $classKey).'(default)'
  if ($previousDll) { $previousDll = [IO.Path]::GetFullPath($previousDll) }
}

$installedDll = $Dll
$versionInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($Dll)
$version = $versionInfo.ProductVersion
if (!$version -or $version -notmatch '^\d+\.\d+\.\d+') {
  throw "qlic-wic.dll does not contain a valid product version."
}
$version = $Matches[0]
$installBase = Join-Path $env:ProgramFiles "QLIC\WIC"
$installedDirectory = Join-Path $installBase "$version-$($bundleHash.Substring(0, 12))"
$installedDll = Join-Path $installedDirectory "qlic-wic.dll"
$installedDirectoryCreated = !(Test-Path -LiteralPath $installedDirectory)
New-Item -ItemType Directory -Force $installedDirectory | Out-Null
$installedPrefix = [IO.Path]::GetFullPath($installedDirectory).TrimEnd(
  [IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
foreach ($entry in $bundleEntries) {
  $destination = [IO.Path]::GetFullPath(
    (Join-Path $installedDirectory $entry.Relative))
  if (!$destination.StartsWith($installedPrefix,
      [StringComparison]::OrdinalIgnoreCase)) {
    throw "A QLIC bundle entry escapes the installation directory: $($entry.Relative)"
  }
  $destinationIsCurrent =
    (Test-Path -LiteralPath $destination -PathType Leaf) -and
    ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant() `
      -eq $entry.Hash)
  if ($destinationIsCurrent) { continue }
  $parent = Split-Path -Parent $destination
  New-Item -ItemType Directory -Force $parent | Out-Null
  $temporary = "$destination.new"
  Copy-Item -LiteralPath $entry.Source -Destination $temporary -Force
  if ((Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash.ToLowerInvariant() `
      -ne $entry.Hash) {
    Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
    throw "A staged QLIC bundle file failed its copy-integrity check: $($entry.Relative)"
  }
  Move-Item -LiteralPath $temporary -Destination $destination -Force
}

$regsvr = Join-Path $env:WINDIR "System32\regsvr32.exe"
if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
  $regsvr = Join-Path $env:WINDIR "Sysnative\regsvr32.exe"
}

$installScope = "machine"
try {
  $p = Start-Process -FilePath $regsvr -WindowStyle Hidden `
    -ArgumentList "/s /n /i:$installScope `"$installedDll`"" -Wait -PassThru
  if ($p.ExitCode -ne 0) {
    throw "regsvr32 failed with exit code $($p.ExitCode)"
  }
  $registered = (Get-ItemProperty -LiteralPath $classKey).'(default)'
  if ([IO.Path]::GetFullPath($registered) -ne
      [IO.Path]::GetFullPath($installedDll)) {
    throw "WIC registration verification failed."
  }
} catch {
  if ($previousDll -and (Test-Path -LiteralPath $previousDll)) {
    $rollback = Start-Process -FilePath $regsvr -WindowStyle Hidden `
      -ArgumentList "/s /n /i:$installScope `"$previousDll`"" -Wait -PassThru
    if ($rollback.ExitCode -ne 0) {
      Write-Warning "The previous WIC registration could not be restored."
    }
  }
  if ($installedDirectory -and $installedDirectoryCreated) {
    Remove-Item -LiteralPath $installedDirectory -Recurse -Force `
      -ErrorAction SilentlyContinue
  }
  throw
}

Remove-StaleManagedBundles $installedDll $installBase
Write-Host "QLIC WIC is installed and verified for this computer."
Write-Host "Registered decoder: $installedDll"
