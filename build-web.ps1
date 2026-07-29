param(
  [string]$OutDir = "web\dist"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
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

# keep this list small, it is the public browser ABI
$exports = @(
  "--export=qlic_alloc",
  "--export=qlic_reset",
  "--export=qlic_encode",
  "--export=qlic_encoded_ptr",
  "--export=qlic_encoded_size",
  "--export=qlic_decode",
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
Copy-Item (Join-Path $root "web\qlic-worker.js") $out -Force
Copy-Item (Join-Path $root "web\demo.html") $out -Force
Copy-Item (Join-Path $root "web\README.md") $out -Force
$outPath = [IO.Path]::GetFullPath($out).TrimEnd(
  [IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
$objPath = [IO.Path]::GetFullPath($obj)
if (!$objPath.StartsWith($outPath, [StringComparison]::OrdinalIgnoreCase)) {
  throw "Refusing to clear an object path outside the web output."
}
Remove-Item -LiteralPath $objPath -Recurse -Force `
  -ErrorAction SilentlyContinue

$node = Get-Command node.exe -ErrorAction SilentlyContinue
if ($node) {
  & $node.Source (Join-Path $root "tests\web_test.mjs") `
    (Join-Path $out "qlic-web.js") `
    (Join-Path $out "qlic-web.wasm") `
    (Join-Path $root "tests\fixtures\native.qlic") `
    (Join-Path $root "tests\fixtures\animation.qlic") `
    (Join-Path $root "tests\fixtures\cpalette-lzms.qlic") `
    (Join-Path $root "tests\fixtures\gray-model-lzms.qlic") `
    (Join-Path $root "tests\fixtures\rgb-lzms.qlic")
  if ($LASTEXITCODE -ne 0) { throw "WebAssembly decoder test failed." }
}

Write-Host "Web package: $out"
