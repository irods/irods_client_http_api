# Running Tests

To run all tests, perform the following steps:

## Startup a fresh iRODS server

The iRODS testing environment is assumed to be used to setup an iRODS server, with a minimum version of 4.3.2.
See the testing environment to setup a clean iRODS server: https://github.com/irods/irods_testing_environment

### Get the iRODS network information

We need the network that the HTTP API server is running on to have the iRODS server, the HTTP API server, and
the Keycloak server communicate with each other.

Following is an example of how to list all available docker networks with examples
of how the output may appear:

```console
$ docker network ls
NETWORK ID     NAME                              DRIVER    SCOPE
d06849108e9c   bridge                            bridge    local
371117285a13   host                              host      local
fbf586b6459f   none                              null      local
297992bf57a9   ubuntu-2204-postgres-14_default   bridge    local
```

In the example output, the desired network would be `ubuntu-2204-postgres-14_default`,
which would have been created by the testing environment with a running iRODS server.

The docker network used will be referred to as `<NETWORK>` in the following sections.

## Startup the Keycloak image for testing

To run tests that require an OpenID Provider, make sure you build the image.
To build the image simply run the following command in the `keycloak` directory:

```console
docker build -f irods-http-api-keycloak.Dockerfile -t irods-http-api-keycloak .
```

After which, run the following command to startup Keycloak:

```console
docker run --rm --network <NETWORK> -p 8080:8080 irods-http-api-keycloak start-dev
```

### Get detailed network information

Detailed information of the network will be required to properly configure the HTTP API server.
For example, in the HTTP API server's configuration file, `/http_server/host` will need to be set to the correct address given by the network
configuration.

To get this information run the following command:
```console
docker network inspect <NETWORK>
```

## Startup the iRODS HTTP API server

See the [iRODS HTTP API README](/README.md) on how to build the iRODS HTTP API server runner image. As well as launching the container.

Be sure to add the `--network <NETWORK>` flag, to have the container be able to communicate with the iRODS server, as well as the Keycloak server.

An example of how the command may look is as follows:
```console
docker run -d --rm --name irods_http_api \
    --network <NETWORK> \
    -v /path/to/config/file:/config.json:ro \
    -p 9000:9000 \
    irods-http-api-runner
```

### Ensure the HTTP API server can communicate with the iRODS server and the Keycloak image

If the HTTP API server cannot communicate with the Keycloak server, the HTTP API server should not
complete the startup process.

Additionally, if you [get detailed network information](#get-detailed-network-information), you should see all of the previously started containers listed within the network.

## Configure the [config.py](config.py) to the appropriate test configuration

The configuration should reflect that of the iRODS HTTP API server configuration, otherwise, the incorrect tests may be ran.
For example, having the HTTP API server set to `protected_resource` mode, while leaving the test configuration in `client` mode
will produce false errors.

## Install `pytest` to run all tests

Simply run the following command:
```console
pip install pytest
```

Afterwards you should be able to run all the tests by running the following command:
```console
pytest
```
