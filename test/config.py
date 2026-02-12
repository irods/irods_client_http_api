import logging
from jsonschema import validate

test_config = {
    'log_level': logging.INFO,

    'host': 'localhost',
    'port': 9000,
    'url_base': '/irods-http-api/<version>',

    'openid_connect': {
        'mode': 'client'
    },

    'rodsadmin': {
        'username': 'rods',
        'password': 'rods'
    },

    'rodsuser': {
        'username': 'http_api',
        'password': 'http_api'
    },

    'irods_zone': 'tempZone',
    'irods_server_hostname': 'localhost',

    'run_genquery2_tests': True,

    # TODO(#395): Set to True when this project includes an automated testing environment.
    'run_quota_tests': False
}

schema = {
    '$schema': 'http://json-schema.org/draft-07/schema#',
    '$id': 'https://schemas.irods.org/irods-http-api/test_config.json',
    'type': 'object',
    'properties': {
        'host': {
            'type': 'string'
        },
        'port': {
            'type': 'number'
        },
        'url_base': {
            'type': 'string'
        },
        'openid_connect': {
            'type': 'object',
            'properties': {
                'mode': {
                    'enum': [ 'client', 'protected_resource' ]
                }
            },
            'required': [ 'mode' ]
        },
        'rodsadmin': {
            '$ref': '#/definitions/login'
        },
        'rodsuser': {
            '$ref': '#/definitions/login'
        },
        'irods_zone': {
            'type': 'string'
        },
        'irods_server_hostname': {
            'type': 'string'
        },
        'run_genquery2_tests': {
            'type': 'boolean'
        }
    },
    'required': [
        'host',
        'port',
        'url_base',
        'openid_connect',
        'rodsadmin',
        'rodsuser',
        'irods_zone',
        'irods_server_hostname',
        'run_genquery2_tests'
    ],
    'definitions': {
        'login': {
            'type': 'object',
            'properties': {
                'username': {
                    'type': 'string'
                },
                'password': {
                    'type': 'string'
                }
            },
            'required': [ 'username', 'password' ]
        }
    }
}

validate(instance=test_config, schema=schema)
