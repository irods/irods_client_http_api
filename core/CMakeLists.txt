include(GetGitRevisionDescription)
get_git_head_revision(IRODS_HTTP_API_GIT_REFSPEC IRODS_HTTP_API_GIT_SHA1)

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/include/irods/private/http_api/version.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/include/irods/private/http_api/version.hpp"
  @ONLY
)

add_library(
  irods_http_api_core
  OBJECT
  "${CMAKE_CURRENT_SOURCE_DIR}/src/common.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/crlf_parser.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/globals.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/process_stash.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/session.cpp"
)

target_link_libraries(
  irods_http_api_core
  PRIVATE
  irods_client
  "${IRODS_EXTERNALS_FULLPATH_BOOST}/lib/libboost_program_options.so"
  "${IRODS_EXTERNALS_FULLPATH_FMT}/lib/libfmt.so"
  CURL::libcurl
)

target_compile_definitions(
  irods_http_api_core
  PRIVATE
  ${IRODS_COMPILE_DEFINITIONS}
  ${IRODS_COMPILE_DEFINITIONS_PRIVATE}
  # The following macro requires that the application never attempt to
  # change log levels concurrently across threads.
  SPDLOG_NO_ATOMIC_LEVELS
  # Defines a macro which is used to construct the URL paths to individual
  # endpoint handlers (e.g. /irods-http-api/0.1.0/collections).
  IRODS_HTTP_API_BASE_URL="/irods-http-api/${IRODS_HTTP_API_VERSION}"
)

target_include_directories(
  irods_http_api_core
  PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_BINARY_DIR}/include"
  "${IRODS_HTTP_PROJECT_SOURCE_DIR}/endpoints/shared/include"
  "${IRODS_EXTERNALS_FULLPATH_BOOST}/include"
  "${IRODS_EXTERNALS_FULLPATH_FMT}/include"
  "${IRODS_EXTERNALS_FULLPATH_SPDLOG}/include"
)

set_target_properties(irods_http_api_core PROPERTIES EXCLUDE_FROM_ALL TRUE)