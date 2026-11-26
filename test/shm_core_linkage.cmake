# Inspect direct dependencies because Endpoint may load other Core libraries.
execute_process(
  COMMAND "${READELF}" --dynamic --wide "${BINARY}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE dynamic_section
  ERROR_VARIABLE error)
if(NOT result STREQUAL "0")
  message(FATAL_ERROR "readelf failed for ${BINARY}: ${error}")
endif()

string(REGEX MATCHALL "\\(NEEDED\\)[^\n]*\\[lib(shakoc|assets|conductor|hako)\\.so[^]]*\\]"
  core_dependencies "${dynamic_section}")
list(LENGTH core_dependencies core_dependency_count)
if(NOT core_dependency_count EQUAL 1 OR NOT core_dependencies MATCHES "\\[libshakoc\\.so")
  message(FATAL_ERROR "Expected only libshakoc as a direct Core dependency. Found: ${core_dependencies}")
endif()
