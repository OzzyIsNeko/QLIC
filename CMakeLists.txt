cmake_minimum_required(VERSION 3.25)

project(qlic VERSION 0.5.0 LANGUAGES C)

if(WIN32)
  enable_language(RC)
endif()

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
option(QLIC_SANITIZE "Build QLIC with compiler sanitizers" OFF)
option(QLIC_BUILD_CLI "Build the QLIC command line tool" ON)
option(QLIC_LINUX_LZMS "Use wimlib for LZMS encoding on Linux" ON)
set(QLIC_PACKAGE_NEEDS_WIMLIB FALSE)
if(NOT WIN32 AND QLIC_LINUX_LZMS)
  find_path(QLIC_WIMLIB_INCLUDE_DIR NAMES wimlib.h REQUIRED)
  find_library(QLIC_WIMLIB_LIBRARY NAMES wim REQUIRED)
  if(QLIC_WIMLIB_LIBRARY MATCHES "\\.a$")
    message(FATAL_ERROR "QLIC requires the shared wimlib library")
  endif()
  add_library(qlic_wimlib SHARED IMPORTED)
  set_target_properties(qlic_wimlib PROPERTIES
    IMPORTED_LOCATION "${QLIC_WIMLIB_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${QLIC_WIMLIB_INCLUDE_DIR}")
  set(QLIC_PACKAGE_NEEDS_WIMLIB TRUE)
endif()
set(QLIC_PGO "OFF" CACHE STRING "Profile-guided optimization mode")
set_property(CACHE QLIC_PGO PROPERTY STRINGS OFF GENERATE USE)
set(QLIC_PGO_PROFILE "" CACHE FILEPATH "Merged LLVM profile for QLIC_PGO=USE")
string(TOUPPER "${QLIC_PGO}" QLIC_PGO)
if(NOT QLIC_PGO MATCHES "^(OFF|GENERATE|USE)$")
  message(FATAL_ERROR "QLIC_PGO must be OFF, GENERATE, or USE")
endif()
if(NOT QLIC_PGO STREQUAL "OFF" AND
   NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "QLIC_PGO requires Clang")
endif()
if(QLIC_PGO STREQUAL "USE" AND NOT EXISTS "${QLIC_PGO_PROFILE}")
  message(FATAL_ERROR "QLIC_PGO_PROFILE must name a merged LLVM profile")
endif()
if(QLIC_SANITIZE AND NOT QLIC_PGO STREQUAL "OFF")
  message(FATAL_ERROR "QLIC_SANITIZE and QLIC_PGO cannot be combined")
endif()

set(QLIC_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${QLIC_GENERATED_DIR}")
configure_file(cmake/qlic_version.h.in "${QLIC_GENERATED_DIR}/qlic_version.h" @ONLY)
set(QLIC_CLI_RES)
set(QLIC_GUI_RES)
set(QLIC_LIBRARY_RES)
set(QLIC_WIC_RES)
set(QLIC_WEB_DEMO_RES)
if(WIN32)
  set(QLIC_FILE_DESCRIPTION "Quick Lossless Image Codec")
  set(QLIC_INTERNAL_NAME "qlic")
  set(QLIC_ORIGINAL_FILENAME "qlic.exe")
  configure_file(codec/src/qlic.rc.in "${QLIC_GENERATED_DIR}/qlic-cli.rc" @ONLY)
  set(QLIC_CLI_RES "${QLIC_GENERATED_DIR}/qlic-cli.rc")

  set(QLIC_FILE_DESCRIPTION "QLIC Compression Demo")
  set(QLIC_INTERNAL_NAME "qlic-gui")
  set(QLIC_ORIGINAL_FILENAME "qlic-gui.exe")
  configure_file(codec/src/qlic.rc.in "${QLIC_GENERATED_DIR}/qlic-gui.rc" @ONLY)
  set(QLIC_GUI_RES "${QLIC_GENERATED_DIR}/qlic-gui.rc")

  set(QLIC_FILE_DESCRIPTION "QLIC SDK")
  set(QLIC_INTERNAL_NAME "qlic")
  set(QLIC_ORIGINAL_FILENAME "qlic.dll")
  configure_file(codec/src/qlic.rc.in "${QLIC_GENERATED_DIR}/qlic-library.rc" @ONLY)
  set(QLIC_LIBRARY_RES "${QLIC_GENERATED_DIR}/qlic-library.rc")

  set(QLIC_FILE_DESCRIPTION "QLIC WIC Decoder")
  set(QLIC_INTERNAL_NAME "qlic-wic")
  set(QLIC_ORIGINAL_FILENAME "qlic-wic.dll")
  configure_file(codec/src/qlic.rc.in "${QLIC_GENERATED_DIR}/qlic-wic.rc" @ONLY)
  set(QLIC_WIC_RES "${QLIC_GENERATED_DIR}/qlic-wic.rc")

  find_program(QLIC_POWERSHELL NAMES pwsh powershell REQUIRED)
  set(QLIC_WEB_DEMO_DIR "${QLIC_GENERATED_DIR}/web")
  set(QLIC_WEB_DEMO_HTML "${QLIC_WEB_DEMO_DIR}/demo.html")
  set(QLIC_WEB_DEMO_MODULE "${QLIC_WEB_DEMO_DIR}/qlic-web.js")
  set(QLIC_WEB_DEMO_WORKER "${QLIC_WEB_DEMO_DIR}/qlic-worker.js")
  set(QLIC_WEB_DEMO_WASM "${QLIC_WEB_DEMO_DIR}/qlic-web.wasm")
  set(QLIC_WEB_DEMO_JXL
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/image-codecs/jxl_dec.dll")
  set(QLIC_WEB_DEMO_BROTLI_COMMON
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/image-codecs/brotlicommon.dll")
  set(QLIC_WEB_DEMO_BROTLI_DEC
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/image-codecs/brotlidec.dll")
  add_custom_command(
    OUTPUT "${QLIC_WEB_DEMO_WASM}"
    BYPRODUCTS
      "${QLIC_WEB_DEMO_HTML}"
      "${QLIC_WEB_DEMO_MODULE}"
      "${QLIC_WEB_DEMO_WORKER}"
    COMMAND "${QLIC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
      -File "${CMAKE_CURRENT_SOURCE_DIR}/build-web.ps1"
      -OutDir "${QLIC_WEB_DEMO_DIR}"
    DEPENDS
      build-web.ps1
      web/demo.html
      web/qlic-web.js
      web/qlic-worker.js
      web/qlic_wasm.c
      codec/src/qlic.c
      codec/src/qlic_core.h
      codec/src/stream.c
      codec/src/parallel.c
      codec/src/lzms.c
      tests/web_test.mjs
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    VERBATIM)
  add_custom_target(qlic-web-assets DEPENDS "${QLIC_WEB_DEMO_WASM}")
  configure_file(runtime/web_demo.rc.in
    "${QLIC_GENERATED_DIR}/qlic-web-demo.rc" @ONLY)
  set(QLIC_WEB_DEMO_RES "${QLIC_GENERATED_DIR}/qlic-web-demo.rc")
  set_source_files_properties("${QLIC_WEB_DEMO_RES}" PROPERTIES
    OBJECT_DEPENDS
      "${QLIC_WEB_DEMO_HTML};${QLIC_WEB_DEMO_MODULE};${QLIC_WEB_DEMO_WORKER};${QLIC_WEB_DEMO_WASM};${QLIC_WEB_DEMO_JXL};${QLIC_WEB_DEMO_BROTLI_COMMON};${QLIC_WEB_DEMO_BROTLI_DEC}")
endif()
set(QLIC_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/qlic")
configure_package_config_file(
  cmake/qlicConfig.cmake.in
  "${QLIC_GENERATED_DIR}/qlicConfig.cmake"
  INSTALL_DESTINATION "${QLIC_CMAKE_INSTALL_DIR}")
write_basic_package_version_file(
  "${QLIC_GENERATED_DIR}/qlicConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMinorVersion)
include_directories("${QLIC_GENERATED_DIR}")

set(QLIC_MAP_SOURCES)
if(MSVC AND CMAKE_C_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
  list(APPEND QLIC_MAP_SOURCES codec/src/map_avx2.c)
  set_source_files_properties(codec/src/map_avx2.c PROPERTIES COMPILE_OPTIONS /arch:AVX2)
endif()

# sharing the object target keeps static and DLL codec builds identical
set(QLIC_CORE_SOURCES
  codec/src/qlic.c
  codec/src/stream.c
  codec/src/parallel.c
  codec/src/lzms.c
  ${QLIC_MAP_SOURCES})
add_library(qliccore OBJECT ${QLIC_CORE_SOURCES})
set_target_properties(qliccore PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  C_VISIBILITY_PRESET hidden)
target_compile_definitions(qliccore PRIVATE QLIC_NO_MAIN)
target_link_libraries(qliccore PRIVATE Threads::Threads)
if(TARGET qlic_wimlib)
  target_compile_definitions(qliccore PRIVATE QLIC_HAVE_WIMLIB)
  target_link_libraries(qliccore PRIVATE qlic_wimlib)
endif()
if(WIN32)
  target_compile_definitions(qliccore PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()

if(WIN32)
  add_executable(qlic
    codec/src/qlic.c
    codec/src/input.c
    codec/src/stream.c
    codec/src/parallel.c
    codec/src/lzms.c
    ${QLIC_MAP_SOURCES}
    ${QLIC_CLI_RES})
  target_compile_definitions(qlic PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
  target_include_directories(qlic PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/jxl/include")
  target_link_libraries(qlic PRIVATE
    Threads::Threads windowscodecs ole32 cabinet shell32)
  add_custom_command(TARGET qlic POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/runtime/image-codecs"
      "$<TARGET_FILE_DIR:qlic>/image-codecs")
else()
  if(QLIC_BUILD_CLI)
    find_package(PNG REQUIRED)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(QLIC_WEBP IMPORTED_TARGET libwebp)
      pkg_check_modules(QLIC_JXL IMPORTED_TARGET libjxl)
      pkg_check_modules(QLIC_AVIF IMPORTED_TARGET libavif)
      pkg_check_modules(QLIC_TIFF IMPORTED_TARGET libtiff-4)
    endif()
  endif()
endif()

add_library(qlic_static STATIC codec/src/qlic_api.c $<TARGET_OBJECTS:qliccore>)
set_target_properties(qlic_static PROPERTIES
  OUTPUT_NAME qlic_static
  EXPORT_NAME qlic_static)
add_library(qlic::qlic_static ALIAS qlic_static)
target_include_directories(qlic_static PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/codec/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_compile_definitions(qlic_static PUBLIC QLIC_STATIC)
target_link_libraries(qlic_static PRIVATE Threads::Threads)
if(TARGET qlic_wimlib)
  target_link_libraries(qlic_static PUBLIC qlic_wimlib)
endif()
if(WIN32)
  target_compile_definitions(qlic_static PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(qlic_static PRIVATE cabinet)
endif()

add_library(qlic_library SHARED codec/src/qlic_api.c ${QLIC_LIBRARY_RES} $<TARGET_OBJECTS:qliccore>)
set_target_properties(qlic_library PROPERTIES
  OUTPUT_NAME qlic
  EXPORT_NAME qlic
  VERSION "${PROJECT_VERSION}"
  SOVERSION "${PROJECT_VERSION_MAJOR}"
  C_VISIBILITY_PRESET hidden)
add_library(qlic::qlic ALIAS qlic_library)
target_include_directories(qlic_library PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/codec/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_compile_definitions(qlic_library PRIVATE QLIC_API_BUILD)
target_link_libraries(qlic_library PRIVATE Threads::Threads)
if(TARGET qlic_wimlib)
  target_link_libraries(qlic_library PRIVATE qlic_wimlib)
endif()
if(WIN32)
  target_compile_definitions(qlic_library PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS QLIC_RESOURCE_DLL)
  target_link_libraries(qlic_library PRIVATE cabinet)
endif()

add_executable(qlic-example-decode examples/c/decode.c)
target_compile_definitions(qlic-example-decode PRIVATE QLIC_STATIC)
target_link_libraries(qlic-example-decode PRIVATE qlic_static)

if(NOT WIN32 AND QLIC_BUILD_CLI)
  add_executable(qlic codec/src/cli_posix.c)
  target_link_libraries(qlic PRIVATE qlic_static PNG::PNG)
  if(TARGET PkgConfig::QLIC_WEBP)
    target_link_libraries(qlic PRIVATE PkgConfig::QLIC_WEBP)
    target_compile_definitions(qlic PRIVATE QLIC_HAVE_WEBP)
  endif()
  if(TARGET PkgConfig::QLIC_JXL)
    target_link_libraries(qlic PRIVATE PkgConfig::QLIC_JXL)
    target_compile_definitions(qlic PRIVATE QLIC_HAVE_JXL)
  endif()
  if(TARGET PkgConfig::QLIC_AVIF)
    target_link_libraries(qlic PRIVATE PkgConfig::QLIC_AVIF)
    target_compile_definitions(qlic PRIVATE QLIC_HAVE_AVIF)
  endif()
  if(TARGET PkgConfig::QLIC_TIFF)
    target_link_libraries(qlic PRIVATE PkgConfig::QLIC_TIFF)
    target_compile_definitions(qlic PRIVATE QLIC_HAVE_TIFF)
  endif()
endif()

if(WIN32)
  add_executable(qlic-gui WIN32 codec/src/gui.c codec/src/win_util.c ${QLIC_GUI_RES})
  target_compile_definitions(qlic-gui PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
  target_link_libraries(qlic-gui PRIVATE comctl32 comdlg32 dwmapi shell32 user32 gdi32)

  add_library(qlic-wic SHARED codec/src/wic_codec.c codec/src/qlic-wic.def ${QLIC_WIC_RES})
  target_compile_definitions(qlic-wic PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE QLIC_RESOURCE_DLL)
  target_link_libraries(qlic-wic PRIVATE qlic_static windowscodecs ole32 cabinet shell32 advapi32)

  add_executable(qlic-web-demo WIN32
    runtime/web_demo.c codec/src/input.c ${QLIC_WEB_DEMO_RES})
  set_target_properties(qlic-web-demo PROPERTIES OUTPUT_NAME "QLIC Demo")
  target_include_directories(qlic-web-demo PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/codec/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/jxl/include")
  target_compile_definitions(qlic-web-demo PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
  target_link_libraries(qlic-web-demo PRIVATE shell32 user32 ws2_32)
  add_dependencies(qlic-web-demo qlic-web-assets)
endif()

if(BUILD_TESTING)
  set(QLIC_TEST_TARGETS
    qlic-api-test
    qlic-api-shared-test
    qlic-compat-test
    qlic-lzms-robust-test
    qlic-parallel-test
    qlic-stream-test)

  add_executable(qlic-api-test tests/api_test.c)
  target_compile_definitions(qlic-api-test PRIVATE QLIC_STATIC)
  target_link_libraries(qlic-api-test PRIVATE qlic_static Threads::Threads)

  add_executable(qlic-api-shared-test tests/api_test.c)
  target_link_libraries(qlic-api-shared-test PRIVATE qlic_library Threads::Threads)

  add_executable(qlic-compat-test tests/compat_test.c)
  target_include_directories(qlic-compat-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/codec/include")
  target_compile_definitions(qlic-compat-test PRIVATE
    QLIC_STATIC QLIC_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  target_link_libraries(qlic-compat-test PRIVATE qlic_static)

  add_executable(qlic-lzms-robust-test
    tests/lzms_robust_test.c codec/src/lzms.c)
  target_include_directories(qlic-lzms-robust-test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/codec/src")

  add_executable(qlic-parallel-test tests/parallel_test.c codec/src/parallel.c)
  target_include_directories(qlic-parallel-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/codec/src")
  target_link_libraries(qlic-parallel-test PRIVATE Threads::Threads)

  add_executable(qlic-stream-test tests/stream_test.c codec/src/stream.c codec/src/parallel.c ${QLIC_MAP_SOURCES})
  target_include_directories(qlic-stream-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/codec/src")
  target_link_libraries(qlic-stream-test PRIVATE Threads::Threads)

  add_test(NAME qlic-api COMMAND qlic-api-test)
  add_test(NAME qlic-api-shared COMMAND qlic-api-shared-test)
  add_test(NAME qlic-compat COMMAND qlic-compat-test)
  add_test(NAME qlic-lzms-robust
    COMMAND qlic-lzms-robust-test
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/cpalette-lzms.qlic")
  add_test(NAME qlic-parallel COMMAND qlic-parallel-test)
  add_test(NAME qlic-stream COMMAND qlic-stream-test)
  if(UNIX AND NOT APPLE)
    add_test(NAME qlic-exports
      COMMAND "${CMAKE_COMMAND}"
        "-DNM=${CMAKE_NM}"
        "-DLIBRARY=$<TARGET_FILE:qlic_library>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_exports.cmake")
  endif()

  if(WIN32)
    target_compile_definitions(qlic-api-test PRIVATE
      WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(qlic-api-shared-test PRIVATE
      WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

    add_executable(qlic-win-util-test tests/win_util_test.c codec/src/win_util.c)
    target_include_directories(qlic-win-util-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/codec/src")
    target_compile_definitions(qlic-win-util-test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
    target_link_libraries(qlic-win-util-test PRIVATE shell32)

    add_executable(qlic-input-test tests/input_test.c codec/src/input.c)
    target_include_directories(qlic-input-test PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/codec/src"
      "${CMAKE_CURRENT_SOURCE_DIR}/third_party/jxl/include")
    target_compile_definitions(qlic-input-test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
    add_custom_command(TARGET qlic-input-test POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime/image-codecs"
        "$<TARGET_FILE_DIR:qlic-input-test>/image-codecs")

    add_executable(qlic-lzms-test tests/lzms_test.c codec/src/lzms.c)
    target_include_directories(qlic-lzms-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/codec/src")
    target_compile_definitions(qlic-lzms-test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(qlic-lzms-test PRIVATE cabinet)

    add_executable(qlic-wic-smoke tests/wic_smoke.c)
    target_compile_definitions(qlic-wic-smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
    target_link_libraries(qlic-wic-smoke PRIVATE windowscodecs ole32)
    add_dependencies(qlic-wic-smoke qlic-wic)

    list(APPEND QLIC_TEST_TARGETS
      qlic-win-util-test qlic-input-test qlic-lzms-test)
    add_test(NAME qlic-win-util COMMAND qlic-win-util-test)
    add_test(NAME qlic-input
      COMMAND qlic-input-test "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/input")
    add_test(NAME qlic-lzms
      COMMAND qlic-lzms-test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/cpalette-lzms.qlic"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gray-model-lzms.qlic"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/rgb-lzms.qlic")
    add_test(NAME qlic-input-cli
      COMMAND pwsh -NoProfile -File
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/input_cli_test.ps1"
        -Qlic "$<TARGET_FILE:qlic>"
        -Fixtures "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/input"
        -Output "${CMAKE_CURRENT_BINARY_DIR}/input-cli-test")
    add_test(NAME qlic-wic-direct COMMAND qlic-wic-smoke --self-test)
  elseif(TARGET qlic)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/cli-test")
    add_test(NAME qlic-linux-pack-png
      COMMAND qlic pack
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/input/base.png"
        "${CMAKE_CURRENT_BINARY_DIR}/cli-test/base-png.qlic"
        --threads all)
    if(TARGET qlic_wimlib)
      add_test(NAME qlic-linux-emits-lzms
        COMMAND "${CMAKE_COMMAND}"
          "-DINPUT=${CMAKE_CURRENT_BINARY_DIR}/cli-test/base-png.qlic"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_lzms_output.cmake")
      set_tests_properties(qlic-linux-emits-lzms PROPERTIES
        DEPENDS qlic-linux-pack-png)
      add_test(NAME qlic-linux-roundtrip-own-lzms
        COMMAND qlic unpack
          "${CMAKE_CURRENT_BINARY_DIR}/cli-test/base-png.qlic"
          "${CMAKE_CURRENT_BINARY_DIR}/cli-test/base-png-roundtrip.png")
      set_tests_properties(qlic-linux-roundtrip-own-lzms PROPERTIES
        DEPENDS qlic-linux-emits-lzms)
    endif()
    add_test(NAME qlic-linux-pack-bmp
      COMMAND qlic pack
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/input/base.bmp"
        "${CMAKE_CURRENT_BINARY_DIR}/cli-test/base-bmp.qlic")
    add_test(NAME qlic-linux-unpack-lzms
      COMMAND qlic unpack
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/rgb-lzms.qlic"
        "${CMAKE_CURRENT_BINARY_DIR}/cli-test/rgb-lzms.png")
    add_test(NAME qlic-linux-batch
      COMMAND qlic batch
        "${CMAKE_CURRENT_BINARY_DIR}/cli-test/batch"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/input/base.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/input/base.bmp"
        --threads 4)
    if(TARGET PkgConfig::QLIC_AVIF)
      add_test(NAME qlic-linux-pack-avif
        COMMAND qlic pack
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/input/lossless.avif"
          "${CMAKE_CURRENT_BINARY_DIR}/cli-test/base-avif.qlic")
      add_test(NAME qlic-linux-reject-lossy-avif
        COMMAND qlic pack
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/input/lossy.avif"
          "${CMAKE_CURRENT_BINARY_DIR}/cli-test/lossy-avif.qlic")
      set_tests_properties(qlic-linux-reject-lossy-avif
        PROPERTIES WILL_FAIL TRUE)
    endif()
    if(TARGET PkgConfig::QLIC_TIFF)
      add_test(NAME qlic-linux-pack-tiff
        COMMAND qlic pack
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/input/lossless.tiff"
          "${CMAKE_CURRENT_BINARY_DIR}/cli-test/base-tiff.qlic")
      add_test(NAME qlic-linux-reject-lossy-tiff
        COMMAND qlic pack
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/input/lossy.tiff"
          "${CMAKE_CURRENT_BINARY_DIR}/cli-test/lossy-tiff.qlic")
      set_tests_properties(qlic-linux-reject-lossy-tiff
        PROPERTIES WILL_FAIL TRUE)
    endif()
  endif()

  if(TARGET qlic)
    add_test(NAME qlic-cli-help COMMAND qlic --help)
    add_test(NAME qlic-cli-version COMMAND qlic version)
    add_test(NAME qlic-cli-info-native
      COMMAND qlic info "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/native.qlic")
    add_test(NAME qlic-cli-reject-unknown COMMAND qlic unknown)
    add_test(NAME qlic-cli-reject-invalid-threads
      COMMAND qlic info "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/native.qlic" --threads invalid)
    add_test(NAME qlic-cli-reject-extra-argument COMMAND qlic version extra)
    set_tests_properties(qlic-cli-version PROPERTIES PASS_REGULAR_EXPRESSION "${PROJECT_VERSION}")
    if(WIN32)
      set_tests_properties(qlic-cli-info-native PROPERTIES
        PASS_REGULAR_EXPRESSION
          "native-mode=[0-9][0-9]* native-transform=[0-9][0-9]* native-tile-log=[0-9][0-9]* native-adaptation=[0-9][0-9]*")
    else()
      set_tests_properties(qlic-cli-info-native PROPERTIES
        PASS_REGULAR_EXPRESSION "64x64 frames=1 animated=0")
    endif()
    set_tests_properties(
      qlic-cli-reject-unknown
      qlic-cli-reject-invalid-threads
      qlic-cli-reject-extra-argument
      PROPERTIES WILL_FAIL TRUE)
  endif()
endif()

set(QLIC_LINK_TARGETS qlic_static qlic_library qlic-example-decode)
if(TARGET qlic)
  list(APPEND QLIC_LINK_TARGETS qlic)
endif()
if(WIN32)
  list(APPEND QLIC_LINK_TARGETS qlic-gui qlic-wic qlic-web-demo)
endif()
if(BUILD_TESTING)
  list(APPEND QLIC_LINK_TARGETS ${QLIC_TEST_TARGETS})
  if(WIN32)
    list(APPEND QLIC_LINK_TARGETS qlic-wic-smoke)
  endif()
endif()
set(QLIC_COMPILE_TARGETS qliccore ${QLIC_LINK_TARGETS})

if(QLIC_PGO STREQUAL "GENERATE")
  if(MSVC)
    set(QLIC_PGO_COMPILE_OPTIONS /clang:-fprofile-instr-generate)
  else()
    set(QLIC_PGO_COMPILE_OPTIONS -fprofile-instr-generate)
    set(QLIC_PGO_LINK_OPTIONS -fprofile-instr-generate)
  endif()
elseif(QLIC_PGO STREQUAL "USE")
  if(MSVC)
    set(QLIC_PGO_COMPILE_OPTIONS
      "/clang:-fprofile-instr-use=${QLIC_PGO_PROFILE}"
      /clang:-Wno-profile-instr-unprofiled
      /clang:-Wno-profile-instr-out-of-date)
  else()
    set(QLIC_PGO_COMPILE_OPTIONS
      "-fprofile-instr-use=${QLIC_PGO_PROFILE}"
      -Wno-profile-instr-unprofiled
      -Wno-profile-instr-out-of-date)
  endif()
endif()
if(QLIC_PGO_COMPILE_OPTIONS)
  foreach(target IN LISTS QLIC_COMPILE_TARGETS)
    target_compile_options(${target} PRIVATE ${QLIC_PGO_COMPILE_OPTIONS})
  endforeach()
endif()
if(QLIC_PGO_LINK_OPTIONS)
  foreach(target IN LISTS QLIC_LINK_TARGETS)
    target_link_options(${target} PRIVATE ${QLIC_PGO_LINK_OPTIONS})
  endforeach()
endif()

if(MSVC)
  if(QLIC_SANITIZE AND CMAKE_C_COMPILER_ID MATCHES "Clang")
    set_property(TARGET ${QLIC_COMPILE_TARGETS} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
  else()
    set_property(TARGET ${QLIC_COMPILE_TARGETS} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
  foreach(target IN LISTS QLIC_COMPILE_TARGETS)
    # release archives are reproducible only when compiler output is too
    target_compile_options(${target} PRIVATE /O2 /Oi /Ot /Gy /Gw /W4 /WX /Brepro)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
      target_compile_options(${target} PRIVATE /clang:-O3 /clang:-Wall /clang:-Wextra /clang:-Wconversion /clang:-Wno-unsafe-buffer-usage)
    else()
      target_compile_options(${target} PRIVATE /GL /wd5105)
    endif()
  endforeach()
  foreach(target IN LISTS QLIC_LINK_TARGETS)
    target_link_options(${target} PRIVATE /OPT:REF /OPT:ICF)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
      target_link_options(${target} PRIVATE /timestamp:0)
    else()
      target_link_options(${target} PRIVATE /LTCG /Brepro)
    endif()
  endforeach()
  if(CMAKE_C_COMPILER_ID MATCHES "Clang" AND NOT QLIC_SANITIZE)
    target_compile_options(qlic PRIVATE /clang:-flto=thin)
  endif()
else()
  foreach(target IN LISTS QLIC_COMPILE_TARGETS)
    target_compile_options(${target} PRIVATE -O3 -Wall -Wextra)
  endforeach()
endif()

if(QLIC_SANITIZE)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "QLIC_SANITIZE requires Clang")
  endif()
  if(MSVC)
    get_filename_component(QLIC_CLANG_BIN "${CMAKE_C_COMPILER}" DIRECTORY)
    file(GLOB QLIC_CLANG_RT_DIRS "${QLIC_CLANG_BIN}/../lib/clang/*/lib/windows")
    list(SORT QLIC_CLANG_RT_DIRS COMPARE NATURAL ORDER DESCENDING)
    list(LENGTH QLIC_CLANG_RT_DIRS QLIC_CLANG_RT_COUNT)
    if(QLIC_CLANG_RT_COUNT EQUAL 0)
      message(FATAL_ERROR "Could not find the LLVM sanitizer runtime")
    endif()
    list(GET QLIC_CLANG_RT_DIRS 0 QLIC_CLANG_RT_DIR)
    set(QLIC_ASAN_DYNAMIC "${QLIC_CLANG_RT_DIR}/clang_rt.asan_dynamic-x86_64.lib")
    set(QLIC_ASAN_THUNK "${QLIC_CLANG_RT_DIR}/clang_rt.asan_dynamic_runtime_thunk-x86_64.lib")
    set(QLIC_ASAN_DLL "${QLIC_CLANG_RT_DIR}/clang_rt.asan_dynamic-x86_64.dll")
    if(NOT EXISTS "${QLIC_ASAN_DYNAMIC}" OR NOT EXISTS "${QLIC_ASAN_THUNK}" OR NOT EXISTS "${QLIC_ASAN_DLL}")
      message(FATAL_ERROR "Could not find the LLVM sanitizer runtime files")
    endif()
    foreach(target IN LISTS QLIC_COMPILE_TARGETS)
      target_compile_options(${target} PRIVATE /fsanitize=address /Zi /Oy-)
    endforeach()
    foreach(target IN LISTS QLIC_LINK_TARGETS)
      if(NOT target STREQUAL "qlic_static")
        target_link_directories(${target} PRIVATE "${QLIC_CLANG_RT_DIR}")
        target_link_libraries(${target} PRIVATE "${QLIC_ASAN_DYNAMIC}")
        target_link_options(${target} PRIVATE "/WHOLEARCHIVE:${QLIC_ASAN_THUNK}")
        add_custom_command(TARGET ${target} POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different "${QLIC_ASAN_DLL}" "$<TARGET_FILE_DIR:${target}>")
      endif()
    endforeach()
  else()
    foreach(target IN LISTS QLIC_COMPILE_TARGETS)
      target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    endforeach()
    foreach(target IN LISTS QLIC_LINK_TARGETS)
      if(NOT target STREQUAL "qlic_static")
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
      endif()
    endforeach()
  endif()
endif()

set(QLIC_RUNTIME_TARGETS)
if(TARGET qlic)
  list(APPEND QLIC_RUNTIME_TARGETS qlic)
endif()
if(WIN32)
  list(APPEND QLIC_RUNTIME_TARGETS qlic-gui qlic-wic qlic-web-demo)
endif()
if(QLIC_RUNTIME_TARGETS)
  install(TARGETS ${QLIC_RUNTIME_TARGETS}
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_BINDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
endif()
if(WIN32)
  install(DIRECTORY runtime/image-codecs
    DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
install(TARGETS qlic_library qlic_static
  EXPORT qlicTargets
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES codec/include/qlic/qlic.h
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/qlic")
install(EXPORT qlicTargets
  FILE qlicTargets.cmake
  NAMESPACE qlic::
  DESTINATION "${QLIC_CMAKE_INSTALL_DIR}")
install(FILES
  "${QLIC_GENERATED_DIR}/qlicConfig.cmake"
  "${QLIC_GENERATED_DIR}/qlicConfigVersion.cmake"
  DESTINATION "${QLIC_CMAKE_INSTALL_DIR}")
install(FILES README.md LICENSE NOTICE SECURITY.md DESTINATION "${CMAKE_INSTALL_DATADIR}/qlic")
install(FILES third_party/lzms/LICENSE
  DESTINATION "${CMAKE_INSTALL_DATADIR}/qlic/third_party/lzms")
install(FILES third_party/wimlib/README.md
  DESTINATION "${CMAKE_INSTALL_DATADIR}/qlic/third_party/wimlib")
install(DIRECTORY docs/ DESTINATION "${CMAKE_INSTALL_DATADIR}/qlic/docs")
if(WIN32)
  install(FILES install-wic.ps1 uninstall-wic.ps1 DESTINATION "${CMAKE_INSTALL_DATADIR}/qlic")
endif()
