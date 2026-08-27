if(NOT DEFINED SOURCE_DIR OR NOT DEFINED LOCK)
  message(FATAL_ERROR "SOURCE_DIR and LOCK are required")
endif()

file(READ "${LOCK}" lock_json)
foreach(path IN ITEMS
    "codec/src/stream.c"
    "rust/qlic-decoder/src/qst1.rs"
    "docs/profiles.json"
    "tests/fixtures/manifest.json"
    "tests/fixtures/negative-manifest.json")
  string(JSON expected ERROR_VARIABLE json_error GET "${lock_json}"
    "normative_decoder_sources" "${path}")
  if(json_error)
    string(JSON expected ERROR_VARIABLE json_error GET "${lock_json}"
      "normative_data" "${path}")
  endif()
  if(json_error)
    message(FATAL_ERROR "conformance lock has no entry for ${path}: ${json_error}")
  endif()
  file(SHA256 "${SOURCE_DIR}/${path}" actual)
  if(NOT actual STREQUAL expected)
    message(FATAL_ERROR
      "conformance source drift: ${path}\nexpected ${expected}\nactual   ${actual}\n"
      "Review the wire-format change and deliberately refresh conformance.lock.json.")
  endif()
endforeach()

file(READ "${SOURCE_DIR}/docs/core-still.md" core_spec)
if(NOT core_spec MATCHES "qst1-model-annex.md")
  message(FATAL_ERROR "core-still.md does not incorporate the QST1 model annex")
endif()
message(STATUS "QLIC conformance lock verified")
