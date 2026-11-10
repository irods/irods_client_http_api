#! /bin/bash -e

echo "Waiting for iRODS catalog database to be ready"
catalog_db_hostname=irods-catalog
until pg_isready -h ${catalog_db_hostname} -d ICAT -U irods -q; do
    sleep 1
done
echo "iRODS catalog database is ready"

input_file=/setup.input
if [ -f "${input_file}" ]; then
    echo "Running iRODS setup"
    python3 /var/lib/irods/scripts/setup_irods.py < ${input_file}

    # Move the input file used to configure the server out of the way so
    # the container is restartable.
    mv ${input_file} ${input_file}.processed

    # Enable physical quotas subsystem.
    sed -i 'acRescQuotaPolicy { msiSetRescQuotaPolicy("on"); }' /etc/irods/core.re
fi

echo "Starting server"
su - irods -c '/usr/sbin/irodsServer -u'
