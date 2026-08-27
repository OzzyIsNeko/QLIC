if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR is required")
endif()

file(READ "${SOURCE_DIR}/scripts/install-wic.ps1" install_script)
file(READ "${SOURCE_DIR}/scripts/install-wic.cmd" install_launcher)
file(READ "${SOURCE_DIR}/scripts/uninstall-wic.ps1" uninstall_script)
file(READ "${SOURCE_DIR}/scripts/uninstall-wic.cmd" uninstall_launcher)
file(READ "${SOURCE_DIR}/packaging/README-wic.md" readme)
file(READ "${SOURCE_DIR}/codec/src/wic_codec.c" codec_source)
file(READ "${SOURCE_DIR}/scripts/package.ps1" package_script)

foreach(contents IN ITEMS install_script uninstall_script)
  if("${${contents}}" MATCHES "ValidateSet\\([^\\r\\n]*User" OR
     "${${contents}}" MATCHES "Scope[ \\t]*=[ \\t]*\"User\"")
    message(FATAL_ERROR "WIC package scripts must not advertise per-user registration")
  endif()
endforeach()
if(NOT install_script MATCHES "ValidateSet\\(\"Machine\"\\)" OR
   NOT uninstall_script MATCHES "ValidateSet\\(\"Machine\"\\)")
  message(FATAL_ERROR "WIC package scripts must require machine scope")
endif()
if(NOT install_script MATCHES "Verb RunAs" OR
   NOT install_script MATCHES "Administrator permission was not granted" OR
   NOT install_script MATCHES "QLIC WIC is installed and verified")
  message(FATAL_ERROR "WIC setup must self-elevate and report a verified result")
endif()
if(NOT install_launcher MATCHES "install-wic\.ps1" OR
   NOT install_launcher MATCHES "pause" OR
   NOT install_launcher MATCHES "QLIC WIC setup did not complete")
  message(FATAL_ERROR "WIC package must include a double-click setup launcher")
endif()
if(NOT uninstall_script MATCHES "Verb RunAs" OR
   NOT uninstall_script MATCHES "QLIC WIC was removed and verified" OR
   NOT uninstall_launcher MATCHES "uninstall-wic\.ps1" OR
   NOT uninstall_launcher MATCHES "pause" OR
   NOT uninstall_launcher MATCHES "QLIC WIC removal did not complete")
  message(FATAL_ERROR "WIC package must provide verified self-elevating removal")
endif()
if(NOT readme MATCHES "does not[^.]*offer a per-user WIC install")
  message(FATAL_ERROR "WIC package README must explain the machine-wide contract")
endif()
if(NOT install_script MATCHES "qlic-gui\\.exe" OR
   NOT install_script MATCHES "bundleHash" OR
   NOT package_script MATCHES "qlic-gui\\.exe[^\r\n]*\\$wic")
  message(FATAL_ERROR
    "WIC packaging must stage the content-addressed QLIC GUI viewer")
endif()
if(NOT codec_source MATCHES "qlic-gui\\.exe" OR
   NOT codec_source MATCHES "has_qlic_viewer")
  message(FATAL_ERROR "QLIC GUI must be the preferred original-file viewer")
endif()
string(REPLACE "\r\n" "\n" codec_source "${codec_source}")
string(FIND "${codec_source}"
  "HRESULT __stdcall DllRegisterServer(void) {\n  return reg_write(HKEY_LOCAL_MACHINE);\n}"
  register_index)
if(register_index EQUAL -1)
  message(FATAL_ERROR "DllRegisterServer must use the machine WIC catalog")
endif()
string(FIND "${codec_source}"
  "HRESULT __stdcall DllUnregisterServer(void) {\n  return reg_remove(HKEY_LOCAL_MACHINE);\n}"
  unregister_index)
if(unregister_index EQUAL -1)
  message(FATAL_ERROR "DllUnregisterServer must use the machine WIC catalog")
endif()

message(STATUS "QLIC WIC machine-wide installation contract verified")
