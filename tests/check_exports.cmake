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
  qlic_decode_hdr
  qlic_decode_limits_default
  qlic_decode_limits_v2_default
  qlic_decode_pixels
  qlic_decode_region_rgba
  qlic_decode_rgba
  qlic_decode_rows_rgba
  qlic_decode_wide
  qlic_encode_animation
  qlic_encode_hdr
  qlic_encode_options_default
  qlic_encode_pixels
  qlic_encode_rgba
  qlic_encode_wide
  qlic_free
  qlic_get_info
  qlic_get_info_ex
  qlic_get_info_v2
  qlic_get_capabilities
  qlic_hardware_thread_count
  qlic_hdr_image_free
  qlic_image_free
  qlic_last_error
  qlic_status_string
  qlic_validate
  qlic_version)
list(APPEND expected qlic_wide_image_free)
list(SORT expected)
if(NOT exports STREQUAL expected)
  message(FATAL_ERROR "unexpected shared library exports: ${exports}")
endif()
