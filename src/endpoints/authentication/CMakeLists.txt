set(IRODS_HTTP_ENDPOINT authentication)

add_library(
  ${IRODS_HTTP_ENDPOINT}
  OBJECT
  impl.cpp)

target_compile_definitions(
  ${IRODS_HTTP_ENDPOINT}
  PRIVATE
  ${IRODS_COMPILE_DEFINITIONS}
  ${IRODS_COMPILE_DEFINITIONS_PRIVATE}
  # Needed so that beast::string_view can be passed to fmtlib.
  # Likely defines the following:
  #
  #  using string_view = std::string_view;
  #
  BOOST_BEAST_USE_STD_STRING_VIEW)

target_include_directories(
  ${IRODS_HTTP_ENDPOINT}
  PRIVATE
  ${IRODS_INCLUDE_DIRS}
  ${IRODS_EXTERNALS_FULLPATH_BOOST}/include
  ${IRODS_EXTERNALS_FULLPATH_FMT}/include
  ${IRODS_EXTERNALS_FULLPATH_SPDLOG}/include
  CURL::libcurl
  ${jwt_SOURCE_DIR}/include
  ${CMAKE_BINARY_DIR}/src # For version.hpp
  ${CMAKE_SOURCE_DIR}/src # For log.hpp
  ${CMAKE_SOURCE_DIR}/src/endpoints) # For common.hpp

target_link_libraries(
  ${IRODS_HTTP_ENDPOINT}
  PRIVATE
  irods_client
  ${IRODS_EXTERNALS_FULLPATH_FMT}/lib/libfmt.so
  jwt
  CURL::libcurl)
