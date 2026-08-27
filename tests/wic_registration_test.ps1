param(
  [Parameter(Mandatory = $true)]
  [string]$Dll,
  [Parameter(Mandatory = $true)]
  [string]$Smoke,
  [Parameter(Mandatory = $true)]
  [string]$Fixture
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$dllPath = (Resolve-Path -LiteralPath $Dll).Path
$smokePath = (Resolve-Path -LiteralPath $Smoke).Path
$fixturePath = (Resolve-Path -LiteralPath $Fixture).Path
$isAdmin = ([Security.Principal.WindowsPrincipal] `
  [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (!$isAdmin) {
  throw "The system WIC discovery test requires an elevated process."
}
$classId = "{5CE9F7D8-140B-43FC-8762-B8E72FF6B765}"
$categoryId = "{7ED96837-96F0-4812-B211-F13C24117ED3}"
$classKey = "Registry::HKEY_LOCAL_MACHINE\Software\Classes\CLSID\$classId"
$categoryKey = "Registry::HKEY_LOCAL_MACHINE\Software\Classes\CLSID\$categoryId\Instance\$classId"
$extensionKey = "Registry::HKEY_LOCAL_MACHINE\Software\Classes\.qlic"
$typeKey = "Registry::HKEY_LOCAL_MACHINE\Software\Classes\QLIC.Image"
$systemAssociationKey = "Registry::HKEY_LOCAL_MACHINE\Software\Classes\SystemFileAssociations\.qlic"
$thumbnailId = "{E357FCCD-A995-4576-B01F-234630154E96}"
$thumbnailProvider = "{C7657C4A-9F68-40FA-A4DF-96BC08EB3551}"
$previewProvider = "{FFE2A43C-56B9-4BF5-9A79-CC6D4285608A}"
$kindMap = "Registry::HKEY_LOCAL_MACHINE\Software\Microsoft\Windows\CurrentVersion\Explorer\KindMap"
$keys = @($classKey, $categoryKey, $extensionKey, $typeKey, $systemAssociationKey)
$server = $null

foreach ($key in $keys) {
  if (Test-Path -LiteralPath $key) {
    throw "Refusing to replace an existing QLIC registration: $key"
  }
}

$installed = $false
try {
  & (Join-Path $root "scripts\install-wic.ps1") -Dll $dllPath -Scope Machine
  $installed = $true
  foreach ($key in $keys) {
    if (!(Test-Path -LiteralPath $key)) {
      throw "WIC installation did not create $key"
    }
  }
  $server = (Get-ItemProperty -LiteralPath (Join-Path $classKey "InProcServer32")).'(default)'
  if ([IO.Path]::GetFullPath($server) -eq $dllPath -or
      !(Test-Path -LiteralPath $server -PathType Leaf)) {
    throw "WIC installation did not stage a stable DLL: $server"
  }
  if ((Get-FileHash -LiteralPath $server -Algorithm SHA256).Hash -ne
      (Get-FileHash -LiteralPath $dllPath -Algorithm SHA256).Hash) {
    throw "The stable WIC DLL differs from the package DLL."
  }
  $threading = (Get-ItemProperty -LiteralPath (Join-Path $classKey "InProcServer32")).ThreadingModel
  if ($threading -ne "Both") {
    throw "WIC registration has the wrong threading model: $threading"
  }
  foreach ($thumbnailKey in @(
    (Join-Path $extensionKey "ShellEx\$thumbnailId"),
    (Join-Path $systemAssociationKey "ShellEx\$thumbnailId")
  )) {
    $provider = (Get-ItemProperty -LiteralPath $thumbnailKey).'(default)'
    if ($provider -ine $thumbnailProvider) {
      throw "WIC thumbnail integration is missing at $thumbnailKey"
    }
  }
  foreach ($previewKey in @(
    (Join-Path $extensionKey "ShellEx\ContextMenuHandlers\ShellImagePreview"),
    (Join-Path $systemAssociationKey "ShellEx\ContextMenuHandlers\ShellImagePreview")
  )) {
    $provider = (Get-ItemProperty -LiteralPath $previewKey).'(default)'
    if ($provider -ine $previewProvider) {
      throw "WIC shell preview integration is missing at $previewKey"
    }
  }
  if ((Get-ItemProperty -LiteralPath $kindMap).'.qlic' -ne "Picture") {
    throw "The .qlic extension was not mapped to System.Kind Picture."
  }
  $openCommand = (Get-ItemProperty -LiteralPath `
    (Join-Path $typeKey "shell\open\command")).'(default)'
  $qlicViewer = Join-Path (Split-Path -Parent $server) "qlic-gui.exe"
  $photoViewer = Join-Path $env:ProgramFiles "Windows Photo Viewer\PhotoViewer.dll"
  $viewerRequirements = if (Test-Path -LiteralPath $qlicViewer -PathType Leaf) {
    @($qlicViewer, '"%1"')
  } elseif (Test-Path -LiteralPath $photoViewer -PathType Leaf) {
    @("rundll32.exe", $photoViewer, "ImageView_Fullscreen", "%1")
  } else {
    @("rundll32.exe", $server, "OpenInPhotos", '"%1"')
  }
  foreach ($required in $viewerRequirements) {
    if ($openCommand.IndexOf($required,
        [StringComparison]::OrdinalIgnoreCase) -lt 0) {
      throw "The QLIC original-file viewer command is incomplete: $openCommand"
    }
  }
  if ((Test-Path -LiteralPath $qlicViewer -PathType Leaf) -and
      !$openCommand.Contains('"%1"')) {
    throw "The QLIC GUI command must quote the original file path: $openCommand"
  }
  if (!(Test-Path -LiteralPath $qlicViewer -PathType Leaf) -and
      (Test-Path -LiteralPath $photoViewer -PathType Leaf) -and
      $openCommand.Contains('"%1"')) {
    throw "Windows Photo Viewer requires its native unquoted %1 argument: $openCommand"
  }
  $photosCommand = (Get-ItemProperty -LiteralPath `
    (Join-Path $typeKey "shell\photos\command")).'(default)'
  $photosVerb = (Get-ItemProperty -LiteralPath `
    (Join-Path $typeKey "shell\photos")).MuiVerb
  if ($photosVerb -ne "Open in Microsoft Photos (compatibility)") {
    throw "The Photos alias path is not labeled as compatibility-only: $photosVerb"
  }
  foreach ($required in @("rundll32.exe", $server, "OpenInPhotos", '"%1"')) {
    if ($photosCommand.IndexOf($required,
        [StringComparison]::OrdinalIgnoreCase) -lt 0) {
      throw "The optional Photos compatibility command is incomplete: $photosCommand"
    }
  }
  if (Test-Path -LiteralPath (Join-Path $typeKey "shell\open\DropTarget")) {
    throw "The obsolete Windows Photo Viewer DropTarget remains registered."
  }
  $openWith = Get-ItemProperty -LiteralPath (Join-Path $extensionKey "OpenWithProgids")
  if ($openWith.PSObject.Properties.Name -contains
      "AppX4mntx4h978m1v9gtzv0ewksfd6pmwsre") {
    throw "The broken direct Microsoft Photos association remains registered."
  }
  $signature = Get-AuthenticodeSignature -LiteralPath $dllPath
  $expectedFlags = if ($signature.Status -eq "Valid") { 1 } else { 2 }
  $flags = (Get-ItemProperty -LiteralPath $classKey).Flags
  if ($flags -ne $expectedFlags) {
    throw "WIC signing flags $flags disagree with Authenticode status $($signature.Status)."
  }
  & $smokePath --catalog $fixturePath
  if ($LASTEXITCODE -ne 0) {
    throw "The registered WIC catalog could not instantiate the decoder."
  }
  & $smokePath $fixturePath
  if ($LASTEXITCODE -ne 0) {
    throw "The registered WIC decoder was not discoverable."
  }
  # A second install is the repair path and must remain idempotent.
  & (Join-Path $root "scripts\install-wic.ps1") -Dll $dllPath -Scope Machine
  $repaired = (Get-ItemProperty -LiteralPath `
    (Join-Path $classKey "InProcServer32")).'(default)'
  if ([IO.Path]::GetFullPath($repaired) -ne [IO.Path]::GetFullPath($server)) {
    throw "WIC repair changed the content-addressed installation path."
  }
} finally {
  if ($installed) {
    & (Join-Path $root "scripts\uninstall-wic.ps1") -Scope Machine
  }
}

foreach ($key in $keys) {
  if (Test-Path -LiteralPath $key) {
    throw "WIC uninstallation left a registration behind: $key"
  }
}
if ((Get-ItemProperty -LiteralPath $kindMap -ErrorAction SilentlyContinue).'.qlic') {
  throw "WIC uninstallation left the .qlic KindMap value behind."
}
if ($server -and (Test-Path -LiteralPath $server)) {
  throw "WIC uninstallation left the managed DLL behind: $server"
}
Write-Host "QLIC machine-wide WIC install, discovery, decode, and uninstall passed."
