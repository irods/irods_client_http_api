# Blah blah blah

some words here...
probably:

+ don't forget to substitute `<version>` in the [config.py](config.py)
+ Ensure all OpenID options are valid
+ Have `pytest` installed for OpenID tests

## Keycloak

Provided in the [`keycloak`](keycloak/) directory is a Dockerfile used for testing
all OpenID related features. This Dockerfile, [irods-http-api-keycloak.Dockerfile](keycloak/irods-http-api-keycloak.Dockerfile),
depends on [example-realm-export.json](keycloak/example-realm-export.json) to
provide the realm used for testing.
