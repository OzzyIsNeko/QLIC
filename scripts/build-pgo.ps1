param(
  [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
  [string]$Config = "Release",
  [string]$ReferenceBuildDir = "build\pgo-reference",
  [string]$ReferenceQlic = "",
  [string]$InstrumentedBuildDir = "build\pgo-generate",
  [string]$OutputBuildDir = "build\pgo-use",
  [string]$ProfileDir = "build\pgo-profile",
  [string]$Results = "",
  [string]$Corpus = "",
  [ValidateRange(1, 100)]
  [int]$ImagesPerCategory = 10,
  [ValidateSet("FullCorpus", "BalancedMode52", "AllP1Mode52")]
  [string]$TrainingSet = "FullCorpus",
  [switch]$Native
)

$ErrorActionPreference = "Stop"
$fullCorpus = $TrainingSet -eq "FullCorpus"
$allMode52 = $TrainingSet -eq "AllP1Mode52"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$repo = Split-Path -Parent $root
$buildClang = Join-Path $root "scripts\build-clang.ps1"
$decodeTrainer = Join-Path $repo `
  "benchmarks_and_tools\bin\qlic-decode-dll-bench.exe"
$decodeTrainerBuild = Join-Path $repo `
  "benchmarks_and_tools\build_decode_dll_bench.ps1"
$resultsPath = if ($Results) { $Results } else {
  Join-Path $repo "benchmarks_and_tools\jxl-effort-678-decode-fixed-2026-07-30\results.csv"
}
$corpusPath = if ($Corpus) { $Corpus } else {
  Join-Path $repo "benchmarks_and_tools\large-matrix-2026-07-28\corpus"
}
$profilePath = if ([IO.Path]::IsPathRooted($ProfileDir)) {
  $ProfileDir
} else { Join-Path $root $ProfileDir }
$resultsPath = (Resolve-Path -LiteralPath $resultsPath).Path
$corpusPath = (Resolve-Path -LiteralPath $corpusPath).Path
[IO.Directory]::CreateDirectory($profilePath) | Out-Null

function Resolve-BuildFile([string]$BuildDir, [string]$File) {
  $directory = if ([IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir
  } else { Join-Path $root $BuildDir }
  return Join-Path $directory $File
}

function Build-Clang([string]$BuildDir, [string]$Pgo, [string]$PgoProfile,
                     [bool]$SkipTests, [bool]$UseNative) {
  $arguments = @{
    Config = $Config
    BuildDir = $BuildDir
    Pgo = $Pgo
  }
  if ($PgoProfile) { $arguments.PgoProfile = $PgoProfile }
  if ($SkipTests) { $arguments.SkipTests = $true }
  if ($UseNative) { $arguments.Native = $true }
  & $buildClang @arguments
  if ($LASTEXITCODE -ne 0) { throw "Clang $Pgo build failed" }
}

if ($fullCorpus -and ![IO.File]::Exists($decodeTrainer)) {
  & $decodeTrainerBuild
  if ($LASTEXITCODE -ne 0 -or ![IO.File]::Exists($decodeTrainer)) {
    throw "Could not build full-corpus trainer: $decodeTrainer"
  }
}

if ($ReferenceQlic) {
  $reference = (Resolve-Path -LiteralPath $ReferenceQlic).Path
} else {
  Build-Clang $ReferenceBuildDir "Off" "" $true $false
  $reference = Resolve-BuildFile $ReferenceBuildDir "qlic.exe"
}
Build-Clang $InstrumentedBuildDir "Generate" "" $true $false
$instrumented = Resolve-BuildFile $InstrumentedBuildDir "qlic.exe"
$instrumentedDll = Resolve-BuildFile $InstrumentedBuildDir "qlic.dll"

$p1Categories = @(
  "natural-clic2022",
  "natural-div2k",
  "qoi-photo_tecnick",
  "qoi-photo_wikipedia"
)
$allRecords = @(Import-Csv -LiteralPath $resultsPath)
$records = if ($fullCorpus) { $allRecords } else {
  @($allRecords | Where-Object Category -in $p1Categories)
}
$trainingCategories = if ($fullCorpus) {
  @($records | Select-Object -ExpandProperty Category -Unique)
} else { $p1Categories }
$rawPrefix = Join-Path $profilePath "decode-"
Get-ChildItem -LiteralPath $profilePath -Filter "decode-*.profraw" -File |
  ForEach-Object { [IO.File]::Delete($_.FullName) }
$merged = Join-Path $profilePath "decode.profdata"
if ([IO.File]::Exists($merged)) { [IO.File]::Delete($merged) }
$encoded = Join-Path $profilePath "training.qlic"
$decoded = Join-Path $profilePath "training.ppm"
$manifest = Join-Path $profilePath "training-stream.txt"
$training = [Collections.Generic.List[object]]::new()
$oldProfile = $env:LLVM_PROFILE_FILE
$env:LLVM_PROFILE_FILE = "$rawPrefix%m.profraw"
try {
  foreach ($category in $trainingCategories) {
    $selected = 0
    $categoryRecords = @($records | Where-Object Category -eq $category |
      Sort-Object { [int64]$_.Pixels } -Descending)
    foreach ($record in $categoryRecords) {
      if (!$fullCorpus -and !$allMode52 -and
          $selected -ge $ImagesPerCategory) { break }
      & $reference pack (Join-Path $corpusPath $record.Path) $encoded `
        --threads 1 2>&1 | Out-Null
      if ($LASTEXITCODE -ne 0) { throw "Pack failed: $($record.Path)" }
      $info = (& $reference info $encoded 2>&1 | Out-String)
      if ($LASTEXITCODE -ne 0) { throw "Info failed: $($record.Path)" }
      $nativeMode = if ($info -match "native-mode=(\d+)(?:\s|$)") {
        [int]$Matches[1]
      } else { $null }
      if (!$fullCorpus -and $nativeMode -ne 52) {
        [IO.File]::Delete($encoded)
        continue
      }
      if ($fullCorpus) {
        [IO.File]::WriteAllText($manifest,
          $encoded + [Environment]::NewLine)
        & $decodeTrainer train $instrumentedDll $manifest 2>&1 | Out-Null
      } else {
        & $instrumented unpack $encoded $decoded --threads 1 2>&1 | Out-Null
      }
      if ($LASTEXITCODE -ne 0) {
        throw "Instrumented decode failed: $($record.Path)"
      }
      $training.Add([pscustomobject][ordered]@{
        Path = $record.Path
        Category = $record.Category
        Pixels = [int64]$record.Pixels
        NativeMode = $nativeMode
      })
      $selected++
      [IO.File]::Delete($encoded)
      [IO.File]::Delete($decoded)
    }
    if (!$fullCorpus -and !$allMode52 -and
        $selected -ne $ImagesPerCategory) {
      throw "Found only $selected mode-52 images in $category"
    }
    if (!$fullCorpus -and $allMode52 -and !$selected) {
      throw "Found no mode-52 images in $category"
    }
    if ($fullCorpus -and $selected -ne $categoryRecords.Count) {
      throw "Trained on only $selected of $($categoryRecords.Count) images in $category"
    }
  }
} finally {
  $env:LLVM_PROFILE_FILE = $oldProfile
  if ([IO.File]::Exists($encoded)) { [IO.File]::Delete($encoded) }
  if ([IO.File]::Exists($decoded)) { [IO.File]::Delete($decoded) }
  if ([IO.File]::Exists($manifest)) { [IO.File]::Delete($manifest) }
}
$training | Export-Csv (Join-Path $profilePath "training-set.csv") `
  -NoTypeInformation

$llvmProfdata = Get-Command llvm-profdata.exe -ErrorAction SilentlyContinue
if (!$llvmProfdata) {
  $llvmProfdataPath = Join-Path $env:ProgramFiles "LLVM\bin\llvm-profdata.exe"
  if (!(Test-Path -LiteralPath $llvmProfdataPath)) {
    throw "llvm-profdata.exe was not found"
  }
} else { $llvmProfdataPath = $llvmProfdata.Source }
$rawProfiles = @(Get-ChildItem -LiteralPath $profilePath `
  -Filter "decode-*.profraw" -File | Select-Object -ExpandProperty FullName)
if (!$rawProfiles.Count) { throw "Training produced no LLVM profile" }
& $llvmProfdataPath merge "-output=$merged" @rawProfiles
if ($LASTEXITCODE -ne 0) { throw "Could not merge LLVM profile" }

Build-Clang $OutputBuildDir "Use" $merged $false $Native.IsPresent
Write-Output "Decode-only PGO build: $(Resolve-BuildFile $OutputBuildDir 'qlic.exe')"
Write-Output "Training stream generator: $reference"
Write-Output "Training images: $($training.Count)"
Write-Output "Merged profile: $merged"
