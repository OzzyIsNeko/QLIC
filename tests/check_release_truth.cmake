if(NOT DEFINED SOURCE_DIR OR NOT DEFINED PROJECT_VERSION)
  message(FATAL_ERROR "SOURCE_DIR and PROJECT_VERSION are required")
endif()

function(require_match path expression description)
  file(READ "${path}" contents)
  if(NOT contents MATCHES "${expression}")
    message(FATAL_ERROR "${description} does not match QLIC ${PROJECT_VERSION}: ${path}")
  endif()
endfunction()

string(REPLACE "." "\\." version_regex "${PROJECT_VERSION}")
require_match(
  "${SOURCE_DIR}/rust/qlic-decoder/Cargo.toml"
  "version[ \t]*=[ \t]*\"${version_regex}\""
  "Rust package version")
require_match(
  "${SOURCE_DIR}/web/package.json"
  "\"version\"[ \t\r\n]*:[ \t\r\n]*\"${version_regex}\""
  "Web package version")
require_match(
  "${SOURCE_DIR}/docs/releases/${PROJECT_VERSION}.md"
  "# QLIC ${version_regex}"
  "Release notes version")
file(READ "${SOURCE_DIR}/SUPPORT.md" support)
if(support MATCHES "arbitrary EXIF/XMP/comment" OR
   support MATCHES "WIC adapter is[ \r\n]+an 8-bit view")
  message(FATAL_ERROR "SUPPORT.md contains a superseded metadata/WIC claim")
endif()

file(READ "${SOURCE_DIR}/benchmark/corpus-manifest.csv" corpus_manifest)
if(NOT corpus_manifest MATCHES
   "^\"Path\",\"Category\",\"Width\",\"Height\",\"Pixels\",\"NormalizedSHA256\"[\r\n]")
  message(FATAL_ERROR "The benchmark corpus manifest schema is not portable")
endif()
file(READ "${SOURCE_DIR}/benchmark/corpus-excluded.csv" corpus_excluded)
if(corpus_excluded MATCHES ",\"([A-Za-z]:[/\\]|/|\\\\)")
  message(FATAL_ERROR "The benchmark exclusion record contains a machine-specific path")
endif()

message(STATUS "QLIC ${PROJECT_VERSION} release versions and support claims agree")
