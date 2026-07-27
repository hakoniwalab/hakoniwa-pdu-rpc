# Compatibility finder for Endpoint installations that predate the exported
# hakoniwa_pdu_endpoint CMake package. Modern builds should resolve the package
# config first and never enter this module.

set(_HAKO_ENDPOINT_PREFIX_HINTS "")
foreach(_value
    "${HAKO_PDU_ENDPOINT_PREFIX}"
    "${HAKONIWA_PDU_ENDPOINT_ROOT}"
    "${HAKONIWA_PDU_ENDPOINT_DIR}"
    "$ENV{HAKO_PDU_ENDPOINT_PREFIX}"
    "$ENV{HAKO_PDU_ENDPOINT_ROOT}")
  if(NOT "${_value}" STREQUAL "")
    list(APPEND _HAKO_ENDPOINT_PREFIX_HINTS "${_value}")
  endif()
endforeach()
if(NOT WIN32)
  list(APPEND _HAKO_ENDPOINT_PREFIX_HINTS /usr/local/hakoniwa /usr/local /usr /opt/homebrew)
endif()

if(HAKO_PDU_ENDPOINT_INCLUDE_DIR)
  set(HAKONIWA_PDU_ENDPOINT_INCLUDE_DIR "${HAKO_PDU_ENDPOINT_INCLUDE_DIR}")
else()
  find_path(HAKONIWA_PDU_ENDPOINT_INCLUDE_DIR
    NAMES hakoniwa/pdu/endpoint.hpp
    HINTS ${_HAKO_ENDPOINT_PREFIX_HINTS}
    PATH_SUFFIXES include
  )
endif()

if(HAKO_PDU_ENDPOINT_LIBRARY)
  set(HAKONIWA_PDU_ENDPOINT_LIBRARY "${HAKO_PDU_ENDPOINT_LIBRARY}")
else()
  find_library(HAKONIWA_PDU_ENDPOINT_LIBRARY
    NAMES hakoniwa_pdu_endpoint
    HINTS ${_HAKO_ENDPOINT_PREFIX_HINTS}
    PATH_SUFFIXES lib lib64 bin
  )
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(HakoniwaPduEndpoint
  REQUIRED_VARS HAKONIWA_PDU_ENDPOINT_INCLUDE_DIR HAKONIWA_PDU_ENDPOINT_LIBRARY
)

if(HakoniwaPduEndpoint_FOUND
   AND NOT TARGET hakoniwa_pdu_endpoint::hakoniwa_pdu_endpoint)
  add_library(hakoniwa_pdu_endpoint::hakoniwa_pdu_endpoint UNKNOWN IMPORTED)
  set_target_properties(hakoniwa_pdu_endpoint::hakoniwa_pdu_endpoint PROPERTIES
    IMPORTED_LOCATION "${HAKONIWA_PDU_ENDPOINT_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${HAKONIWA_PDU_ENDPOINT_INCLUDE_DIR}"
  )
endif()

mark_as_advanced(
  HAKONIWA_PDU_ENDPOINT_INCLUDE_DIR
  HAKONIWA_PDU_ENDPOINT_LIBRARY
)
