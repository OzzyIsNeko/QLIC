execute_process(
  COMMAND "${NM}" -D --defined-only --format=posix "${LIBRARY}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "could not inspect the shared library exports")
endif()

string(REGEX MATCHALL "qlic_[a-z0-9_]+ " exports "${output}")
list(TRANSFORM exports STRIP)
list(SORT exports)
set(expected
  qlic_animation_free
  qlic_decode_animation
  qlic_decode_limits_default
  qlic_decode_rgba
  qlic_encode_animation
  qlic_encode_options_default
  qlic_encode_rgba
  qlic_free
  qlic_get_info
  qlic_hardware_thread_count
  qlic_image_free
  qlic_last_error
  qlic_status_string
  qlic_version)
list(SORT expected)
if(NOT exports STREQUAL expected)
  message(FATAL_ERROR "unexpected shared library exports: ${exports}")
endif()
