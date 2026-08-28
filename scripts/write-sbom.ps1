param(
  [Parameter(Mandatory = $true)]
  [string]$PackageDir,
  [Parameter(Mandatory = $true)]
  [string]$PackageName,
  [Parameter(Mandatory = $true)]
  [string]$PackageVersion,
  [Parameter(Mandatory = $true)]
  [string]$OutputPath,
  [string]$License = "Apache-2.0 AND MIT"
)

$ErrorActionPreference = "Stop"
$package = (Resolve-Path -LiteralPath $PackageDir).Path
$output = [IO.Path]::GetFullPath($OutputPath)
$files = @(Get-ChildItem -LiteralPath $package -File -Recurse |
    Where-Object { $_.FullName -ne $output } |
    Sort-Object FullName)
if ($files.Count -eq 0) {
  throw "Cannot make an SBOM for an empty package: $package"
}

$spdxFiles = [Collections.Generic.List[object]]::new()
$relationships = [Collections.Generic.List[object]]::new()
$sha1Values = [Collections.Generic.List[string]]::new()
for ($index = 0; $index -lt $files.Count; $index++) {
  $file = $files[$index]
  $relative = [IO.Path]::GetRelativePath($package, $file.FullName).Replace("\", "/")
  $sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
  $sha1 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA1).Hash.ToLowerInvariant()
  $sha1Values.Add($sha1)
  $id = "SPDXRef-File-{0:D6}" -f ($index + 1)
  $spdxFiles.Add([ordered]@{
    SPDXID = $id
    fileName = "./$relative"
    checksums = @(
      [ordered]@{ algorithm = "SHA1"; checksumValue = $sha1 },
      [ordered]@{ algorithm = "SHA256"; checksumValue = $sha256 }
    )
    licenseConcluded = "NOASSERTION"
    licenseInfoInFiles = @("NOASSERTION")
    copyrightText = "NOASSERTION"
  })
  $relationships.Add([ordered]@{
    spdxElementId = "SPDXRef-Package-QLIC"
    relationshipType = "CONTAINS"
    relatedSpdxElement = $id
  })
}

$verificationText = ($sha1Values | Sort-Object) -join ""
$verificationBytes = [Text.Encoding]::ASCII.GetBytes($verificationText)
$verificationHash = [Security.Cryptography.SHA1]::HashData($verificationBytes)
$verification = [Convert]::ToHexString($verificationHash).ToLowerInvariant()
$escapedName = [Uri]::EscapeDataString($PackageName)
$namespace = "https://qlic.invalid/spdx/$escapedName/$PackageVersion/$verification"

$allRelationships = [Collections.Generic.List[object]]::new()
$allRelationships.Add([ordered]@{
  spdxElementId = "SPDXRef-DOCUMENT"
  relationshipType = "DESCRIBES"
  relatedSpdxElement = "SPDXRef-Package-QLIC"
})
foreach ($relationship in $relationships) {
  $allRelationships.Add($relationship)
}

$document = [ordered]@{
  spdxVersion = "SPDX-2.3"
  dataLicense = "CC0-1.0"
  SPDXID = "SPDXRef-DOCUMENT"
  name = "$PackageName SBOM"
  documentNamespace = $namespace
  creationInfo = [ordered]@{
    created = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
    creators = @("Tool: qlic-write-sbom-1.0")
  }
  packages = @(
    [ordered]@{
      SPDXID = "SPDXRef-Package-QLIC"
      name = $PackageName
      versionInfo = $PackageVersion
      downloadLocation = "NOASSERTION"
      filesAnalyzed = $true
      packageVerificationCode = [ordered]@{
        packageVerificationCodeValue = $verification
      }
      licenseConcluded = "NOASSERTION"
      licenseDeclared = $License
      licenseInfoFromFiles = @("NOASSERTION")
      copyrightText = "Copyright 2026 Ozzy M."
    }
  )
  files = @($spdxFiles)
  relationships = @($allRelationships)
}

$parent = Split-Path -Parent $output
New-Item -ItemType Directory -Force $parent | Out-Null
$document | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $output -Encoding utf8
Write-Host "SPDX SBOM: $output"
