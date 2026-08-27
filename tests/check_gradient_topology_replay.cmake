if(NOT DEFINED PROGRAM OR NOT DEFINED MANIFEST OR NOT DEFINED TRACE)
  message(FATAL_ERROR "PROGRAM, MANIFEST, and TRACE are required")
endif()

execute_process(
  COMMAND "${PROGRAM}" 0 "${MANIFEST}" "${TRACE}"
  RESULT_VARIABLE replay_result
  OUTPUT_VARIABLE replay_stdout
  ERROR_VARIABLE replay_stderr)
if(NOT replay_result EQUAL 0)
  message(FATAL_ERROR
    "Gradient-topology replay failed (${replay_result})\n"
    "stdout:\n${replay_stdout}\n"
    "stderr:\n${replay_stderr}")
endif()

file(READ "${TRACE}" trace)
foreach(required IN ITEMS
    "gradient-topology-file-begin index=1"
    "gradient-topology-model plane=0 classes=1"
    "gradient-topology-model plane=0 classes=4"
    "gradient-topology-model plane=0 classes=8"
    "gradient-topology-model plane=0 classes=16"
    "gradient-topology-shared plane=0 classes=4 weight=1"
    "gradient-topology-shared plane=0 classes=16 weight=6"
    "gradient-topology-signatures plane=0"
    "gradient-topology-file-end index=1 status=ok"
    "gradient-topology-replay files=1 passed=1 failed=0")
  string(FIND "${trace}" "${required}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR
      "Gradient-topology trace is missing: ${required}\n${trace}")
  endif()
endforeach()

string(REGEX MATCHALL "gradient-topology-model plane=" model_lines "${trace}")
list(LENGTH model_lines model_line_count)
string(REGEX MATCHALL "gradient-topology plane=" plane_lines "${trace}")
list(LENGTH plane_lines plane_line_count)
math(EXPR expected_model_lines "${plane_line_count} * 4")
string(REGEX MATCHALL "gradient-topology-shared plane=" shared_lines "${trace}")
list(LENGTH shared_lines shared_line_count)
math(EXPR expected_shared_lines "${plane_line_count} * 18")
if(plane_line_count LESS 3 OR plane_line_count GREATER 4 OR
   NOT model_line_count EQUAL expected_model_lines OR
   NOT shared_line_count EQUAL expected_shared_lines)
  message(FATAL_ERROR
    "Expected four entropy models and 18 shared models for each of three or "
    "four planes; got ${model_line_count} entropy and ${shared_line_count} "
    "shared models for ${plane_line_count} planes")
endif()

message(STATUS
  "Gradient-topology replay emitted ${plane_line_count} complete plane records")
