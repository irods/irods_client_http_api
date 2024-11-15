FROM quay.io/keycloak/keycloak:latest as builder

# Configure stuff
ENV KEYCLOAK_ADMIN=admin
ENV KEYCLOAK_ADMIN_PASSWORD=admin

# Optimized build thing
RUN /opt/keycloak/bin/kc.sh build
FROM quay.io/keycloak/keycloak:latest
COPY --from=builder /opt/keycloak/ /opt/keycloak/

# Use our exported realm
COPY example-realm-export.json /opt/keycloak/data/import/realm-export.json

# Standard entrypoint
ENTRYPOINT ["/opt/keycloak/bin/kc.sh"]
