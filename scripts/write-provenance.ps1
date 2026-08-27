param(
  [Parameter(Mandatory = $true)]
  [string]$ArchiveDir,
  [Parameter(Mandatory = $true)]
  [string]$OutputPath,
  [Parameter(Mandatory = $true)]
  [string]$Version,
  [ValidateSet("Release", "RelWithDebInfo", "MinSizeRel")]
  [string]$Configuration = "Release",
  [Parameter(Mandatory = $true)]
  [ValidatePattern('^[0-9a-fA-F]{64}$')]
  [string]$SourceRevision,
  [switch]$CommunityRelease,
  [switch]$Production,
  [string]$QualificationRecord = ""
)

$ErrorActionPreference = "Stop"
if ($CommunityRelease -and $Production) {
  throw "Provenance cannot be both community and production."
}
$releaseChannel = if ($Production) {
  "production"
} elseif ($CommunityRelease) {
  "community"
} else {
  "development"
}
$archives = @(Get-ChildItem -LiteralPath $ArchiveDir -Filter *.zip -File |
  Sort-Object Name)
if (!$archives.Count) { throw "No archives were found for provenance." }
$qualification = $null
if ($Production) {
  if (!$QualificationRecord) {
    throw "Production provenance requires a qualification record."
  }
  $qualificationPath = (Resolve-Path -LiteralPath $QualificationRecord).Path
  $qualificationJson = Get-Content -Raw -LiteralPath $qualificationPath |
    ConvertFrom-Json
  $qualification = [ordered]@{
    sha256 = (Get-FileHash -LiteralPath $qualificationPath -Algorithm SHA256).Hash.ToLowerInvariant()
    sourceRevision = $qualificationJson.source_revision.ToLowerInvariant()
  }
}
$subjects = foreach ($archive in $archives) {
  [ordered]@{
    name = $archive.Name
    digest = [ordered]@{
      sha256 = (Get-FileHash -LiteralPath $archive.FullName `
        -Algorithm SHA256).Hash.ToLowerInvariant()
    }
  }
}
$statement = [ordered]@{
  _type = "https://in-toto.io/Statement/v1"
  subject = @($subjects)
  predicateType = "https://slsa.dev/provenance/v1"
  predicate = [ordered]@{
    buildDefinition = [ordered]@{
      buildType = "https://qlic.dev/build/package-ps1/v1"
      externalParameters = [ordered]@{
        version = $Version
        configuration = $Configuration
        releaseChannel = $releaseChannel
        production = [bool]$Production
        qualification = $qualification
      }
      internalParameters = [ordered]@{}
      resolvedDependencies = @(
        [ordered]@{
          uri = "https://qlic.dev/source-tree"
          digest = [ordered]@{ sha256 = $SourceRevision.ToLowerInvariant() }
        }
      )
    }
    runDetails = [ordered]@{
      builder = [ordered]@{ id = "https://qlic.dev/builders/windows-package/v1" }
      metadata = [ordered]@{
        invocationId = [guid]::NewGuid().ToString("D")
        startedOn = [DateTime]::UtcNow.ToString("o")
      }
      byproducts = @()
    }
  }
}
$json = $statement | ConvertTo-Json -Depth 20
[IO.File]::WriteAllText([IO.Path]::GetFullPath($OutputPath), $json + "`n",
  [Text.UTF8Encoding]::new($false))
Write-Host "SLSA/in-toto provenance: $OutputPath"
