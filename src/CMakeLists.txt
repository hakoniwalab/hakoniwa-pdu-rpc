include(FetchContent)

if(NOT TARGET nlohmann_json::nlohmann_json)
  option(HAKO_PDU_RPC_INSTALL_NLOHMANN_JSON "Install bundled nlohmann_json when installing hakoniwa-pdu-rpc" ON)
  if(HAKO_PDU_RPC_INSTALL_NLOHMANN_JSON)
    set(JSON_Install ON CACHE INTERNAL "")
  endif()

  FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
  )
  FetchContent_MakeAvailable(nlohmann_json)
endif()

set(HAKONIWA_PDU_RPC_CPP_SOURCES
  rpc_server_endpoint_impl.cpp
  rpc_services_server.cpp
  rpc_client_endpoint_impl.cpp
  rpc_services_client.cpp
)

function(hakoniwa_pdu_rpc_configure_cpp_target target_name)
  target_compile_features(${target_name} PUBLIC cxx_std_20)
  target_include_directories(${target_name}
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
      $<BUILD_INTERFACE:${HAKONIWA_PDU_REGISTRY_DIR}/pdu/types>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  )

  target_link_libraries(${target_name}
    PUBLIC
      hakoniwa_pdu_endpoint::hakoniwa_pdu_endpoint
      nlohmann_json::nlohmann_json
  )
endfunction()

# BUILD_SHARED_LIBS=OFF preserves the original C++ library and test topology.
# BUILD_SHARED_LIBS=ON is the Python/CFFI distribution profile: keep the proven
# C++ implementation as a PIC static core and expose only the C facade through
# a shared library.
if(BUILD_SHARED_LIBS)
  add_library(hakoniwa_pdu_rpc_core STATIC ${HAKONIWA_PDU_RPC_CPP_SOURCES})
  hakoniwa_pdu_rpc_configure_cpp_target(hakoniwa_pdu_rpc_core)
  set_target_properties(hakoniwa_pdu_rpc_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    EXPORT_NAME rpc
  )

  add_library(hakoniwa_pdu_rpc SHARED c_rpc.cpp)
  target_compile_features(hakoniwa_pdu_rpc PRIVATE cxx_std_20)
  target_link_libraries(hakoniwa_pdu_rpc PRIVATE hakoniwa_pdu_rpc_core)
  set_target_properties(hakoniwa_pdu_rpc PROPERTIES EXPORT_NAME c)

  # Generate the Windows import library consumed by CFFI. The shared target
  # contains only the C facade plus the statically linked C++ implementation.
  if(WIN32)
    set_target_properties(hakoniwa_pdu_rpc PROPERTIES
      WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
  endif()

  add_library(hakoniwa_pdu_rpc::rpc ALIAS hakoniwa_pdu_rpc_core)
  add_library(hakoniwa_pdu_rpc::hakoniwa_pdu_rpc ALIAS hakoniwa_pdu_rpc_core)
  add_library(hakoniwa_pdu_rpc::c ALIAS hakoniwa_pdu_rpc)

  install(
    TARGETS hakoniwa_pdu_rpc_core hakoniwa_pdu_rpc
    EXPORT hakoniwa_pdu_rpcTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  )
else()
  add_library(hakoniwa_pdu_rpc
    ${HAKONIWA_PDU_RPC_CPP_SOURCES}
    c_rpc.cpp
  )
  hakoniwa_pdu_rpc_configure_cpp_target(hakoniwa_pdu_rpc)
  set_target_properties(hakoniwa_pdu_rpc PROPERTIES EXPORT_NAME rpc)

  add_library(hakoniwa_pdu_rpc::rpc ALIAS hakoniwa_pdu_rpc)
  add_library(hakoniwa_pdu_rpc::hakoniwa_pdu_rpc ALIAS hakoniwa_pdu_rpc)
  add_library(hakoniwa_pdu_rpc::c ALIAS hakoniwa_pdu_rpc)

  install(
    TARGETS hakoniwa_pdu_rpc
    EXPORT hakoniwa_pdu_rpcTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  )
endif()

install(
  DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(
  DIRECTORY "${HAKONIWA_PDU_REGISTRY_DIR}/pdu/types/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING
    PATTERN "*.h"
    PATTERN "*.hpp"
)
