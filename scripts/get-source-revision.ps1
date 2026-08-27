param(
  [Parameter(Mandatory = $true)]
  [string]$SourceDir
)

$ErrorActionPreference = "Stop"
$source = (Resolve-Path -LiteralPath $SourceDir).Path
$packageMarkers = @(
  (Join-Path $source "UNSIGNED-DEVELOPMENT-BUILD.txt"),
  (Join-Path $source "UNSIGNED-COMMUNITY-RELEASE.txt")
)
$excludedRoots = @(
  (Join-Path $source "build"),
  (Join-Path $source "dist"),
  (Join-Path $source "release"),
  (Join-Path $source "rust\qlic-decoder\target")
) | ForEach-Object {
  [IO.Path]::GetFullPath($_).TrimEnd([IO.Path]::DirectorySeparatorChar) +
    [IO.Path]::DirectorySeparatorChar
}
$records = [Collections.Generic.List[string]]::new()
$files = @(Get-ChildItem -LiteralPath $source -File -Recurse | Where-Object {
  $full = $_.FullName
  $excluded = $false
  foreach ($root in $excludedRoots) {
    if ($full.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
      $excluded = $true
      break
    }
  }
  !$excluded -and
    !($packageMarkers -contains $full) -and
    $full -notmatch '[\\/]build[\\/]' -and
    $full -notmatch '[\\/]web[\\/]dist(?:-[^\\/]+)?[\\/]' -and
    $full -notmatch '[\\/]__pycache__[\\/]'
})
foreach ($file in $files) {
  $relative = [IO.Path]::GetRelativePath($source, $file.FullName).Replace("\", "/")
  $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
  $records.Add("$hash  $relative")
}
$records.Sort([StringComparer]::Ordinal)
$manifest = [string]::Join("`n", $records) + "`n"
$algorithm = [Security.Cryptography.SHA256]::Create()
try {
  $digest = $algorithm.ComputeHash([Text.UTF8Encoding]::new($false).GetBytes($manifest))
} finally {
  $algorithm.Dispose()
}
[Convert]::ToHexString($digest).ToLowerInvariant()
