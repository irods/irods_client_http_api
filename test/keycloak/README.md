# Testing support for OpenID Connect via Keycloak

Provided in this directory is a Dockerfile used for testing all OpenID related
features. This Dockerfile, [irods-http-api-keycloak.Dockerfile](irods-http-api-keycloak.Dockerfile),
depends on [example-realm-export.json](example-realm-export.json) to
provide the realm used for testing.

## Future Considerations

Keycloak secret keys are only good for 10 years.
Be sure to update the secrets before then.
