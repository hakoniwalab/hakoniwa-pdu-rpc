if(HAKO_PDU_ENDPOINT_INCLUDE_DIR)
  set(HAKONIWA_PDU_ENDPOINT_INCLUDE_DIR "${HAKO_PDU_ENDPOINT_INCLUDE_DIR}")
else()
  find_path(HAKONIWA_PDU_ENDPOINT_INCLUDE_DIR
    NAMES hakoniwa/pdu/endpoint.hpp
    PATHS
      ${HAKO_PDU_ENDPOINT_PREFIX}
      ${HAKONIWA_PDU_ENDPOINT_ROOT}
      ${HAKONIWA_PDU_ENDPOINT_DIR}
      /usr/local
      /usr
      /opt/homebrew
    PATH_SUFFIXES
      include
  )
endif()

if(HAKO_PDU_ENDPOINT_LIBRARY)
  set(HAKONIWA_PDU_ENDPOINT_LIBRARY "${HAKO_PDU_ENDPOINT_LIBRARY}")
else()
  find_library(HAKONIWA_PDU_ENDPOINT_LIBRARY
    NAMES hakoniwa_pdu_endpoint
    PATHS
      ${HAKO_PDU_ENDPOINT_PREFIX}
      ${HAKONIWA_PDU_ENDPOINT_ROOT}
      ${HAKONIWA_PDU_ENDPOINT_DIR}
      /usr/local
      /usr
      /opt/homebrew
    PATH_SUFFIXES
      lib
      lib64
  )
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(HakoniwaPduEndpoint
  REQUIRED_VARS HAKONIWA_PDU_ENDPOINT_INCLUDE_DIR HAKONIWA_PDU_ENDPOINT_LIBRARY
)

if(HakoniwaPduEndpoint_FOUND AND NOT TARGET hakoniwa_pdu_endpoint)
  add_library(hakoniwa_pdu_endpoint UNKNOWN IMPORTED)
  set(_hako_pdu_endpoint_include_dirs "${HAKONIWA_PDU_ENDPOINT_INCLUDE_DIR}")
  if(HAKO_PDU_ENDPOINT_PREFIX)
    list(APPEND _hako_pdu_endpoint_include_dirs
      "${HAKO_PDU_ENDPOINT_PREFIX}/include"
      "${HAKO_PDU_ENDPOINT_PREFIX}/include/hakoniwa"
    )
  endif()
  set_target_properties(hakoniwa_pdu_endpoint PROPERTIES
    IMPORTED_LOCATION "${HAKONIWA_PDU_ENDPOINT_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${_hako_pdu_endpoint_include_dirs}"
  )
endif()

mark_as_advanced(HAKONIWA_PDU_ENDPOINT_INCLUDE_DIR HAKONIWA_PDU_ENDPOINT_LIBRARY)
