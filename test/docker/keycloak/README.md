# Testing support for OpenID Connect via Keycloak

Provided in this directory is a [Dockerfile](Dockerfile) used for testing all OpenID related
features. This Dockerfile depends on [example_realm_export.json](example_realm_export.json) to
provide the realm used for testing.

## Future Considerations

Keycloak secret keys are only good for 10 years.
Be sure to update the secrets before then.
