foreach(name IN ITEMS QLIC INPUT OUTPUT DECODED)
  if(NOT DEFINED ${name} OR "${${name}}" STREQUAL "")
    message(FATAL_ERROR "${name} is required")
  endif()
endforeach()

file(REMOVE "${OUTPUT}" "${DECODED}")
execute_process(
  COMMAND "${QLIC}" pack "${INPUT}" "${OUTPUT}"
  RESULT_VARIABLE pack_result
  OUTPUT_VARIABLE pack_output
  ERROR_VARIABLE pack_error)
if(NOT pack_result STREQUAL "0")
  message(FATAL_ERROR
    "QLIC rejected a decodable lossy input:\n${pack_output}${pack_error}")
endif()

string(CONCAT pack_log "${pack_output}" "${pack_error}")
string(FIND "${pack_log}" "warning: This source is lossy." warning_at)
if(warning_at EQUAL -1)
  message(FATAL_ERROR "QLIC accepted lossy input without the size warning")
endif()

if(NOT EXISTS "${OUTPUT}")
  message(FATAL_ERROR "QLIC did not write the packed file")
endif()
file(SIZE "${OUTPUT}" output_size)
if(output_size EQUAL 0)
  message(FATAL_ERROR "QLIC wrote an empty packed file")
endif()

execute_process(
  COMMAND "${QLIC}" unpack "${OUTPUT}" "${DECODED}"
  RESULT_VARIABLE unpack_result
  OUTPUT_VARIABLE unpack_output
  ERROR_VARIABLE unpack_error)
if(NOT unpack_result STREQUAL "0" OR NOT EXISTS "${DECODED}")
  message(FATAL_ERROR
    "QLIC could not decode the accepted input:\n${unpack_output}${unpack_error}")
endif()
file(SIZE "${DECODED}" decoded_size)
if(decoded_size EQUAL 0)
  message(FATAL_ERROR "QLIC decoded the accepted input to an empty file")
endif()
