# syntax=docker/dockerfile:1

# Start with a 'builder' image that will be
# used as scratch (i.e. dirty environment)
FROM quay.io/keycloak/keycloak:23.0.6 AS builder

# Build since import command uses --optimized
RUN /opt/keycloak/bin/kc.sh build

# Use our exported realm made for testing
COPY example-realm-export.json /realm-export.json

# Import realm at build time, this will shorten startup time
RUN /opt/keycloak/bin/kc.sh import --file /realm-export.json

# Use clean image and copy over changes made in builder image
FROM quay.io/keycloak/keycloak:23.0.6
COPY --from=builder /opt/keycloak/ /opt/keycloak/

# Configure environment variables
# TODO: Figure out if this is better left in a compose file...
ENV KEYCLOAK_ADMIN=admin
ENV KEYCLOAK_ADMIN_PASSWORD=admin

# Standard entrypoint
ENTRYPOINT ["/opt/keycloak/bin/kc.sh"]
