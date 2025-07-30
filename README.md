# iRODS HTTP API

A project that presents an iRODS Zone as HTTP v1.1.

![iRODS HTTP API network diagram - HTTP Basic Authentication](http_api_diagram-basic_auth.png)

## Quickstart (Running from Docker Hub Image)

Generate a local configuration JSON file.

```
docker run --rm irods/irods_http_api --dump-config-template > config.json
```

Edit/update the template `config.json` file (point to your iRODS Zone, remove OIDC and TLS sections, etc.).

```
vim config.json
```

Launch the HTTP API with your customized configuration file to check for success/errors.

```
docker run --rm --name irods_http_api \
    -v ./config.json:/config.json:ro \
    -p 9000:9000 \
    irods/irods_http_api
```

Then, the HTTP API will be available.

```
$ curl -X POST -u rods:rods \
    http://localhost:9000/irods-http-api/<version>/authenticate
568bbfc2-7d19-4723-b659-bb9325f9b076

$ curl -s http://localhost:9000/irods-http-api/<version>/collections \
    -H 'Authorization: Bearer 568bbfc2-7d19-4723-b659-bb9325f9b076' \
    --data-urlencode 'op=stat' \
    --data-urlencode 'lpath=/tempZone/home/rods' -G | jq
{
  "inheritance_enabled": false,
  "irods_response": {
    "status_code": 0
  },
  "modified_at": 1699448576,
  "permissions": [
    {
      "name": "rods",
      "perm": "own",
      "type": "rodsadmin",
      "zone": "tempZone"
    },
    {
      "name": "alice",
      "perm": "read_object",
      "type": "groupadmin",
      "zone": "tempZone"
    }
  ],
  "registered": true,
  "type": "collection"
}
```

## Documentation

API documentation can be found in [API.md](./API.md).

## Build Dependencies

- iRODS development package _(4.3.2 or later)_
- iRODS externals package for boost
- iRODS externals package for nlohmann-json
- iRODS externals package for spdlog 
- Curl development package
- OpenSSL development package

## Build

> [!IMPORTANT]
> As documented under [Build Dependencies](#build-dependencies), the minimum version requirement for the iRODS development package is 4.3.2 or later. As a result, support for the [GenQuery2 API plugin](https://github.com/irods/irods_api_plugin_genquery2) has been removed. To use GenQuery2, the iRODS HTTP API must be connected to a server running iRODS 4.3.2 or later.

To build this project, follow the normal CMake build steps.
```bash
mkdir build # Preferably outside of the repository
cd build
cmake /path/to/repository
make package # Use -j to use more parallelism.
```

Upon success, you should have an installable package.

## Docker

This project provides two types of Dockerfiles, some for building packages on various operating systems, and one for running the application.

> [!IMPORTANT]
> All commands in the sections that follow assume you are located in the root of the repository.

### The Builder Image

The builder image is responsible for building the iRODS HTTP API package. Before you can use it, you must build the image. To do that, run the following:
```bash
docker build -t irods-http-api-builder -f irods_builder.ub22.Dockerfile .
```

The builder image defaults to installing the development packages of the minimum supported iRODS version, 4.3.2. To install a later version of the development packages, set the `irods_version` parameter. The following is an example which demonstrates building the builder with development packages for iRODS 5.0.1.
```bash
docker build -t irods-http-api-builder -f irods_builder.ub22.Dockerfile --build-arg 'irods_version=5.0.1-0~jammy' .
```

> [!Note]
> Using updated iRODS packages can sometimes resolve compatibility issues. This is particularly true when the client is built against a version of iRODS which is older than the iRODS server which it is connecting to.

With the builder image in hand, all that's left is to build the iRODS HTTP API project. The builder image is designed to compile code sitting on your machine. This is important because it gives you the ability to build any fork or branch of the project. **Keep in mind the [GenQuery2 API plugin](https://github.com/irods/irods_api_plugin_genquery2) is no longer supported.**

Building the package requires mounting the project into the container at the appropriate location. The command you run should look similar to the one below. **Don't forget to create the directory which will hold your packages!**
```bash
docker run -it --rm \
    -v /path/to/irods_client_http_api:/http_api_source:ro \
    -v /path/to/packages_directory:/packages_output \
    irods-http-api-builder
```

If everything succeeds, you will have a DEB package in the local directory you mapped to **/packages_output**.

### The Runner Image

The runner image is responsible for running the iRODS HTTP API. Building the runner image requires the DEB package for the iRODS HTTP API to exist on the local machine. See the previous section for details on generating the package.

To build the image, run the following command:
```bash
docker build -t irods-http-api-runner \
    -f irods_runner.Dockerfile \
    /path/to/packages/directory
```

If all goes well, you will have a containerized iRODS HTTP API server! You can verify this by checking the version information. Below is an example.
```bash
$ docker run -it --rm irods-http-api-runner -v
irods_http_api v<version>-<build_sha>
```

### Launching the Container

To run the containerized server, you need to provide a configuration file at the correct location. If you do not have a configuration file already, see [Configuration](#configuration) for details.

To launch the server, run the following command:
```bash
docker run -d --rm --name irods_http_api \
    -v /path/to/config/file:/config.json:ro \
    -p 9000:9000 \
    irods-http-api-runner
```

The first thing the server will do is validate the configuration. If the configuration fails validation, the server will exit immediately. If the configuration passes validation, then congratulations, you now have a working iRODS HTTP API server!

You can view the log output using `docker logs -f` or by passing `-it` to `docker run` instead of `-d`.

If for some reason the default schema file is not sufficient, you can instruct the iRODS HTTP API to use a different schema file. See the following example.
```bash
# Generate the default JSON schema.
docker run -it --rm irods-http-api-runner --dump-default-jsonschema > schema.json

# Tweak the schema.
vim schema.json

# Launch the server with the new schema file.
docker run -d --rm --name irods_http_api \
    -v /path/to/config/file:/config.json:ro \
    -v ./schema.json:/jsonschema.json:ro \
    -p 9000:9000 \
    irods-http-api-runner \
    --jsonschema-file /jsonschema.json
```

### Stopping the Container

If the container was launched with `-it`, use **CTRL-C** or `docker container stop <container_name>` to shut it down.

If the container was launched with `-d`, use `docker container stop <container_name>`.

## Configuration

Before you can run the server, you'll need to create a configuration file.

You can generate a configuration file by running the following:
```bash
irods_http_api --dump-config-template > config.json
```

> [!IMPORTANT]
> `--dump-config-template` does not produce a fully working configuration. It must be updated before it can be used.

### Configuration File Structure

The JSON structure below represents the default configuration.

Notice how some of the configuration values are wrapped in angle brackets (e.g. `"<string>"`). These are placeholder values that must be updated before launch.

> [!IMPORTANT]
> The comments in the JSON structure are there for explanatory purposes and must not be included in your configuration. Failing to follow this requirement will result in the server failing to start up.

```js
{
    // Defines HTTP options that affect how the client-facing component of the
    // server behaves.
    "http_server": {
        // The hostname or IP address to bind.
        // "0.0.0.0" instructs the server to listen on all network interfaces.
        "host": "0.0.0.0",

        // The port used to accept incoming client requests.
        "port": 9000,

        // The minimum log level needed before logging activity.
        //
        // The following values are supported:
        // - trace
        // - debug
        // - info
        // - warn
        // - error
        // - critical
        "log_level": "info",

        // Defines options that affect various authentication schemes.
        "authentication": {
            // The amount of time that must pass before checking for expired
            // bearer tokens.
            "eviction_check_interval_in_seconds": 60,

            // Defines options for the "Basic" authentication scheme.
            "basic": {
                // The amount of time before a user's authentication
                // token expires.
                "timeout_in_seconds": 3600
            },

            // Defines required OIDC related configuration.
            "openid_connect": {
                // The amount of time before a user's authentication
                // token expires.
                "timeout_in_seconds": 3600,

                // The url of the OIDC provider, with a path leading to
                // where the .well-known configuration is.
                // The protocol will determine the default port used if
                // none is specified in the url.
                "provider_url": "https://oidc.example.org/realms/irods",

                // The client id given to the application by OIDC provider.
                "client_id": "irods_http_api",

                // The client secret used for accessing the introspection endpoint.
                // Optional unless running as a protected resource.
                "client_secret": "xxxxxxxxxxxxxxx",

                // The secret used when validating a JWT signed with
                // a symmetric algorithm (e.g. HS256).
                // If provided, it MUST be base64url encoded.
                "access_token_secret": "xxxxxxxxxxxxxxx",

                // Controls whether the HTTP API requires the presence of the
                // "aud" member in the introspection endpoint response. If set
                // to true and the "aud" member is NOT present, the provided
                // access token will be rejected.
                "require_aud_member_from_introspection_endpoint": false,

                // The amount of time before the OIDC Authorization Code grant
                // times out, requiring another attempt at authentication.
                "state_timeout_in_seconds": 600,

                // Defines relevant information related to the User Mapping plugin system.
                // Allows for the selection and configuration of the plugin.
                "user_mapping": {
                    // The full path to the desired plugin to load.
                    // See the section titled "Mapping OpenID Users to iRODS"
                    // for more details on available plugins.
                    "plugin_path": "/path/to/plugin/the_plugin.so",

                    // The configuration information required by
                    // the selected plugin to execute properly.
                    // See the section titled "Mapping OpenID Users to iRODS"
                    // for more details on plugin configuration.
                    "configuration": {
                    }
                },

                // A list of paths to the TLS certificates directories used to establish
                // secure connections between the HTTP API and the OpenID provider.
                // Use this setting to point to a non-standard certificate directory.
                // When set, this will not include the standard certificate directory
                // by default.
                "tls_certificate_directories": ["/path/to/certs"]
            }
        },

        // Defines options that affect how client requests are handled.
        "requests": {
            // The number of threads dedicated to servicing client requests.
            // When adjusting this value, consider adjusting "background_io/threads"
            // and "irods_client/connection_pool/size" as well.
            "threads": 3,

            // The maximum size allowed for the body of a request.
            "max_size_of_request_body_in_bytes": 8388608,

            // The amount of time allowed to service a request. If the timeout
            // is exceeded, the client's connection is terminated immediately.
            "timeout_in_seconds": 30
        },

        // Defines options that affect tasks running in the background.
        // These options are primarily related to long-running tasks.
        "background_io": {
            // The number of threads dedicated to background I/O.
            "threads": 6
        }
    },

    // Defines iRODS connection information.
    "irods_client": {
        // The hostname or IP of the target iRODS server.
        "host": "<string>",

        // The port of the target iRODS server.
        "port": 1247,

        // The zone of the target iRODS server.
        "zone": "<string>",

        // Defines options for secure communication with the target iRODS server.
        "tls": {
            // Controls whether the client and server communicate using TLS.
            //
            // The following values are supported:
            // - CS_NEG_REFUSE:    Do not use secure communication.
            // - CS_NEG_REQUIRE:   Demand secure communication.
            // - CS_NEG_DONT_CARE: Let the server decide.
            "client_server_policy": "CS_NEG_REFUSE",

            // The file containing trusted CA certificates in PEM format.
            //
            // Note that the certificates in this file are used in conjunction
            // with the system default trusted certificates.
            "ca_certificate_file": "<string>",

            // Defines the level of server certificate authentication to
            // perform.
            //
            // The following values are supported:
            // - none:     Authentication is skipped.
            // - cert:     The server verifies the certificate is signed by
            //             a trusted CA.
            // - hostname: Equivalent to "cert", but also verifies the FQDN
            //             of the iRODS server matches either the common
            //             name or one of the subjectAltNames.
            "verify_server": "cert",

            // Controls whether advanced negotiation is used.
            //
            // This option must be set to "request_server_negotiation" for
            // establishing secure communication. 
            "client_server_negotiation": "request_server_negotiation",

            // Defines the encryption algorithm used for secure communication.
            "encryption_algorithm": "AES-256-CBC",

            // Defines the size of key used for encryption.
            "encryption_key_size": 32,

            // Defines the number of hash rounds used for encryption.
            "encryption_hash_rounds": 16,

            // Defines the size of salt used for encryption.
            "encryption_salt_size": 8
        },

        // Controls how the HTTP API communicates with the iRODS server.
        //
        // When set to true, the following applies:
        // - Only APIs supported by the iRODS 4.2 series will be used.
        // - Connection pool settings are ignored.
        // - All HTTP requests will be served using a new iRODS connection.
        //
        // When set to false, the HTTP API will take full advantage of the
        // iRODS server's capabilities.
        //
        // This option should be used when the HTTP API is configured to
        // communicate with an iRODS 4.2 server.
        //
        // NOTE: The HTTP API will not be able to detect changes in policy
        // within the connected iRODS server unless this option is enabled.
        // See the "connection_pool" option for additional details.
        "enable_4_2_compatibility": false,

        // The credentials for the rodsadmin user that will act as a proxy
        // for all authenticated users.
        "proxy_admin_account": {
            "username": "<string>",
            "password": "<string>"
        },

        // Defines options for the connection pool.
        //
        // The options defined in this section are only used when
        // "enable_4_2_compatibility" is set to false.
        //
        // When connection pooling is used, the HTTP API will not be able
        // to detect changes in policy within the connected iRODS server.
        // Any changes in policy will require a restart of the HTTP API.
        //
        // If this is a concern, set "enable_4_2_compatibility" to true to
        // force the HTTP API to use a new iRODS connection for every HTTP
        // request. The additional connections will honor any iRODS server
        // policy changes, but will degrade overall performance.
        "connection_pool": {
            // The number of connections in the pool.
            "size": 6,

            // The amount of time that must pass before a connection is
            // renewed (i.e. replaced).
            "refresh_timeout_in_seconds": 600,

            // The number of times a connection can be fetched from the pool
            // before it is refreshed.
            "max_retrievals_before_refresh": 16,

            // Instructs the connection pool to track changes in resources.
            // If a change is detected, all connections will be refreshed.
            "refresh_when_resource_changes_detected": true
        },

        // The maximum number of parallel streams that can be associated to a
        // single parallel write handle.
        "max_number_of_parallel_write_streams": 3,

        // The maximum number of bytes that can be read from a data object
        // during a single read operation.
        "max_number_of_bytes_per_read_operation": 8192,

        // The maximum number of bytes that can be written to a data object
        // during a single write operation.
        "max_number_of_bytes_per_write_operation": 8192,

        // The number of rows that can be returned by a General or Specific
        // query. If the client specifies a number greater than the value
        // defined here, it will be clamped to this value. If the client does
        // not specify a value, it will be defaulted to this value.
        "max_number_of_rows_per_catalog_query": 15
    }
}
```

## Run

To run the server, do the following:
```bash
irods_http_api /path/to/config.json
```

To stop the server, you can use **CTRL-C** or send **SIGINT** or **SIGTERM** to the process.

## OpenID Connect

![iRODS HTTP API network diagram - OpenID Connect](http_api_diagram-openid_connect.png)

Some additional configuration is required to run the OpenID Connect portion of the HTTP API.
Following are a few points of interest.

### OpenID Provider Requirements and HTTP API Configuration

The OpenID Provider, at this moment, must support discovery via a well-known endpoint.
The URL to the OpenID Provider must be specified in the `provider_url` OIDC configuration parameter.

One should take care to ensure that `/.well-known/openid-configuration` is not included
in the configuration parameter, as this is included automatically.

The OpenID Provider must be running prior to starting the HTTP API server, otherwise, the HTTP API server
will not be able to query the required information from the desired OpenID Provider.

### Mapping OpenID Users to iRODS

Before you can use OpenID with iRODS, you must enable one of the two shipped plugins, or develop your own to suit your specific needs.

#### Local File plugin

Within the `user_mapping` stanza, set `plugin_path` to the absolute path of `libirods_http_api_plugin-local_file.so`.
On most Linux-based systems, the HTTP API user mapping plugins will be installed under `/usr/lib/irods_http_api/plugins/user_mapping`.

This plugin allows for the defining of mappings of iRODS users based on desired attributes.
The attributes specified within the file can be updated, and the plugin will reload the file
to update the mappings without having to restart the server.

##### Configuration

The required configuration for this plugin is as follows:

```json
"configuration": {
    "file_path": "/path/to/file.json"
}
```

Where `file_path` is the full path to a JSON file containing the mapping of desired attributes to an iRODS user.
An example of what the JSON file can contain is as follows:

```json
{
    "alice": {
        "email": "alice@example.org",
        "sub": "123-abc-456-xyz"
    },
    "bob": {
        "email": "bob@example.org",
        "phone": "56709"
    }
}
```

#### User Claim plugin

Within the `user_mapping` stanza, set `plugin_path` to the absolute path of `libirods_http_api_plugin-user_claim.so`.
On most Linux-based systems, the HTTP API user mapping plugins will be installed under `/usr/lib/irods_http_api/plugins/user_mapping`.

This plugin looks for a claim that provides a direct mapping of an authenticated OpenID user to an iRODS user.
This requires that you have the ability to add a claim within the OpenID Provider.

##### Configuration

The configuration for this plugin is as follows:

```json
"configuration": {
    "irods_user_claim": "claim_to_map_user"
}
```

In this particular example, `claim_to_map_user` is the claim that maps the OpenID user to an iRODS user.

### Plugin Development

To develop your own plugin, make sure to conform to the plugin interface defined in [interface.h](./plugins/user_mapping/include/irods/http_api/plugins/user_mapping/interface.h).
Further documentation on the interface functions are within the file.

## Secure Communication (SSL/TLS)

The HTTP API does not handle SSL/TLS termination itself. Deployments should plan to provide a reverse proxy to handle SSL/TLS termination _in front of_ the HTTP API for secure communication with the client.

Popular proxy servers include nginx, Apache httpd, and HAProxy.
