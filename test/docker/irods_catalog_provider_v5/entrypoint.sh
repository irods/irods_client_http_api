#! /bin/bash -e

echo "Waiting for iRODS catalog database to be ready"
catalog_db_hostname=irods-catalog
until pg_isready -h ${catalog_db_hostname} -d ICAT -U irods -q; do
    sleep 1
done
echo "iRODS catalog database is ready"

unattended_install_file=/unattended_install.json
if [ -f "${unattended_install_file}" ]; then
    echo "Running iRODS setup"

    # Add generated hostname as a recognizable alias.
    sed -i "s/CONTAINER_HOSTNAME_ALIAS/${HOSTNAME}/g" ${unattended_install_file}
    python3 /var/lib/irods/scripts/setup_irods.py --json_configuration_file ${unattended_install_file}

    # Move the input file used to configure the server out of the way so
    # the container is restartable.
    mv ${unattended_install_file} ${unattended_install_file}.processed

    # Enable physical quotas subsystem.
    sed -i '1i acRescQuotaPolicy { msiSetRescQuotaPolicy("on"); }' /etc/irods/core.re

    # Temporarily launch the server in the background.
    su - irods -c 'irodsServer -d'
    sleep 3

    # Enable logical quotas subsystem.
    su - irods -c 'iadmin set_grid_configuration logical_quotas enabled 1'

    # Shutdown the server.
    su - irods -c 'kill $(cat /run/irods/irods-server.pid)'
    sleep 3

    # Used by the docker healthcheck configuration to detect when the
    # iRODS server has completed setup.
    su - irods -c 'touch /tmp/irods_server_setup_complete.flag'
fi

echo "Starting server"
su - irods -c 'irodsServer --stdout'
