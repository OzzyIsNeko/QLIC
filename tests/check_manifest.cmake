if(NOT DEFINED MANIFEST OR NOT DEFINED FIXTURE_DIR OR NOT DEFINED PROFILES)
  message(FATAL_ERROR "manifest check paths are required")
endif()

file(READ "${MANIFEST}" manifest_json)
file(READ "${PROFILES}" profiles_json)
file(SHA256 "${PROFILES}" profiles_hash)
string(TOLOWER "${profiles_hash}" profiles_hash)
string(JSON expected_profiles_hash GET "${manifest_json}" profiles_sha256)
if(NOT profiles_hash STREQUAL expected_profiles_hash)
  message(FATAL_ERROR "fixture manifest has a stale profile hash")
endif()

string(JSON fixture_count LENGTH "${manifest_json}" fixtures)
if(fixture_count LESS 1)
  message(FATAL_ERROR "fixture manifest is empty")
endif()
math(EXPR last_fixture "${fixture_count} - 1")
set(manifest_names)
foreach(index RANGE 0 ${last_fixture})
  string(JSON name GET "${manifest_json}" fixtures ${index} file)
  string(JSON profile GET "${manifest_json}" fixtures ${index} profile)
  string(JSON outer_mode GET "${manifest_json}" fixtures ${index} outer_mode)
  string(JSON expected_size GET "${manifest_json}" fixtures ${index} bytes)
  string(JSON expected_hash GET "${manifest_json}" fixtures ${index} sha256)

  string(JSON profile_type ERROR_VARIABLE profile_error TYPE
    "${profiles_json}" profiles "${profile}")
  if(profile_error OR NOT profile_type STREQUAL "OBJECT")
    message(FATAL_ERROR
      "conformance fixture has an unknown profile: ${name}: ${profile}")
  endif()
  string(JSON mode_count LENGTH
    "${profiles_json}" profiles "${profile}" outer_modes)
  math(EXPR last_mode "${mode_count} - 1")
  set(mode_allowed FALSE)
  foreach(mode_index RANGE 0 ${last_mode})
    string(JSON allowed_mode GET
      "${profiles_json}" profiles "${profile}" outer_modes ${mode_index})
    if(outer_mode EQUAL allowed_mode)
      set(mode_allowed TRUE)
      break()
    endif()
  endforeach()
  if(NOT mode_allowed)
    message(FATAL_ERROR
      "conformance fixture profile does not allow its outer mode: ${name}: ${profile}, mode ${outer_mode}")
  endif()

  if(profile STREQUAL "core-still-1" AND outer_mode EQUAL 9)
    string(JSON native_mode GET "${manifest_json}" fixtures ${index} native_mode)
    string(JSON native_transform GET
      "${manifest_json}" fixtures ${index} native_transform)
    string(JSON native_mode_count LENGTH
      "${profiles_json}" profiles "${profile}" native_modes)
    math(EXPR last_native_mode "${native_mode_count} - 1")
    set(native_mode_allowed FALSE)
    foreach(mode_index RANGE 0 ${last_native_mode})
      string(JSON allowed_native_mode GET
        "${profiles_json}" profiles "${profile}" native_modes ${mode_index})
      if(native_mode EQUAL allowed_native_mode)
        set(native_mode_allowed TRUE)
        break()
      endif()
    endforeach()
    string(JSON minimum_transform GET "${profiles_json}" profiles
      "${profile}" native_transforms minimum)
    string(JSON maximum_transform GET "${profiles_json}" profiles
      "${profile}" native_transforms maximum)
    if(NOT native_mode_allowed OR native_transform LESS minimum_transform OR
       native_transform GREATER maximum_transform)
      message(FATAL_ERROR
        "native fixture falls outside its declared profile: ${name}: mode ${native_mode}, transform ${native_transform}")
    endif()
  endif()

  set(path "${FIXTURE_DIR}/${name}")
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "missing conformance fixture: ${name}")
  endif()
  file(SIZE "${path}" actual_size)
  file(SHA256 "${path}" actual_hash)
  string(TOLOWER "${actual_hash}" actual_hash)
  if(NOT actual_size EQUAL expected_size)
    message(FATAL_ERROR "conformance fixture size changed: ${name}")
  endif()
  if(NOT actual_hash STREQUAL expected_hash)
    message(FATAL_ERROR "conformance fixture hash changed: ${name}")
  endif()
  list(APPEND manifest_names "${name}")
endforeach()

file(GLOB fixture_paths RELATIVE "${FIXTURE_DIR}" "${FIXTURE_DIR}/*.qlic")
list(SORT fixture_paths)
list(SORT manifest_names)
if(NOT fixture_paths STREQUAL manifest_names)
  message(FATAL_ERROR
    "fixture manifest membership differs: files=${fixture_paths}; manifest=${manifest_names}")
endif()

message(STATUS "verified ${fixture_count} QLIC conformance fixtures")
