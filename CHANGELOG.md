# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project **only** adheres to the following _(as defined at [Semantic Versioning](https://semver.org/spec/v2.0.0.html))_:

> Given a version number MAJOR.MINOR.PATCH, increment the:
> 
> - MAJOR version when you make incompatible API changes
> - MINOR version when you add functionality in a backward compatible manner
> - PATCH version when you make backward compatible bug fixes

## [0.6.0] - 2025-08-22

This release makes the HTTP API compatible with iRODS 5, improves compatibility with earlier versions of iRODS, expands support for targeting replicas for I/O operations, and improves write performance to data objects.

It removes the OAuth client mode, requiring the HTTP API to be deployed as a protected resource. The OpenID Connect configuration now requires deployments to declare the method used for access token validation as well.

Dockerfiles for EL8 and EL9 have been added to make building packages easier.

### Changed

- Use jsoncons C++ library for configuration validation (#261).
- Make strict introspection endpoint `aud` check an opt-in (#375).
- Make configuration of SSL/TLS certificates for OIDC optional (#443).
- Make `parallel_write_init` operation return an iRODS error code on bad output stream (#445).
- Make `write` operation return an iRODS error code on bad output stream (#445).
- Require explicit selection of OIDC access token validation method in configuration file (#448).
- Return HTTP status code of 400 when `read` operation fails to open input stream (#460).
- Do not treat warnings as errors for Docker builders (#464).
- Migrate to system CMake (irods/irods#8330).

### Removed

- Remove support for running as an OIDC client (#416).
- Remove support for resource owner password credentials grant and authorization code grant (#416).
- Remove non-standard behavior for symmetrically signed JWTs (#419).

### Fixed

- Fix user mapping plugin paths in README (#377).
- Fix linker error for Ubuntu 20.04 (#384).
- Fix `KeyValPair` memory leak (#389).
- Fix lack of `return` keyword in `set_log_level()` (#391).
- Fix segfault on multiple calls to `parallel_write_init` operation (#402).
- Terminate outbound HTTP connections to OIDC Provider sooner (#412).
- Fix compatibility issues for `modify_replica` operation (#420).
- Fix compatibility issues for `create_group` operation (#420).
- Close output streams on invalid offset values (#446).

### Added

- Add support for replica number parameter to `read`, `write`, and `replicate` operations (#128).
- Improve write performance to data objects (#208).
- Implement `truncate` operation for /data-objects endpoint (#271).
- Document SSL/TLS termination assumptions (#386).
- Add support for Undefined Behavior Sanitizer (#390).
- Add support to `parallel_write_init` for targeting a specific root resource (#399).
- Add Dockerfiles for building EL8 and EL9 packages (#406).
- Make server compatible with iRODS 5 (#410, #411).
- Implement custom `procApiRequest` for forward/backward compatibility (#420).
- Allow user claim plugin to modify claim using regular expression (#421).
- Add network diagrams for common deployment patterns (#429).
- Implement support for modifying the access time of a replica (iRODS 5 only) (#431).

## [0.5.0] - 2024-11-13

This release includes a user-mapping plugin system that enables dynamic user-mapping between OpenID Connect users and iRODS users. It also includes improved security through token validation.

User-mapping functionality from previous versions of the HTTP API is now provided through the user-mapping plugin system. See [Mapping OpenID Users to iRODS](https://github.com/irods/irods_client_http_api/tree/0.5.0?tab=readme-ov-file#mapping-openid-users-to-irods) for details.

### Changed

- Improve documentation for `tls_certificates_directory` OIDC property (#243).
- Provide clear message on OIDC connection error (#244).
- Collapse management of version number to single point of control (#269).
- Improve handling of inaccessible configuration file on startup (#337).

### Fixed

- Disable SIGPIPE signal for iRODS connections (#333).
- Server no longer enters infinite loop when listening socket is already bound (#335).
- Server verifies OIDC-mapped iRODS username exists in catalog before returning bearer token (#338).
- Server terminates on startup if configuration received from OIDC Provider does not contain required information (#356).
- Leave space for null-terminating byte in destination buffer when using `std::string::copy` (#365).

### Added

- Implement validation of OIDC ID tokens in client mode (#107).
- Implement validation of OAuth Token Introspection response (#270).
- Implement user-mapping plugin system (#293).
- Implement local validation of JWT Access Tokens (#343, #359).

## [0.4.0] - 2024-08-26

This release includes several enhancements including more exposure of the iRODS API, logging of IP addresses, an improved multipart/form-data parser, and improved stability.

### Changed

- Log IP address of clients (#73).
- Remove **experimental** from package name (#159).
- Distribute long-running write operations across thread-pool (#196).
- Replace support for GenQuery2 API plugin with native iRODS 4.3.2 implementation (#272).
- Improve documentation (#285, #286, #294, #309, #310, #312, #323).
- Improve testing (#287).
- Stat of resource no longer interprets resource status string (#299).
- Remove unnecessary log functions (#324).
- Rename namespace alias for logger to avoid name collisions with C++ standard library (#325).
- Rename configuration property to better express its intent (#328).

### Removed

- Stat of data object no longer includes `registered` property in JSON object (#180).

### Fixed

- Rewrite multipart/form-data parser to handle embedded CRLFs (#273).
- Server no longer segfaults during OIDC bearer token resolution (#300).

### Added

- Implement support for modifying properties of resources (#133).
- Implement support for applying expiration to new tickets (#134).
- Implement support for adding and removing SpecificQueries (#276).
- Implement support for adding, removing, and modifying zones (#277).
