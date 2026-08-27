param(
  [string]$OutDir = "web\dist"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$out = if ([System.IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $root $OutDir }
$clangCommand = Get-Command clang.exe -ErrorAction SilentlyContinue
$wasmldCommand = Get-Command wasm-ld.exe -ErrorAction SilentlyContinue
$clang = if ($clangCommand) { $clangCommand.Source } else { Join-Path $env:ProgramFiles "LLVM\bin\clang.exe" }
$wasmld = if ($wasmldCommand) { $wasmldCommand.Source } else { Join-Path $env:ProgramFiles "LLVM\bin\wasm-ld.exe" }

if (!(Test-Path $clang) -or !(Test-Path $wasmld)) {
  throw "LLVM with clang.exe and wasm-ld.exe is required."
}

New-Item -ItemType Directory -Force $out | Out-Null
$obj = Join-Path $out "obj"
New-Item -ItemType Directory -Force $obj | Out-Null

# the wasm decoder is freestanding so browsers do not need another runtime
$cflags = @(
  "--target=wasm32",
  "-O3",
  "-DNDEBUG",
  "-DQLIC_WASM=1",
  "-ffreestanding",
  "-fno-builtin",
  "-ffunction-sections",
  "-fdata-sections",
  "-Wall",
  "-Wextra",
  "-Wconversion",
  "-Werror",
  "-I", (Join-Path $root "codec\src")
)

& $clang @cflags -c -o (Join-Path $obj "qlic_wasm.o") (Join-Path $root "web\qlic_wasm.c")
if ($LASTEXITCODE -ne 0) { throw "qlic_wasm.c compilation failed." }
& $clang @cflags -DQLIC_NO_MAIN -c -o (Join-Path $obj "qlic.o") (Join-Path $root "codec\src\qlic.c")
if ($LASTEXITCODE -ne 0) { throw "qlic.c compilation failed." }
& $clang @cflags -c -o (Join-Path $obj "stream.o") (Join-Path $root "codec\src\stream.c")
if ($LASTEXITCODE -ne 0) { throw "stream.c compilation failed." }
& $clang @cflags -c -o (Join-Path $obj "parallel.o") (Join-Path $root "codec\src\parallel.c")
if ($LASTEXITCODE -ne 0) { throw "parallel.c compilation failed." }
& $clang @cflags -c -o (Join-Path $obj "lzms.o") (Join-Path $root "codec\src\lzms.c")
if ($LASTEXITCODE -ne 0) { throw "lzms.c compilation failed." }

# Public browser ABI.
$exports = @(
  "--export=qlic_alloc",
  "--export=qlic_reset",
  "--export=qlic_encode",
  "--export=qlic_encoded_ptr",
  "--export=qlic_encoded_size",
  "--export=qlic_decode",
  "--export=qlic_validate",
  "--export=qlic_decode_wide",
  "--export=qlic_decode_hdr",
  "--export=qlic_width",
  "--export=qlic_height",
  "--export=qlic_frame_count",
  "--export=qlic_loop_count",
  "--export=qlic_animated",
  "--export=qlic_frame_width",
  "--export=qlic_frame_height",
  "--export=qlic_frame_delay",
  "--export=qlic_frame_ptr",
  "--export=qlic_frame_size",
  "--export=qlic_sample_width",
  "--export=qlic_sample_height",
  "--export=qlic_sample_channels",
  "--export=qlic_sample_bits",
  "--export=qlic_sample_ptr",
  "--export=qlic_sample_size",
  "--export=qlic_sample_stride",
  "--export=qlic_hdr_metadata_ptr",
  "--export=qlic_hdr_metadata_size",
  "--export=qlic_hdr_block_count",
  "--export=qlic_hdr_block_tag",
  "--export=qlic_hdr_block_ptr",
  "--export=qlic_hdr_block_size",
  "--export=qlic_error_ptr"
)

& $wasmld `
  --no-entry `
  --export-memory `
  --gc-sections `
  --initial-memory=16777216 `
  --max-memory=1073741824 `
  -z stack-size=1048576 `
  @exports `
  -o (Join-Path $out "qlic-web.wasm") `
  (Join-Path $obj "qlic_wasm.o") `
  (Join-Path $obj "qlic.o") `
  (Join-Path $obj "stream.o") `
  (Join-Path $obj "parallel.o") `
  (Join-Path $obj "lzms.o")
if ($LASTEXITCODE -ne 0) { throw "WebAssembly link failed." }

Copy-Item (Join-Path $root "web\qlic-web.js") $out -Force
Copy-Item (Join-Path $root "web\qlic-web.d.ts") $out -Force
Copy-Item (Join-Path $root "web\qlic-worker.js") $out -Force
$modulePath = Join-Path $root "web\qlic-web.js"
$workerPath = Join-Path $root "web\qlic-worker.js"
$indexTemplatePath = Join-Path $root "web\index.html"
$moduleSource = [IO.File]::ReadAllText($modulePath)
$standaloneModule = $moduleSource.Replace(
  "export async function createQlic", "async function createQlic")
$standaloneModule = $standaloneModule.Replace(
  'new URL("qlic-web.wasm", import.meta.url)', '"qlic-web.wasm"')
$standaloneModule = $standaloneModule.Replace("export default createQlic;", "")
if ($standaloneModule -eq $moduleSource -or
    $standaloneModule -match '(?m)^\s*export\s' -or
    $standaloneModule.Contains("import.meta")) {
  throw "The Web module could not be converted to the standalone classic worker."
}
$moduleBase64 = [Convert]::ToBase64String(
  [Text.Encoding]::UTF8.GetBytes($standaloneModule))
$wasmBase64 = [Convert]::ToBase64String(
  [IO.File]::ReadAllBytes((Join-Path $out "qlic-web.wasm")))
$workerSource = [IO.File]::ReadAllText($workerPath)
$workerBody = [Text.RegularExpressions.Regex]::Replace(
  $workerSource,
  '\A\s*import\s+\{\s*createQlic\s*\}\s+from\s+"\.\/qlic-web\.js";\s*',
  "")
$workerReady = "const qlicReady = createQlic();"
if (!$workerBody.Contains($workerReady)) {
  throw "The browser worker bootstrap is not recognized."
}
$workerBody = $workerBody.Replace(
  $workerReady,
  "const qlicReady = createQlic({ wasmBinary: qlicStandaloneWasm });")
$workerBodyBase64 = [Convert]::ToBase64String(
  [Text.Encoding]::UTF8.GetBytes($workerBody))
$index = [IO.File]::ReadAllText($indexTemplatePath)
$index = $index.Replace("__QLIC_WEB_MODULE_BASE64__", $moduleBase64)
$index = $index.Replace("__QLIC_WEB_WASM_BASE64__", $wasmBase64)
$index = $index.Replace("__QLIC_WEB_WORKER_BASE64__", $workerBodyBase64)
foreach ($placeholder in @(
  "__QLIC_WEB_MODULE_BASE64__",
  "__QLIC_WEB_WASM_BASE64__",
  "__QLIC_WEB_WORKER_BASE64__"
)) {
  if ($index.Contains($placeholder)) {
    throw "The standalone browser page still contains $placeholder."
  }
}
[IO.File]::WriteAllText(
  (Join-Path $out "index.html"), $index, [Text.UTF8Encoding]::new($false))
Copy-Item (Join-Path $root "web\README.md") $out -Force
Copy-Item (Join-Path $root "web\package.json") $out -Force
Copy-Item (Join-Path $root "LICENSE") $out -Force
Copy-Item (Join-Path $root "third_party\lzms\LICENSE") `
  (Join-Path $out "LICENSE-LZMS") -Force
Copy-Item (Join-Path $root "NOTICE") $out -Force
Copy-Item (Join-Path $root "SECURITY.md") $out -Force
Copy-Item (Join-Path $root "SUPPORT.md") $out -Force
$package = Get-Content -Raw (Join-Path $out "package.json") | ConvertFrom-Json
if ($package.private -or $package.name -ne "qlic-web" -or
    $package.module -ne "./qlic-web.js" -or
    $package.types -ne "./qlic-web.d.ts" -or
    !(Test-Path (Join-Path $out "qlic-web.d.ts"))) {
  throw "Web package metadata is incomplete."
}
$outPath = [IO.Path]::GetFullPath($out).TrimEnd(
  [IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
$objPath = [IO.Path]::GetFullPath($obj)
if (!$objPath.StartsWith($outPath, [StringComparison]::OrdinalIgnoreCase)) {
  throw "Refusing to clear an object path outside the web output."
}
Remove-Item -LiteralPath $objPath -Recurse -Force `
  -ErrorAction SilentlyContinue

$node = Get-Command node.exe -ErrorAction SilentlyContinue
if (!$node) {
  throw "Node.js is required to validate the WebAssembly decoder."
}
& $node.Source (Join-Path $root "tests\web_test.mjs") `
  (Join-Path $out "qlic-web.js") `
  (Join-Path $out "qlic-web.wasm") `
  (Join-Path $root "tests\fixtures\native.qlic") `
  (Join-Path $root "tests\fixtures\animation.qlic") `
  (Join-Path $root "tests\fixtures\cpalette-lzms.qlic") `
  (Join-Path $root "tests\fixtures\gray-model-lzms.qlic") `
  (Join-Path $root "tests\fixtures\rgb-lzms.qlic") `
  (Join-Path $root "tests\fixtures\normal-map-quadratic.qlic") `
  (Join-Path $root "tests\fixtures\normal-map-sphere-green8.qlic") `
  (Join-Path $root "tests\fixtures\planar-med-lzms.qlic") `
  (Join-Path $root "tests\fixtures\tile-palette-lzms.qlic") `
  --mode-tiles-rgba `
  (Join-Path $root "tests\fixtures\retained\mode45-current-streams\0013.qlic") `
  --wide `
  (Join-Path $root "tests\fixtures\wide-u16-10-boundary.qlic") `
  (Join-Path $root "tests\fixtures\wide-u16-16-rgba.qlic") `
  (Join-Path $root "tests\fixtures\wide-u32-17-boundary.qlic") `
  (Join-Path $root "tests\fixtures\wide-u32-24-rgb.qlic") `
  --hdr `
  (Join-Path $root "tests\fixtures\hdr-u16-12-pq-rgba.qlic") `
  --hlg `
  (Join-Path $root "tests\fixtures\hdr-u16-10-hlg-rgb.qlic") `
  --described8 `
  (Join-Path $root "tests\fixtures\described-u16-8-srgb-rgb.qlic") `
  --reject `
  (Join-Path $root "tests\fixtures\hdr-u16-12-pq-rgba.qlic")
if ($LASTEXITCODE -ne 0) { throw "WebAssembly decoder test failed." }

Write-Host "Web package: $out"
