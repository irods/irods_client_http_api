add_library(
  irods_http_api_endpoint_users_groups
  OBJECT
  "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
)

target_compile_definitions(
  irods_http_api_endpoint_users_groups
  PRIVATE
  ${IRODS_COMPILE_DEFINITIONS}
  ${IRODS_COMPILE_DEFINITIONS_PRIVATE}
)

target_link_libraries(
  irods_http_api_endpoint_users_groups
  PRIVATE
  irods_client
  CURL::libcurl
  nlohmann_json::nlohmann_json
)

target_include_directories(
  irods_http_api_endpoint_users_groups
  PRIVATE
  "${IRODS_HTTP_PROJECT_SOURCE_DIR}/core/include"
  "${IRODS_HTTP_PROJECT_BINARY_DIR}/core/include"
  "${IRODS_HTTP_PROJECT_SOURCE_DIR}/endpoints/shared/include"
  "${IRODS_EXTERNALS_FULLPATH_BOOST}/include"
)

target_link_libraries(
  irods_http_api_endpoint_users_groups
  PUBLIC
  fmt::fmt
)

set_target_properties(irods_http_api_endpoint_users_groups PROPERTIES EXCLUDE_FROM_ALL TRUE)
