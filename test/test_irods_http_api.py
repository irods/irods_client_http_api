import config
import irods_error_codes

import base64
import concurrent.futures
import hashlib
import http.client
import json
import logging
import os
import requests
import socket
import sys
import time
import unittest

def setup_class(cls, opts):
    '''Initializes shared state needed by all test cases.

    This function is designed to be called in setUpClass().

    Arguments:
    cls -- The class to attach state to.
    opts -- A dict containing options for controlling the behavior of the function.
    '''

    # Used as a signal for determining whether setUpClass() succeeded or not.
    # If this results in being True, no tests should be allowed to run.
    cls._class_init_error = False
    cls._remove_rodsuser = False

    # Initialize the class logger.
    cls.logger = logging.getLogger(cls.__name__)

    log_level = config.test_config.get('log_level', logging.INFO)
    cls.logger.setLevel(log_level)

    ch = logging.StreamHandler()
    ch.setLevel(log_level)
    ch.setFormatter(logging.Formatter(f'[%(asctime)s] [{cls.__name__}] [%(levelname)s] %(message)s'))

    cls.logger.addHandler(ch)

    # Initialize state.

    if config.test_config.get('host', None) == None:
        cls.logger.debug('Missing configuration property: host')
        cls._class_init_error = True
        return

    if config.test_config.get('port', None) == None:
        cls.logger.debug('Missing configuration property: port')
        cls._class_init_error = True
        return

    if config.test_config.get('url_base', None) == None:
        cls.logger.debug('Missing configuration property: url_base')
        cls._class_init_error = True
        return

    cls.url_base = f"http://{config.test_config['host']}:{config.test_config['port']}{config.test_config['url_base']}"
    cls.url_endpoint = f'{cls.url_base}/{opts["endpoint_name"]}'

    cls.zone_name = config.test_config['irods_zone']
    cls.server_hostname = config.test_config['irods_server_hostname']

    # create_rodsuser cannot be honored if init_rodsadmin is set to False.
    # Therefore, return immediately.
    if not opts.get('init_rodsadmin', True):
        cls.logger.debug('init_rodsadmin is False. Class setup complete.')
        return

    # Authenticate as a rodsadmin and store the bearer token.
    cls.rodsadmin_username = config.test_config['rodsadmin']['username']
    r = requests.post(f'{cls.url_base}/authenticate', auth=(cls.rodsadmin_username, config.test_config['rodsadmin']['password']))
    cls.logger.debug(r.content)
    if r.status_code != 200:
        cls._class_init_error = True
        cls.logger.debug(f'Failed to authenticate as rodsadmin [{cls.rodsadmin_username}].')
        return
    cls.rodsadmin_bearer_token = r.text

    # Create a rodsuser for testing.
    if not opts.get('create_rodsuser', True):
        cls.logger.debug('create_rodsuser is False. Class setup complete.')
        return

    cls.rodsuser_username = config.test_config['rodsuser']['username']
    headers = {'Authorization': f'Bearer {cls.rodsadmin_bearer_token}'}
    r = requests.post(f'{cls.url_base}/users-groups', headers=headers, data={
        'op': 'create_user',
        'name': cls.rodsuser_username,
        'zone': cls.zone_name
    })
    cls.logger.debug(r.content)
    if r.status_code != 200:
        cls._class_init_error = True
        cls.logger.debug(f'Failed to create rodsuser [{cls.rodsuser_username}].')
        return
    cls._remove_rodsuser = True

    # Set the rodsuser's password.
    r = requests.post(f'{cls.url_base}/users-groups', headers=headers, data={
        'op': 'set_password',
        'name': cls.rodsuser_username,
        'zone': cls.zone_name,
        'new-password': config.test_config['rodsuser']['password']
    })
    cls.logger.debug(r.content)
    if r.status_code != 200:
        cls._class_init_error = True
        cls.logger.debug(f'Failed to set password for rodsuser [{cls.rodsuser_username}].')
        return

    # Authenticate as the rodsuser and store the bearer token.
    r = requests.post(f'{cls.url_base}/authenticate', auth=(cls.rodsuser_username, config.test_config['rodsuser']['password']))
    cls.logger.debug(r.content)
    if r.status_code != 200:
        cls._class_init_error = True
        cls.logger.debug(f'Failed to authenticate as rodsuser [{cls.rodsuser_username}].')
        return
    cls.rodsuser_bearer_token = r.text

    cls.logger.debug('Class setup complete.')

def tear_down_class(cls):
    if cls._class_init_error:
        return

    if not cls._remove_rodsuser:
        return

    headers = {'Authorization': f'Bearer {cls.rodsadmin_bearer_token}'}
    r = requests.post(f'{cls.url_base}/users-groups', headers=headers, data={
        'op': 'remove_user',
        'name': cls.rodsuser_username,
        'zone': cls.zone_name
    })
    cls.logger.debug(r.content)
    if r.status_code != 200:
        cls.logger.debug(f'Failed to remove rodsuser [{cls.rodsuser_username}].')
        return

def do_test_server_reports_error_when_http_method_is_not_supported(cls):
    r = requests.delete(cls.url_endpoint)
    logging.debug(r.content)
    cls.assertEqual(r.status_code, 405)

def do_test_server_reports_error_when_op_is_not_supported(cls, test_http_post_method=True):
    rodsuser_headers = {'Authorization': f'Bearer {cls.rodsadmin_bearer_token}'}
    invalid_op = {'op': 'invalid_op'}

    r = requests.get(cls.url_endpoint, headers=rodsuser_headers, params=invalid_op)
    logging.debug(r.content)
    cls.assertEqual(r.status_code, 400)

    if test_http_post_method:
        r = requests.post(cls.url_endpoint, headers=rodsuser_headers, data=invalid_op)
        logging.debug(r.content)
        cls.assertEqual(r.status_code, 400)

class test_authenticate_endpoint(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        setup_class(cls, {'endpoint_name': 'authenticate'})

    @classmethod
    def tearDownClass(cls):
        tear_down_class(cls)

    def setUp(self):
        self.assertFalse(self._class_init_error, 'Class initialization failed. Cannot continue.')

    def test_server_does_not_crash_when_incorrect_http_method_is_used_for_basic_authentication(self):
        # Get some general information about the HTTP API.
        r = requests.get(f'{self.url_base}/info')
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        # Skip this test if OpenID Connect is enabled.
        # When OpenID Connect is enabled, the HTTP status code of the auth request
        # changes, causing this test to fail. That behavior is expected.
        if r.json().get('openid_connect_enabled', False) == True:
            self.skipTest('Test produces incorrect result when OpenID Connect is enabled.')

        # Try to authenticate using the HTTP GET method.
        r = requests.get(self.url_endpoint, auth=('rods', 'rods'))
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 405)

        # Show the server is still running.
        r = requests.get(f'{self.url_base}/collections', headers={'Authorization': f'Bearer {self.rodsuser_bearer_token}'}, params={
            'op': 'stat',
            'lpath': f'/{self.zone_name}/home/{self.rodsuser_username}'
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

    def test_server_reports_error_when_http_method_is_not_supported(self):
        do_test_server_reports_error_when_http_method_is_not_supported(self)

class test_collections_endpoint(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        setup_class(cls, {'endpoint_name': 'collections'})

    @classmethod
    def tearDownClass(cls):
        tear_down_class(cls)

    def setUp(self):
        self.assertFalse(self._class_init_error, 'Class initialization failed. Cannot continue.')

    def test_common_operations(self):
        headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}
        collection_path = os.path.join('/', self.zone_name, 'home', self.rodsuser_username, 'common_ops')

        # Create a new collection.
        data = {'op': 'create', 'lpath': collection_path}
        r = requests.post(self.url_endpoint, headers=headers, data=data)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        stat_info = r.json()
        self.assertEqual(stat_info['irods_response']['status_code'], 0)

        # Stat the collection to show that it exists.
        params = {'op': 'stat', 'lpath': collection_path}
        r = requests.get(self.url_endpoint, headers=headers, params=params)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        stat_info = r.json()
        self.assertEqual(stat_info['irods_response']['status_code'], 0)

        # Rename the collection.
        new_collection_path = collection_path + '.renamed'
        data = {'op': 'rename', 'old-lpath': collection_path, 'new-lpath': new_collection_path}
        r = requests.post(self.url_endpoint, headers=headers, data=data)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Stat the original collection to show that it does not exist.
        params = {'op': 'stat', 'lpath': collection_path}
        r = requests.get(self.url_endpoint, headers=headers, params=params)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], irods_error_codes.NOT_A_COLLECTION)

        # Stat the new collection to show that it does exist.
        params = {'op': 'stat', 'lpath': new_collection_path}
        r = requests.get(self.url_endpoint, headers=headers, params=params)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Give another user permission to read the object.
        data = {
            'op': 'set_permission',
            'lpath': new_collection_path,
            'entity-name': self.rodsadmin_username,
            'permission': 'read_object'
        }
        r = requests.post(self.url_endpoint, headers=headers, data=data)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        # Show that the rodsadmin user now has read permission on the collection.
        params = {'op': 'stat', 'lpath': new_collection_path}
        r = requests.get(self.url_endpoint, headers=headers, params=params)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        stat_info = r.json()
        self.assertEqual(stat_info['irods_response']['status_code'], 0)
        self.assertEqual(len(stat_info['permissions']), 2)

        perms = stat_info['permissions']
        perm = perms[0] if perms[0]['name'] == self.rodsadmin_username else perms[1]
        self.assertEqual(perm['name'], self.rodsadmin_username)
        self.assertEqual(perm['zone'], self.zone_name)
        self.assertEqual(perm['type'], 'rodsadmin')
        self.assertEqual(perm['perm'], 'read_object')

        # Remove the collection.
        data = {'op': 'remove', 'lpath': new_collection_path}
        r = requests.post(self.url_endpoint, headers=headers, data=data)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        # Stat the collection to show that it does not exist.
        params = {'op': 'stat', 'lpath': new_collection_path}
        r = requests.get(self.url_endpoint, headers=headers, params=params)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        stat_info = r.json()
        self.assertEqual(stat_info['irods_response']['status_code'], irods_error_codes.NOT_A_COLLECTION)

    def test_creating_a_collection_with_insufficient_permissions_results_in_an_error(self):
        rodsuser_headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}
        collection = f'/{self.zone_name}/home/{self.rodsadmin_username}/not_allowed'

        # Attempting to create a collection with insufficient permissions and the
        # "create-intermediates" parameter not set to 1 results in an error.
        r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
            'op': 'create',
            'lpath': collection
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], irods_error_codes.OBJ_PATH_DOES_NOT_EXIST)

        # Attempting to create a collection with insufficient permissions and the
        # "create-intermediates" parameter set to 1 results in an error.
        r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
            'op': 'create',
            'lpath': f'{collection}/more/path/elements', # Guards against incorrect implementations.
            'create-intermediates': 1
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        # Sadly, there's nothing we can do within the HTTP API implementation to make
        # this call and the one before result in the same iRODS error code. Fixing this
        # requires a change in the iRODS server.
        self.assertEqual(r.json()['irods_response']['status_code'], irods_error_codes.SYS_INVALID_INPUT_PARAM)

    def test_stat_operation_returns_expected_json_structure(self):
        headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}
        params = {'op': 'stat', 'lpath': os.path.join('/', self.zone_name, 'home', self.rodsuser_username)}
        r = requests.get(self.url_endpoint, headers=headers, params=params)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        stat_info = r.json()
        self.assertEqual(stat_info['irods_response']['status_code'], 0)
        self.assertEqual(stat_info['type'], 'collection')
        self.assertEqual(stat_info['inheritance_enabled'], False)
        self.assertEqual(stat_info['registered'], True)
        self.assertGreater(int(stat_info['modified_at']), 0)
        self.assertGreater(len(stat_info['permissions']), 0)
        self.assertEqual(stat_info['permissions'][0]['name'], self.rodsuser_username)
        self.assertEqual(stat_info['permissions'][0]['zone'], self.zone_name)
        self.assertEqual(stat_info['permissions'][0]['type'], 'rodsuser')
        self.assertEqual(stat_info['permissions'][0]['perm'], 'own')

    def test_stat_operation_returns_error_when_executed_on_object_that_does_not_exist(self):
        headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}

        # Stat a non-existent collection.
        r = requests.get(self.url_endpoint, headers=headers, params={
            'op': 'stat',
            'lpath': os.path.join('/', self.zone_name, 'home', self.rodsuser_username, 'does_not_exist')
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], irods_error_codes.NOT_A_COLLECTION)

    def test_list_operation(self):
        headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}
        home_collection = os.path.join('/', self.zone_name, 'home', self.rodsuser_username)

        # Create nested collections.
        collection = os.path.join(home_collection, 'c0', 'c1', 'c2')
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'create',
            'lpath': collection,
            'create-intermediates': 1
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Create one data object in each collection.
        data_objects = [
            os.path.join(home_collection, 'c0', 'd0'),
            os.path.join(home_collection, 'c0', 'c1', 'd1'),
            os.path.join(home_collection, 'c0', 'c1', 'c2', 'd2')
        ]

        for name in data_objects:
            data_object = os.path.join('/', self.zone_name, 'home', self.rodsuser_username, name)
            r = requests.post(f'{self.url_base}/data-objects', headers=headers, data={'op': 'touch', 'lpath': data_object})
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # List only the contents of the home collection.
        r = requests.get(self.url_endpoint, headers=headers, params={'op': 'list', 'lpath': home_collection})
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(result['entries'], [os.path.dirname(data_objects[0])])

        # List the home collection recursively.
        r = requests.get(self.url_endpoint, headers=headers, params={'op': 'list', 'lpath': home_collection, 'recurse': 1})
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        result['entries'].sort()
        expected_result = [
            os.path.dirname(data_objects[0]),
            os.path.dirname(data_objects[1]),
            os.path.dirname(data_objects[2]),
            data_objects[0],
            data_objects[1],
            data_objects[2]
        ]
        expected_result.sort()
        self.assertEqual(result['entries'], expected_result)

        # Remove collections.
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'remove',
            'lpath': os.path.dirname(data_objects[0]),
            'recurse': 1,
            'no-trash': 1
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

    def test_modifying_metadata_atomically(self):
        headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}
        collection = os.path.join('/', self.zone_name, 'home', self.rodsuser_username)

        # Add metadata to the collection.
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'modify_metadata',
            'lpath': collection,
            'operations': json.dumps([
                {
                    'operation': 'add',
                    'attribute': 'a1',
                    'value': 'v1',
                    'units': 'u1'
                }
            ])
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Show the metadata exists on the collection.
        r = requests.get(f'{self.url_base}/query', headers=headers, params={
            'op': 'execute_genquery',
            'query': "select COLL_NAME where META_COLL_ATTR_NAME = 'a1' and META_COLL_ATTR_VALUE = 'v1' and META_COLL_ATTR_UNITS = 'u1'"
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(result['rows'][0][0], collection)

        # Remove the metadata from the collection.
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'modify_metadata',
            'lpath': collection,
            'operations': json.dumps([
                {
                    'operation': 'remove',
                    'attribute': 'a1',
                    'value': 'v1',
                    'units': 'u1'
                }
            ])
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Show the metadata no longer exists on the collection.
        r = requests.get(f'{self.url_base}/query', headers=headers, params={
            'op': 'execute_genquery',
            'query': "select COLL_NAME where META_COLL_ATTR_NAME = 'a1' and META_COLL_ATTR_VALUE = 'v1' and META_COLL_ATTR_UNITS = 'u1'"
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(len(result['rows']), 0)

    def test_modifying_permissions_atomically(self):
        headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}
        collection = os.path.join('/', self.zone_name, 'home', self.rodsuser_username)

        # Give the rodsadmin read permission on the rodsuser's home collection.
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'modify_permissions',
            'lpath': collection,
            'operations': json.dumps([
                {
                    'entity_name': self.rodsadmin_username,
                    'acl': 'read'
                }
            ])
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Show the rodsadmin now has permission to read the collection.
        r = requests.get(self.url_endpoint, headers=headers, params={
            'op': 'stat',
            'lpath': collection
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(len(result['permissions']), 2)
        #self.assertEqual(result['permissions'][0]['name'], self.rodsuser_username)
        #self.assertEqual(result['permissions'][0]['zone'], self.zone_name)
        #self.assertEqual(result['permissions'][0]['type'], 'rodsuser')
        #self.assertEqual(result['permissions'][0]['perm'], 'own')

        # Remove rodsadmin's permission on the collection.
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'modify_permissions',
            'lpath': collection,
            'operations': json.dumps([
                {
                    'entity_name': self.rodsadmin_username,
                    'acl': 'null'
                }
            ])
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Show the permissions have been removed.
        r = requests.get(self.url_endpoint, headers=headers, params={
            'op': 'stat',
            'lpath': collection
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(len(result['permissions']), 1)
        #self.assertEqual(result['permissions'][0]['name'], self.rodsuser_username)
        #self.assertEqual(result['permissions'][0]['zone'], self.zone_name)
        #self.assertEqual(result['permissions'][0]['type'], 'rodsuser')
        #self.assertEqual(result['permissions'][0]['perm'], 'own')

    def test_touch_operation_updates_mtime(self):
        rodsuser_headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}

        # Get the mtime of the home collection.
        collection = os.path.join('/', self.zone_name, 'home', self.rodsuser_username)
        r = requests.get(f'{self.url_base}/query', headers=rodsuser_headers, params={
            'op': 'execute_genquery',
            'query': f"select COLL_MODIFY_TIME where COLL_NAME = '{collection}'"
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(len(result['rows']), 1)
        original_mtime = int(result['rows'][0][0])
        self.assertGreater(original_mtime, 0)

        # Sleep for a short period of time to guarantee a difference in the mtime.
        time.sleep(2)

        # Update the mtime by calling touch.
        r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
            'op': 'touch',
            'lpath': collection
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Show the mtime has been updated.
        collection = os.path.join('/', self.zone_name, 'home', self.rodsuser_username)
        r = requests.get(f'{self.url_base}/query', headers=rodsuser_headers, params={
            'op': 'execute_genquery',
            'query': f"select COLL_MODIFY_TIME where COLL_NAME = '{collection}'"
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(len(result['rows']), 1)
        new_mtime = int(result['rows'][0][0])
        self.assertGreater(new_mtime, original_mtime)

    def test_touch_operation_reports_error_when_given_a_path_to_a_data_object(self):
        rodsuser_headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}
        data_object = os.path.join('/', self.zone_name, 'home', self.rodsuser_username, 'test_object')

        try:
            # Create a data object.
            r = requests.post(f'{self.url_base}/data-objects', headers=rodsuser_headers, data={
                'op': 'touch',
                'lpath': data_object
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Show an error is returned when the touch operation for a collection is given
            # a path pointing to a data object.
            r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
                'op': 'touch',
                'lpath': data_object
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 400)
            self.assertEqual(r.json()['irods_response']['status_code'], irods_error_codes.NOT_A_COLLECTION)

        finally:
            # Remove the data object.
            r = requests.post(f'{self.url_base}/data-objects', headers=rodsuser_headers, data={
                'op': 'remove',
                'lpath': data_object,
                'catalog-only': 0,
                'no-trash': 1
            })
            self.logger.debug(r.content)

    def test_touch_operation_does_not_create_collections_or_data_objects(self):
        rodsuser_headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}

        # Show the touch operation will silently ignore the fact that the target object
        # does not exist. This is intended.
        r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
            'op': 'touch',
            'lpath': f'/{self.zone_name}/home/{self.rodsuser_username}/does_not_exist'
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Show the touch operation did not create a new collection or data object (would be very bad).
        r = requests.get(self.url_endpoint, headers=rodsuser_headers, params={
            'op': 'stat',
            'lpath': f'/{self.zone_name}/home/{self.rodsuser_username}/does_not_exist'
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], irods_error_codes.NOT_A_COLLECTION)

        r = requests.get(f'{self.url_base}/data-objects', headers=rodsuser_headers, params={
            'op': 'stat',
            'lpath': f'/{self.zone_name}/home/{self.rodsuser_username}/does_not_exist'
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], irods_error_codes.NOT_A_DATA_OBJECT)

    def test_enabling_and_disabling_inheritance(self):
        rodsuser_headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}
        collection = f'/{self.zone_name}/home/{self.rodsuser_username}'

        # Show inheritance is not enabled.
        r = requests.get(self.url_endpoint, headers=rodsuser_headers, params={
            'op': 'stat',
            'lpath': collection
        })
        logging.debug(r.content)
        self.assertEqual(r.status_code, 200)
        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(result['inheritance_enabled'], False)

        # Enable inheritance.
        r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
            'op': 'set_inheritance',
            'lpath': collection,
            'enable': 1
        })
        logging.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Show inheritance is enabled.
        r = requests.get(self.url_endpoint, headers=rodsuser_headers, params={
            'op': 'stat',
            'lpath': collection
        })
        logging.debug(r.content)
        self.assertEqual(r.status_code, 200)
        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(result['inheritance_enabled'], True)

        # Disable inheritance.
        r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
            'op': 'set_inheritance',
            'lpath': collection,
            'enable': 0
        })
        logging.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Show inheritance is not enabled.
        r = requests.get(self.url_endpoint, headers=rodsuser_headers, params={
            'op': 'stat',
            'lpath': collection
        })
        logging.debug(r.content)
        self.assertEqual(r.status_code, 200)
        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(result['inheritance_enabled'], False)

    def test_server_reports_error_when_http_method_is_not_supported(self):
        do_test_server_reports_error_when_http_method_is_not_supported(self)

    def test_server_reports_error_when_op_is_not_supported(self):
        do_test_server_reports_error_when_op_is_not_supported(self)

    @unittest.skip('Test needs to be implemented.')
    def test_return_error_on_missing_parameters(self):
        pass

class test_data_objects_endpoint(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        setup_class(cls, {'endpoint_name': 'data-objects'})

    @classmethod
    def tearDownClass(cls):
        tear_down_class(cls)

    def setUp(self):
        self.assertFalse(self._class_init_error, 'Class initialization failed. Cannot continue.')

    def test_common_operations(self):
        rodsadmin_headers = {'Authorization': 'Bearer ' + self.rodsadmin_bearer_token}
        rodsuser_headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}

        # Create a unixfilesystem resource.
        resc_name = 'test_ufs_common_ops_resc'
        r = requests.post(f'{self.url_base}/resources', headers=rodsadmin_headers, data={
            'op': 'create',
            'name': resc_name,
            'type': 'unixfilesystem',
            'host': self.server_hostname,
            'vault-path': os.path.join('/tmp', f'{resc_name}_vault')
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Create a non-empty data object.
        data_object = os.path.join('/', self.zone_name, 'home', self.rodsuser_username, 'common_ops.txt')
        content = 'hello, this message was written via the iRODS HTTP API!'
        r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
            'op': 'write',
            'lpath': data_object,
            'bytes': content
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Replicate the data object.
        r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
            'op': 'replicate',
            'lpath': data_object,
            'dst-resource': resc_name
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Show there are two replicas.
        coll_name = os.path.dirname(data_object)
        data_name = os.path.basename(data_object)
        r = requests.get(f'{self.url_base}/query', headers=rodsuser_headers, params={
            'op': 'execute_genquery',
            'query': f"select DATA_NAME, RESC_NAME where COLL_NAME = '{coll_name}' and DATA_NAME = '{data_name}'"
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(len(result['rows']), 2)

        # Trim the first replica.
        r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
            'op': 'trim',
            'lpath': data_object,
            'replica-number': 0
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Rename the data object.
        data_object_renamed = f'{data_object}.renamed'
        r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
            'op': 'rename',
            'old-lpath': data_object,
            'new-lpath': data_object_renamed,
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Copy the data object.
        data_object_copied = f'{data_object}.copied'
        r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
            'op': 'copy',
            'src-lpath': data_object_renamed,
            'dst-lpath': data_object_copied,
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Modify permissions on the data object.
        r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
            'op': 'set_permission',
            'lpath': data_object_copied,
            'entity-name': self.rodsadmin_username,
            'permission': 'read_object'
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Show the permissions were updated.
        r = requests.get(self.url_endpoint, headers=rodsuser_headers, params={'op': 'stat', 'lpath': data_object_copied})
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertIn({
            'name': self.rodsadmin_username,
            'zone': self.zone_name,
            'type': 'rodsadmin',
            'perm': 'read_object'
        }, result['permissions'])

        # Remove the data objects.
        for data_object in [data_object_renamed, data_object_copied]:
            with self.subTest(f'Removing [{data_object}]'):
                r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
                    'op': 'remove',
                    'lpath': data_object,
                    'catalog-only': 0,
                    'no-trash': 1
                })
                self.logger.debug(r.content)
                self.assertEqual(r.status_code, 200)
                self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Remove the resource.
        r = requests.post(f'{self.url_base}/resources', headers=rodsadmin_headers, data={'op': 'remove', 'name': resc_name})
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

    def test_copy_operation_honors_resource_parameters(self):
        rodsadmin_headers = {'Authorization': f'Bearer {self.rodsadmin_bearer_token}'}
        rodsuser_headers = {'Authorization': f'Bearer {self.rodsuser_bearer_token}'}

        data_object = f'/{self.zone_name}/home/{self.rodsuser_username}/copy_op_honors_resc_params.txt'
        data_object_copied = f'{data_object}.copied'
        resc_name = 'ufs_copy_resc'

        try:
            # Create an empty data object.
            r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
                'op': 'touch',
                'lpath': data_object
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Show attempting to copy a replica either from or to an invalid resource
            # results in an error.
            for resc_property in ['src-resource', 'dst-resource']:
                with self.subTest(f'Using non-existent resource for [{resc_property}]'):
                    r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
                        'op': 'copy',
                        'src-lpath': data_object,
                        'dst-lpath': f'{data_object}.copied',
                        resc_property: 'does_not_exist'
                    })
                    self.logger.debug(r.content)
                    self.assertEqual(r.status_code, 200)
                    self.assertEqual(r.json()['irods_response']['status_code'], irods_error_codes.SYS_RESC_DOES_NOT_EXIST)

            # Create a unixfilesystem resource.
            r = requests.post(f'{self.url_base}/resources', headers=rodsadmin_headers, data={
                'op': 'create',
                'name': resc_name,
                'type': 'unixfilesystem',
                'host': self.server_hostname,
                'vault-path': f'/tmp/{resc_name}_vault'
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Show copying a data object using valid resources works as expected.
            r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
                'op': 'copy',

                'src-lpath': data_object,
                'src-resource': 'demoResc',

                'dst-lpath': data_object_copied,
                'dst-resource': resc_name
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

        finally:
            # Remove the data objects.
            for lpath in [data_object, data_object_copied]:
                r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
                    'op': 'remove',
                    'lpath': lpath,
                    'catalog-only': 0,
                    'no-trash': 1
                })
                self.logger.debug(r.content)

            # Remove the resource.
            r = requests.post(f'{self.url_base}/resources', headers=rodsadmin_headers, data={
                'op': 'remove',
                'name': resc_name
            })
            self.logger.debug(r.content)

    def test_copy_operation_supports_overwriting_existing_data_objects(self):
        rodsuser_headers = {'Authorization': f'Bearer {self.rodsuser_bearer_token}'}
        data_object_a = f'/{self.zone_name}/home/{self.rodsuser_username}/copy_op_overwrite_param.txt.a'
        data_object_b = f'/{self.zone_name}/home/{self.rodsuser_username}/copy_op_overwrite_param.txt.b'

        try:
            # Create a non-empty data object.
            r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
                'op': 'write',
                'lpath': data_object_a,
                'bytes': 'some data'
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Create a second data object containing different information.
            r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
                'op': 'write',
                'lpath': data_object_b,
                'bytes': 'different data'
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Calculate checksums for each data object to show they are different.
            r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
                'op': 'calculate_checksum',
                'lpath': data_object_a
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            self.assertEqual(result['checksum'], 'sha2:EweZDmulyhRes16ZGCqb7EZTG8VN32VqYCx4D6AkDe4=')
            data_object_a_checksum = result['checksum']

            r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
                'op': 'calculate_checksum',
                'lpath': data_object_b
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            self.assertEqual(result['checksum'], 'sha2:YIoGizPRi+g4vLB77QHjVSHTCED6JNsJGS5nv9GG5iE=')
            data_object_b_checksum = result['checksum']

            self.assertNotEqual(data_object_a_checksum, data_object_b_checksum)

            # Show attempting to copy over an existing data object isn't allowed
            # without the "overwrite" parameter.
            r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
                'op': 'copy',
                'src-lpath': data_object_a,
                'dst-lpath': data_object_b
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], irods_error_codes.OVERWRITE_WITHOUT_FORCE_FLAG)

            # Show copying over an existing data object is possible with the
            # "overwrite" parameter.
            r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
                'op': 'copy',
                'src-lpath': data_object_a,
                'dst-lpath': data_object_b,
                'overwrite': 1
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Show the data objects are now identical.
            expected_checksum = 'sha2:EweZDmulyhRes16ZGCqb7EZTG8VN32VqYCx4D6AkDe4='

            r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
                'op': 'calculate_checksum',
                'lpath': data_object_a,
                'force': 1
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            self.assertEqual(result['checksum'], expected_checksum)

            r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
                'op': 'calculate_checksum',
                'lpath': data_object_b,
                'force': 1
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            self.assertEqual(result['checksum'], expected_checksum)

        finally:
            # Remove the data objects.
            for data_object in [data_object_a, data_object_b]:
                r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
                    'op': 'remove',
                    'lpath': data_object,
                    'catalog-only': 0,
                    'no-trash': 1
                })
                self.logger.debug(r.content)

    def test_copy_operation_returns_an_error_when_destination_path_is_a_collection(self):
        rodsuser_headers = {'Authorization': f'Bearer {self.rodsuser_bearer_token}'}
        data_object = f'/{self.zone_name}/home/{self.rodsuser_username}/copy_op_invalid_dst.txt'
        collection = f'/{self.zone_name}/home/{self.rodsuser_username}/copy_col.d'

        try:
            # Create an empty data object.
            r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
                'op': 'touch',
                'lpath': data_object
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Show attempting to copy a data object to a collection results in an error.
            r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
                'op': 'copy',
                'src-lpath': data_object,
                'dst-lpath': f'/{self.zone_name}/home/public'
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            # CAT_NO_ACCESS_PERMISSION is returned because the rodsuser does not have permission
            # to read the parent collection of "public".
            self.assertEqual(r.json()['irods_response']['status_code'], irods_error_codes.CAT_NO_ACCESS_PERMISSION)

            # Now, try again using the overwrite parameter.
            r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
                'op': 'copy',
                'src-lpath': data_object,
                'dst-lpath': f'/{self.zone_name}/home/public',
                'overwrite': 1
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            # CAT_NO_ACCESS_PERMISSION is returned because the rodsuser does not have permission
            # to read the parent collection of "public".
            self.assertEqual(r.json()['irods_response']['status_code'], irods_error_codes.CAT_NO_ACCESS_PERMISSION)

            # Create a collection.
            r = requests.post(f'{self.url_base}/collections', headers=rodsuser_headers, data={
                'op': 'create',
                'lpath': collection
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            self.assertTrue(result['created'])

            # Show attempting to copy a data object to a collection results in an error.
            r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
                'op': 'copy',
                'src-lpath': data_object,
                'dst-lpath': collection
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], irods_error_codes.CAT_NAME_EXISTS_AS_COLLECTION)

            # Now, try again using the overwrite parameter.
            r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
                'op': 'copy',
                'src-lpath': data_object,
                'dst-lpath': collection,
                'overwrite': 1
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], irods_error_codes.CAT_NAME_EXISTS_AS_COLLECTION)

        finally:
            # Remove the collection.
            r = requests.post(f'{self.url_base}/collections', headers=rodsuser_headers, data={
                'op': 'remove',
                'lpath': collection
            })
            self.logger.debug(r.content)

            # Remove the data object.
            r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
                'op': 'remove',
                'lpath': data_object,
                'catalog-only': 0,
                'no-trash': 1
            })
            self.logger.debug(r.content)

    def test_copying_non_existent_data_object_results_in_an_error(self):
        r = requests.post(self.url_endpoint, headers={'Authorization': f'Bearer {self.rodsuser_bearer_token}'}, data={
            'op': 'copy',
            'src-lpath': '/does_not_exist',
            'dst-lpath': '/ignored'
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 400)
        self.assertEqual(r.json()['irods_response']['status_code'], irods_error_codes.NOT_A_DATA_OBJECT)

    def test_calculating_and_verifying_checksums(self):
        rodsadmin_headers = {'Authorization': 'Bearer ' + self.rodsadmin_bearer_token}
        rodsuser_headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}

        resc_name = 'test_ufs_checksums_resc'
        data_object = os.path.join('/', self.zone_name, 'home', self.rodsuser_username, 'checksums.txt')

        try:
            # Create a unixfilesystem resource.
            r = requests.post(f'{self.url_base}/resources', headers=rodsadmin_headers, data={
                'op': 'create',
                'name': resc_name,
                'type': 'unixfilesystem',
                'host': self.server_hostname,
                'vault-path': os.path.join('/tmp', f'{resc_name}_vault')
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Create a non-empty data object.
            r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
                'op': 'write',
                'lpath': data_object,
                'bytes': 'hello, this message was written via the iRODS HTTP API!'
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Replicate the data object.
            r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
                'op': 'replicate',
                'lpath': data_object,
                'dst-resource': resc_name
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Show there are two replicas.
            coll_name = os.path.dirname(data_object)
            data_name = os.path.basename(data_object)
            r = requests.get(f'{self.url_base}/query', headers=rodsuser_headers, params={
                'op': 'execute_genquery',
                'query': f"select DATA_NAME, RESC_NAME where COLL_NAME = '{coll_name}' and DATA_NAME = '{data_name}'"
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            self.assertEqual(len(result['rows']), 2)

            # Calculate a checksum for first replica.
            r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
                'op': 'calculate_checksum',
                'lpath': data_object,
                'replica-number': 0
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            self.assertEqual(result['checksum'], 'sha2:1SgRcbKcy3+4fjwMvf7xQNG5OZmiYzBVbNuMIgiWbBE=')

            # Verify checksum information across all replicas.
            r = requests.get(self.url_endpoint, headers=rodsuser_headers, params={
                'op': 'verify_checksum',
                'lpath': data_object
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], irods_error_codes.CHECK_VERIFICATION_RESULTS)
            self.assertEqual(result['results'][0]['error_code'], irods_error_codes.CAT_NO_CHECKSUM_FOR_REPLICA)
            self.assertEqual(result['results'][0]['message'], 'WARNING: No checksum available for replica [1].')
            self.assertEqual(result['results'][0]['severity'], 'warning')

        finally:
            # Remove the data objects.
            r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
                'op': 'remove',
                'lpath': data_object,
                'catalog-only': 0,
                'no-trash': 1
            })
            self.logger.debug(r.content)

            # Remove the resource.
            r = requests.post(f'{self.url_base}/resources', headers=rodsadmin_headers, data={
                'op': 'remove',
                'name': resc_name
            })
            self.logger.debug(r.content)

    def test_calculate_checksum_operation_handles_non_existent_data_objects_gracefully(self):
        r = requests.post(self.url_endpoint, headers={'Authorization': 'Bearer ' + self.rodsuser_bearer_token}, data={
            'op': 'calculate_checksum',
            'lpath': '/tempZone/does/not/exist.txt'
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], irods_error_codes.CAT_NO_ROWS_FOUND)

    def test_verify_checksum_operation_handles_non_existent_data_objects_gracefully(self):
        r = requests.get(self.url_endpoint, headers={'Authorization': 'Bearer ' + self.rodsuser_bearer_token}, params={
            'op': 'verify_checksum',
            'lpath': '/tempZone/does/not/exist.txt'
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], irods_error_codes.CAT_NO_ROWS_FOUND)

    def test_registering_a_new_data_object(self):
        rodsadmin_headers = {'Authorization': 'Bearer ' + self.rodsadmin_bearer_token}

        # The name of the physical file to create and register.
        filename = 'newly_registered_file.txt'
        physical_path = f'/tmp/{filename}'

        # The logical path of the data object.
        data_object = f'/{self.zone_name}/home/{self.rodsadmin_username}/{filename}'

        # The name of the resource to register the replica under.
        resource = 'demoResc'

        try:
            # Create a non-empty local file.
            content = 'data'
            with open(physical_path, 'w') as f:
                f.write(content)

            # Show the data object we want to create via registration does not exist.
            r = requests.get(self.url_endpoint, headers=rodsadmin_headers, params={
                'op': 'stat',
                'lpath': data_object
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], irods_error_codes.NOT_A_DATA_OBJECT)

            # Register the local file into the catalog as a new data object.
            # We know we're registering a new data object because the "as-additional-replica"
            # parameter isn't set to 1.
            r = requests.post(self.url_endpoint, headers=rodsadmin_headers, data={
                'op': 'register',
                'lpath': data_object,
                'ppath': physical_path,
                'resource': resource,
                'data-size': len(content)
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Show a new data object exists with the expected replica information.
            r = requests.get(f'{self.url_base}/query', headers=rodsadmin_headers, params={
                'op': 'execute_genquery',
                'query': f"select COLL_NAME, DATA_NAME, DATA_PATH, RESC_NAME where COLL_NAME = '{os.path.dirname(data_object)}' and DATA_NAME = '{filename}'"
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            self.assertEqual(len(result['rows']), 1)
            self.assertEqual(result['rows'][0][0], os.path.dirname(data_object))
            self.assertEqual(result['rows'][0][1], filename)
            self.assertEqual(result['rows'][0][2], physical_path)
            self.assertEqual(result['rows'][0][3], resource)

        finally:
            # Unregister the data object.
            r = requests.post(self.url_endpoint, headers=rodsadmin_headers, data={
                'op': 'remove',
                'lpath': data_object,
                'catalog-only': 1
            })
            self.logger.debug(r.content)

    def test_registering_an_additional_replica_for_an_existing_data_object(self):
        rodsadmin_headers = {'Authorization': 'Bearer ' + self.rodsadmin_bearer_token}

        # The name of the resource we'll use to register an additional replica under.
        other_resource = 'test_registration_resc'

        # The basename and logical path of the data object.
        filename = 'test_registering_replica.txt'
        data_object = f'/{self.zone_name}/home/{self.rodsadmin_username}/{filename}'

        # The name of the physical file to create and register as an additional replica.
        physical_path = f'/tmp/{filename}'

        try:
            # Create a non-empty data object.
            content = 'hello, this message was written via the iRODS HTTP API!'
            r = requests.post(self.url_endpoint, headers=rodsadmin_headers, data={
                'op': 'write',
                'lpath': data_object,
                'bytes': content
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Create a new unixfilesystem resource.
            r = requests.post(f'{self.url_base}/resources', headers=rodsadmin_headers, data={
                'op': 'create',
                'name': other_resource,
                'type': 'unixfilesystem',
                'host': self.server_hostname,
                'vault-path': f'/tmp/{other_resource}_vault'
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Create a local file that will serve as the additional replica.
            # To avoid making replica 0 stale, we must write the same data to this file.
            with open(physical_path, 'w') as f:
                f.write(content)

            # Register the local file into the catalog as an additional replica of the data object.
            r = requests.post(self.url_endpoint, headers=rodsadmin_headers, data={
                'op': 'register',
                'lpath': data_object,
                'ppath': physical_path,
                'resource': other_resource,
                'data-size': len(content),
                'as-additional-replica': 1
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Show replica 0 exists with the expected replica information.
            # Replica 0 will be stale due to the previous registration.
            r = requests.get(f'{self.url_base}/query', headers=rodsadmin_headers, params={
                'op': 'execute_genquery',
                'query': 'select DATA_PATH, DATA_REPL_STATUS ' +
                         f"where COLL_NAME = '{os.path.dirname(data_object)}' and DATA_NAME = '{filename}' and DATA_REPL_NUM = '0'"
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            self.assertEqual(len(result['rows']), 1)
            self.assertNotEqual(result['rows'][0][0], physical_path)
            self.assertEqual(result['rows'][0][1], '0')

            # Show a new replica exists with the expected replica information.
            # Replica 1 must be a good replica since it is the latest replica in the system.
            r = requests.get(f'{self.url_base}/query', headers=rodsadmin_headers, params={
                'op': 'execute_genquery',
                'query': 'select DATA_PATH, RESC_NAME, DATA_REPL_STATUS ' +
                         f"where COLL_NAME = '{os.path.dirname(data_object)}' and DATA_NAME = '{filename}' and DATA_REPL_NUM = '1'"
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            self.assertEqual(len(result['rows']), 1)
            self.assertEqual(result['rows'][0][0], physical_path)
            self.assertEqual(result['rows'][0][1], other_resource)
            self.assertEqual(result['rows'][0][2], '1')

            # Change the replica status of replica 0 so that the replica is good.
            # This is required so the trim operation succeeds (the trim API is not allowed
            # to remove the last good replica).
            r = requests.post(self.url_endpoint, headers=rodsadmin_headers, data={
                'op': 'modify_replica',
                'lpath': data_object,
                'replica-number': 0,
                'new-data-replica-status': 1
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Unregister replica 1.
            r = requests.post(self.url_endpoint, headers=rodsadmin_headers, data={
                'op': 'trim',
                'lpath': data_object,
                'replica-number': 1,
                'catalog-only': 1
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Show replica 1 no longer exists.
            r = requests.get(f'{self.url_base}/query', headers=rodsadmin_headers, params={
                'op': 'execute_genquery',
                'query': 'select COLL_NAME, DATA_NAME ' +
                         f"where COLL_NAME = '{os.path.dirname(data_object)}' and DATA_NAME = '{filename}' and DATA_REPL_NUM = '1'"
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            self.assertEqual(len(result['rows']), 0)

        finally:
            # Remove the data object.
            r = requests.post(self.url_endpoint, headers=rodsadmin_headers, data={
                'op': 'remove',
                'lpath': data_object,
                'catalog-only': 0,
                'no-trash': 1
            })
            self.logger.debug(r.content)

            # Remove the unixfilesystem resource.
            r = requests.post(f'{self.url_base}/resources', headers=rodsadmin_headers, data={
                'op': 'remove',
                'name': other_resource
            })
            self.logger.debug(r.content)

            # Remove the file so the test is idempotent.
            os.remove(physical_path)

    def test_touch_operation_updates_mtime(self):
        rodsuser_headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}
        data_object = os.path.join('/', self.zone_name, 'home', self.rodsuser_username, 'test_object')

        try:
            # Create a new data object.
            r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
                'op': 'touch',
                'lpath': data_object
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Get the mtime of the data object.
            coll_name = os.path.dirname(data_object)
            data_name = os.path.basename(data_object)
            r = requests.get(f'{self.url_base}/query', headers=rodsuser_headers, params={
                'op': 'execute_genquery',
                'query': f"select DATA_MODIFY_TIME where COLL_NAME = '{coll_name}' and DATA_NAME = '{data_name}'"
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            self.assertEqual(len(result['rows']), 1)
            original_mtime = int(result['rows'][0][0])
            self.assertGreater(original_mtime, 0)

            # Sleep for a short period of time to guarantee a difference in the mtime.
            time.sleep(2)

            # Update the mtime by calling touch.
            r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
                'op': 'touch',
                'lpath': data_object
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Show the mtime has been updated.
            collection = os.path.join('/', self.zone_name, 'home', self.rodsuser_username)
            r = requests.get(f'{self.url_base}/query', headers=rodsuser_headers, params={
                'op': 'execute_genquery',
                'query': f"select DATA_MODIFY_TIME where COLL_NAME = '{coll_name}' and DATA_NAME = '{data_name}'"
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            self.assertEqual(len(result['rows']), 1)
            new_mtime = int(result['rows'][0][0])
            self.assertGreater(new_mtime, original_mtime)

        finally:
            # Remove the data object.
            r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
                'op': 'remove',
                'lpath': data_object,
                'catalog-only': 0,
                'no-trash': 1
            })
            self.logger.debug(r.content)

    def test_touch_operation_reports_error_when_given_a_path_to_a_collection(self):
        rodsuser_headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}
        r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
            'op': 'touch',
            'lpath': f'/{self.zone_name}/home/{self.rodsuser_username}'
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 400)
        self.assertEqual(r.json()['irods_response']['status_code'], irods_error_codes.NOT_A_DATA_OBJECT)

    def multipart_form_data_upload(self, **args):
        boundary = '------testing_http_api------'

        body = ''

        for fname, fvalue in args['fields'].items():
            body += f'--{boundary}\r\n'
            body += f'Content-Disposition: form-data; name={fname}\r\n\r\n'
            body += f'{fvalue}\r\n'

        body += f'--{boundary}\r\n'
        body += f'Content-Disposition: form-data; name=bytes\r\n'
        body += f'Content-Type: application/octet-stream\r\n'
        body += f"Content-Length: {len(args['bytes'])}\r\n"
        body += '\r\n'
        body += f"{args['bytes']}\r\n"

        body += f'--{boundary}--\r\n'

        self.logger.debug(f'body = {body}')

        conn = http.client.HTTPConnection(config.test_config['host'], config.test_config['port'])
        conn.request('POST', config.test_config['url_base'] + '/data-objects', bytes(body, 'utf-8'), {
            'Authorization': f"Bearer {args['bearer_token']}",
            'Content-Type': f'multipart/form-data; boundary={boundary}'
        })

        response = conn.getresponse()
        result = json.loads(response.read().decode('utf-8'))
        self.logger.debug(result)
        conn.close()

        self.assertEqual(response.status, 200)

        return result

    def test_parallel_writes(self):
        headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}

        # Tell the server we're about to do a parallel write.
        data_object = os.path.join('/', self.zone_name, 'home', self.rodsuser_username, 'parallel_write.txt')
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'parallel_write_init',
            'lpath': data_object,
            'stream-count': 3
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        parallel_write_handle = result['parallel_write_handle']

        # Write to the data object using the parallel write handle.
        futures = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as executor:
            for e in enumerate(['A', 'B', 'C']):
                count = 10
                futures.append(executor.submit(self.multipart_form_data_upload, **{
                    'bearer_token': self.rodsuser_bearer_token,
                    'fields': {
                        'op': 'write',
                        'parallel-write-handle': parallel_write_handle,
                        'offset': e[0] * count,
                        'stream-index': e[0]
                    },
                    'bytes': e[1] * count
                }))

            for f in concurrent.futures.as_completed(futures):
                result = f.result()
                self.logger.debug(result)
                self.assertEqual(result['irods_response']['status_code'], 0)

        # End the parallel write.
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'parallel_write_shutdown',
            'parallel-write-handle': parallel_write_handle
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Read the contents of the data object and show it contains exactly what we expect.
        r = requests.get(self.url_endpoint, headers=headers, params={
            'op': 'read',
            'lpath': data_object,
            'count': 30
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.content.decode('utf-8'), 'A' * 10 + 'B' * 10 + 'C' * 10)

        # Remove the data object.
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'remove',
            'lpath': data_object,
            'catalog-only': 0,
            'no-trash': 1
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

    def test_parallel_writes_with_data_exceeding_internal_write_threshold(self):
        # This test assumes the HTTP API is configured to use a value smaller than
        # 96kb for "/irods_client/max_number_of_bytes_per_write_operation". This is
        # important because values greater than or equal to 96kb will result in the
        # background thread pool only being used one time. This test is designed to
        # force the HTTP API to schedule multiple writes across the thread pool.
        #
        # NOTE: This test DOES NOT and CANNOT confirm the background thread pool is
        # being used because it does not have visibility into the server's internal
        # mechanisms. Our only means of verification is by careful review of the log
        # messages. The server is expected to output messages about multiple writes
        # to the iRODS server. The thread ID should change from one write operation to
        # the next.

        headers = {'Authorization': f'Bearer {self.rodsuser_bearer_token}'}
        data_object = f'/{self.zone_name}/home/{self.rodsuser_username}/incremental_writes_0.txt'

        # Generate 96kb string.
        # This is the data we'll upload to the HTTP API.
        data96kb = ('A' * 32768) + ('B' * 32768) + ('C' * 32768)

        # Calculate a checksum for data.
        # The checksum will be used to confirm the data was uploaded correctly.
        checksum = base64.b64encode(hashlib.sha256(data96kb.encode('utf-8')).digest()).decode('utf-8')
        self.logger.debug(f'checksum = [{checksum}]')

        # Indicates whether the parallel_write_shutdown operation needs to be called.
        # This is used for clean up when errors occur.
        invoke_parallel_write_shutdown = False

        try:
            # Tell the server we're about to do a parallel write.
            stream_count = 3
            r = requests.post(self.url_endpoint, headers=headers, data={
                'op': 'parallel_write_init',
                'lpath': data_object,
                'stream-count': stream_count
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            parallel_write_handle = result['parallel_write_handle']

            # Make sure the parallel_write_shutdown operation is invoked now that we
            # have a parallel-write handle.
            invoke_parallel_write_shutdown = True

            # Write to the data object using the parallel write handle.
            futures = []
            with concurrent.futures.ThreadPoolExecutor(max_workers=3) as executor:
                chunk_size = len(data96kb) // stream_count
                self.logger.debug(f'chunk size = [{chunk_size}]')
                for i in range(stream_count):
                    chunk_start = i * chunk_size
                    futures.append(executor.submit(self.multipart_form_data_upload, **{
                        'bearer_token': self.rodsuser_bearer_token,
                        'fields': {
                            'op': 'write',
                            'parallel-write-handle': parallel_write_handle,
                            'offset': chunk_start,
                            'stream-index': i
                        },
                        'bytes': data96kb[chunk_start : chunk_start + chunk_size]
                    }))

                for f in concurrent.futures.as_completed(futures):
                    result = f.result()
                    self.logger.debug(result)
                    self.assertEqual(result['irods_response']['status_code'], 0)

            # End the parallel write.
            r = requests.post(self.url_endpoint, headers=headers, data={
                'op': 'parallel_write_shutdown',
                'parallel-write-handle': parallel_write_handle
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # The data object was closed successfully.
            # Do not invoke the parallel_write_shutdown operation in the finally block.
            invoke_parallel_write_shutdown = False

            # Show the data was written to the iRODS server safely.
            r = requests.post(self.url_endpoint, headers=headers, data={
                'op': 'calculate_checksum',
                'lpath': data_object
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            self.assertEqual(result['checksum'][5:], checksum)

        finally:
            # End the parallel write in case something failed.
            # This avoids leaking of parallel-write resources in the HTTP API server.
            if invoke_parallel_write_shutdown:
                r = requests.post(self.url_endpoint, headers=headers, data={
                    'op': 'parallel_write_shutdown',
                    'parallel-write-handle': parallel_write_handle
                })
                self.logger.debug(r.content)

            # Remove the data object.
            r = requests.post(self.url_endpoint, headers=headers, data={
                'op': 'remove',
                'lpath': data_object,
                'catalog-only': 0,
                'no-trash': 1
            })
            self.logger.debug(r.content)

    def test_non_parallel_writes_with_data_exceeding_internal_write_threshold(self):
        # This test assumes the HTTP API is configured to use a value smaller than
        # 64kb for "/irods_client/max_number_of_bytes_per_write_operation". This is
        # important because values greater than or equal to 64kb will result in the
        # background thread pool only being used one time. This test is designed to
        # force the HTTP API to schedule multiple writes across the thread pool.
        #
        # NOTE: This test DOES NOT and CANNOT confirm the background thread pool is
        # being used because it does not have visibility into the server's internal
        # mechanisms. Our only means of verification is by careful review of the log
        # messages. The server is expected to output messages about multiple writes
        # to the iRODS server. The thread ID should change from one write operation to
        # the next.

        headers = {'Authorization': f'Bearer {self.rodsuser_bearer_token}'}
        data_object = f'/{self.zone_name}/home/{self.rodsuser_username}/incremental_writes_1.txt'

        # Generate 64kb of random bytes.
        # This is the data we'll upload to the HTTP API.
        data64kb = os.urandom(64 * 1024)

        # Calculate a checksum for data.
        # The checksum will be used to confirm the data was uploaded correctly.
        checksum = base64.b64encode(hashlib.sha256(data64kb).digest()).decode('utf-8')
        self.logger.debug(f'checksum = [{checksum}]')

        try:
            # Create a new data object holding the binary data.
            # Use the "files" parameter to upload the data using multipart/form-data as
            # the encoding. If the "data" parameter was used, the data would be encoded
            # as application/x-www-form-urlencoded which could cause the data to be corrupted.
            r = requests.post(self.url_endpoint, headers=headers, files={
                'op': 'write',
                'lpath': data_object,
                'bytes': data64kb
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Show the data was written to the iRODS server safely.
            r = requests.post(self.url_endpoint, headers=headers, data={
                'op': 'calculate_checksum',
                'lpath': data_object
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            self.assertEqual(result['checksum'][5:], checksum)

        finally:
            # Remove the data object.
            r = requests.post(self.url_endpoint, headers=headers, data={
                'op': 'remove',
                'lpath': data_object,
                'catalog-only': 0,
                'no-trash': 1
            })
            self.logger.debug(r.content)

    def test_writing_to_data_object_using_header_based_form(self):
        headers = {'Authorization': f'Bearer {self.rodsuser_bearer_token}'}
        data_object = f'/{self.zone_name}/home/{self.rodsuser_username}/header_based_write_op1.txt'

        # Generate 512kb of random bytes.
        # This is the data we'll upload to the HTTP API.
        data512kb = os.urandom(512 * 1024)

        # Calculate a checksum for data.
        # The checksum will be used to confirm the data was uploaded correctly.
        checksum = base64.b64encode(hashlib.sha256(data512kb).digest()).decode('utf-8')
        self.logger.debug(f'checksum = [{checksum}]')

        try:
            # Create a new data object holding the binary data.
            r = requests.post(self.url_endpoint, headers={
                'Authorization': headers['Authorization'],
                'Content-Type': 'application/octet-stream',
                'irods-api-request-op': 'write',
                'irods-api-request-lpath': data_object
            }, data=data512kb)
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Show the data was written to the iRODS server safely.
            r = requests.post(self.url_endpoint, headers=headers, data={
                'op': 'calculate_checksum',
                'lpath': data_object
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            self.assertEqual(result['checksum'][5:], checksum)

        finally:
            # Remove the data object.
            r = requests.post(self.url_endpoint, headers=headers, data={
                'op': 'remove',
                'lpath': data_object,
                'catalog-only': 0,
                'no-trash': 1
            })
            self.logger.debug(r.content)

    def execute_write_using_header_based_form(self, **args):
        conn = http.client.HTTPConnection(config.test_config['host'], config.test_config['port'])

        headers = {
            'Authorization': f"Bearer {args['bearer_token']}",
            'Content-Type': 'application/x-www-form-urlencoded',
            'irods-api-request-op': args['fields']['op'],
            'irods-api-request-offset': args['fields']['offset'],
            'irods-api-request-parallel-write-handle': args['fields']['parallel-write-handle'],
            'irods-api-request-stream-index': args['fields']['stream-index']
        }
        conn.request('POST', config.test_config['url_base'] + '/data-objects', bytes(args['bytes'], 'utf-8'), headers)

        response = conn.getresponse()
        result = json.loads(response.read().decode('utf-8'))
        self.logger.debug(result)
        conn.close()

        self.assertEqual(response.status, 200)

        return result

    def test_parallel_writes_using_header_based_form(self):
        headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}

        # Indicates whether the parallel_write_shutdown operation needs to be called.
        # This is used for clean up when errors occur.
        invoke_parallel_write_shutdown = False

        try:
            stream_count = 3

            # Tell the server we're about to do a parallel write.
            data_object = os.path.join('/', self.zone_name, 'home', self.rodsuser_username, 'parallel_write_header_based_0.txt')
            r = requests.post(self.url_endpoint, headers=headers, data={
                'op': 'parallel_write_init',
                'lpath': data_object,
                'stream-count': stream_count
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            parallel_write_handle = result['parallel_write_handle']

            # Make sure the parallel_write_shutdown operation is invoked now that we
            # have a parallel-write handle.
            invoke_parallel_write_shutdown = True

            # Write to the data object using the parallel write handle.
            futures = []
            with concurrent.futures.ThreadPoolExecutor(max_workers=stream_count) as executor:
                for e in enumerate(['A', 'B', 'C']):
                    count = 10
                    futures.append(executor.submit(self.execute_write_using_header_based_form, **{
                        'bearer_token': self.rodsuser_bearer_token,
                        'fields': {
                            'op': 'write',
                            'parallel-write-handle': parallel_write_handle,
                            'offset': e[0] * count,
                            'stream-index': e[0]
                        },
                        'bytes': e[1] * count
                    }))

                for f in concurrent.futures.as_completed(futures):
                    result = f.result()
                    self.logger.debug(result)
                    self.assertEqual(result['irods_response']['status_code'], 0)

            # End the parallel write.
            r = requests.post(self.url_endpoint, headers=headers, data={
                'op': 'parallel_write_shutdown',
                'parallel-write-handle': parallel_write_handle
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Read the contents of the data object and show it contains exactly what we expect.
            r = requests.get(self.url_endpoint, headers=headers, params={
                'op': 'read',
                'lpath': data_object,
                'count': 30
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.content.decode('utf-8'), 'A' * 10 + 'B' * 10 + 'C' * 10)

            # Show the data was written to the iRODS server safely.
            r = requests.post(self.url_endpoint, headers=headers, data={
                'op': 'calculate_checksum',
                'lpath': data_object
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            self.assertEqual(result['checksum'], 'sha2:40ghWJg094XKraNmQ+nnzxCJp0ugME9rfaZsVd/A4Sc=')

        finally:
            # End the parallel write in case something failed.
            # This avoids leaking of parallel-write resources in the HTTP API server.
            if invoke_parallel_write_shutdown:
                r = requests.post(self.url_endpoint, headers=headers, data={
                    'op': 'parallel_write_shutdown',
                    'parallel-write-handle': parallel_write_handle
                })
                self.logger.debug(r.content)

            # Remove the data object.
            r = requests.post(self.url_endpoint, headers=headers, data={
                'op': 'remove',
                'lpath': data_object,
                'catalog-only': 0,
                'no-trash': 1
            })
            self.logger.debug(r.content)

    def test_parallel_writes_using_header_based_form_with_data_exceeding_internal_write_threshold(self):
        # This test assumes the HTTP API is configured to use a value smaller than
        # 96kb for "/irods_client/max_number_of_bytes_per_write_operation". This is
        # important because values greater than or equal to 96kb will result in the
        # background thread pool only being used one time. This test is designed to
        # force the HTTP API to schedule multiple writes across the thread pool.
        #
        # NOTE: This test DOES NOT and CANNOT confirm the background thread pool is
        # being used because it does not have visibility into the server's internal
        # mechanisms. Our only means of verification is by careful review of the log
        # messages. The server is expected to output messages about multiple writes
        # to the iRODS server. The thread ID should change from one write operation to
        # the next.

        headers = {'Authorization': f'Bearer {self.rodsuser_bearer_token}'}
        data_object = f'/{self.zone_name}/home/{self.rodsuser_username}/parallel_writes_header_based_1.txt'

        # Generate 96kb string.
        # This is the data we'll upload to the HTTP API.
        data96kb = ('A' * 32768) + ('B' * 32768) + ('C' * 32768)

        # Indicates whether the parallel_write_shutdown operation needs to be called.
        # This is used for clean up when errors occur.
        invoke_parallel_write_shutdown = False

        try:
            # Tell the server we're about to do a parallel write.
            stream_count = 3
            r = requests.post(self.url_endpoint, headers=headers, data={
                'op': 'parallel_write_init',
                'lpath': data_object,
                'stream-count': stream_count
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            parallel_write_handle = result['parallel_write_handle']

            # Make sure the parallel_write_shutdown operation is invoked now that we
            # have a parallel-write handle.
            invoke_parallel_write_shutdown = True

            # Write to the data object using the parallel write handle.
            futures = []
            with concurrent.futures.ThreadPoolExecutor(max_workers=3) as executor:
                chunk_size = len(data96kb) // stream_count
                self.logger.debug(f'chunk size = [{chunk_size}]')
                for i in range(stream_count):
                    chunk_start = i * chunk_size
                    futures.append(executor.submit(self.execute_write_using_header_based_form, **{
                        'bearer_token': self.rodsuser_bearer_token,
                        'fields': {
                            'op': 'write',
                            'parallel-write-handle': parallel_write_handle,
                            'offset': chunk_start,
                            'stream-index': i
                        },
                        'bytes': data96kb[chunk_start : chunk_start + chunk_size]
                    }))

                for f in concurrent.futures.as_completed(futures):
                    result = f.result()
                    self.logger.debug(result)
                    self.assertEqual(result['irods_response']['status_code'], 0)

            # End the parallel write.
            r = requests.post(self.url_endpoint, headers=headers, data={
                'op': 'parallel_write_shutdown',
                'parallel-write-handle': parallel_write_handle
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # The data object was closed successfully.
            # Do not invoke the parallel_write_shutdown operation in the finally block.
            invoke_parallel_write_shutdown = False

            # Show the data was written to the iRODS server safely.
            r = requests.post(self.url_endpoint, headers=headers, data={
                'op': 'calculate_checksum',
                'lpath': data_object
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            self.assertEqual(result['checksum'], 'sha2:YS0DmQW7u51PaFVYl/iTsHFOctKtP1MZZ8JLnBwCLMI=')

        finally:
            # End the parallel write in case something failed.
            # This avoids leaking of parallel-write resources in the HTTP API server.
            if invoke_parallel_write_shutdown:
                r = requests.post(self.url_endpoint, headers=headers, data={
                    'op': 'parallel_write_shutdown',
                    'parallel-write-handle': parallel_write_handle
                })
                self.logger.debug(r.content)

            # Remove the data object.
            r = requests.post(self.url_endpoint, headers=headers, data={
                'op': 'remove',
                'lpath': data_object,
                'catalog-only': 0,
                'no-trash': 1
            })
            self.logger.debug(r.content)

    def test_non_parallel_writes_using_header_based_form_with_data_exceeding_internal_write_threshold(self):
        # This test assumes the HTTP API is configured to use a value smaller than
        # 64kb for "/irods_client/max_number_of_bytes_per_write_operation". This is
        # important because values greater than or equal to 64kb will result in the
        # background thread pool only being used one time. This test is designed to
        # force the HTTP API to schedule multiple writes across the thread pool.
        #
        # NOTE: This test DOES NOT and CANNOT confirm the background thread pool is
        # being used because it does not have visibility into the server's internal
        # mechanisms. Our only means of verification is by careful review of the log
        # messages. The server is expected to output messages about multiple writes
        # to the iRODS server. The thread ID should change from one write operation to
        # the next.

        headers = {'Authorization': f'Bearer {self.rodsuser_bearer_token}'}
        data_object = f'/{self.zone_name}/home/{self.rodsuser_username}/parallel_writes_header_based_2.txt'

        # Generate 64kb of random bytes.
        # This is the data we'll upload to the HTTP API.
        data64kb = os.urandom(64 * 1024)

        # Calculate a checksum for data.
        # The checksum will be used to confirm the data was uploaded correctly.
        checksum = base64.b64encode(hashlib.sha256(data64kb).digest()).decode('utf-8')
        self.logger.debug(f'checksum = [{checksum}]')

        try:
            # Create a new data object holding the binary data.
            r = requests.post(self.url_endpoint, headers={
                'Authorization': headers['Authorization'],
                'irods-api-request-op': 'write',
                'irods-api-request-lpath': data_object
            }, data=data64kb)
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Show the data was written to the iRODS server safely.
            r = requests.post(self.url_endpoint, headers=headers, data={
                'op': 'calculate_checksum',
                'lpath': data_object
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            self.assertEqual(result['checksum'][5:], checksum)

        finally:
            # Remove the data object.
            r = requests.post(self.url_endpoint, headers=headers, data={
                'op': 'remove',
                'lpath': data_object,
                'catalog-only': 0,
                'no-trash': 1
            })
            self.logger.debug(r.content)

    def test_modifying_metadata_atomically(self):
        headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}

        # Create a data object.
        data_object = os.path.join('/', self.zone_name, 'home', self.rodsuser_username, 'for_atomic_metadata.txt')
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'touch',
            'lpath': data_object
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Add metadata to the home data object.
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'modify_metadata',
            'lpath': data_object,
            'operations': json.dumps([
                {
                    'operation': 'add',
                    'attribute': 'a1',
                    'value': 'v1',
                    'units': 'u1'
                }
            ])
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Show the metadata exists on the data object.
        r = requests.get(f'{self.url_base}/query', headers=headers, params={
            'op': 'execute_genquery',
            'query': "select COLL_NAME, DATA_NAME where META_DATA_ATTR_NAME = 'a1' and META_DATA_ATTR_VALUE = 'v1' and META_DATA_ATTR_UNITS = 'u1'"
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(result['rows'][0][0], os.path.dirname(data_object))
        self.assertEqual(result['rows'][0][1], os.path.basename(data_object))

        # Remove the metadata from the data object.
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'modify_metadata',
            'lpath': data_object,
            'operations': json.dumps([
                {
                    'operation': 'remove',
                    'attribute': 'a1',
                    'value': 'v1',
                    'units': 'u1'
                }
            ])
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Show the metadata no longer exists on the data object.
        r = requests.get(f'{self.url_base}/query', headers=headers, params={
            'op': 'execute_genquery',
            'query': "select COLL_NAME, DATA_NAME where META_DATA_ATTR_NAME = 'a1' and META_DATA_ATTR_VALUE = 'v1' and META_DATA_ATTR_UNITS = 'u1'"
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(len(result['rows']), 0)

        # Remove the data object.
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'remove',
            'lpath': data_object,
            'catalog-only': 0,
            'no-trash': 1
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

    def test_modifying_permissions_atomically(self):
        headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}

        # Create a data object.
        data_object = os.path.join('/', self.zone_name, 'home', self.rodsuser_username, 'for_atomic_acls.txt')
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'touch',
            'lpath': data_object
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Give the rodsadmin read permission on the data object.
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'modify_permissions',
            'lpath': data_object,
            'operations': json.dumps([
                {
                    'entity_name': self.rodsadmin_username,
                    'acl': 'read'
                }
            ])
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Show the rodsadmin now has permission to read the data object.
        r = requests.get(self.url_endpoint, headers=headers, params={
            'op': 'stat',
            'lpath': data_object
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(len(result['permissions']), 2)
        #self.assertEqual(result['permissions'][0]['name'], self.rodsuser_username)
        #self.assertEqual(result['permissions'][0]['zone'], self.zone_name)
        #self.assertEqual(result['permissions'][0]['type'], 'rodsuser')
        #self.assertEqual(result['permissions'][0]['perm'], 'own')

        # Remove rodsadmin's permission on the data object.
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'modify_permissions',
            'lpath': data_object,
            'operations': json.dumps([
                {
                    'entity_name': self.rodsadmin_username,
                    'acl': 'null'
                }
            ])
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Show the permissions have been removed.
        r = requests.get(self.url_endpoint, headers=headers, params={
            'op': 'stat',
            'lpath': data_object
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(len(result['permissions']), 1)
        #self.assertEqual(result['permissions'][0]['name'], self.rodsuser_username)
        #self.assertEqual(result['permissions'][0]['zone'], self.zone_name)
        #self.assertEqual(result['permissions'][0]['type'], 'rodsuser')
        #self.assertEqual(result['permissions'][0]['perm'], 'own')

        # Remove the data object.
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'remove',
            'lpath': data_object,
            'catalog-only': 0,
            'no-trash': 1
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

    def test_modifying_replica_properties(self):
        headers = {'Authorization': 'Bearer ' + self.rodsadmin_bearer_token}

        # Create a data object.
        data_object = os.path.join('/', self.zone_name, 'home', self.rodsadmin_username, 'modrepl.txt')
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'touch',
            'lpath': data_object
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Show the replica is currently marked as good and has a size of 0.
        r = requests.get(f'{self.url_base}/query', headers=headers, params={
            'op': 'execute_genquery',
            'query': f"select DATA_REPL_STATUS, DATA_SIZE where COLL_NAME = '{os.path.dirname(data_object)}' and DATA_NAME = '{os.path.basename(data_object)}'"
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(result['rows'][0][0], '1')
        self.assertEqual(result['rows'][0][1], '0')

        # Change the replica's status and data size using the modify_replica operation.
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'modify_replica',
            'lpath': data_object,
            'replica-number': 0,
            'new-data-replica-status': 0,
            'new-data-size': 15
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Show the replica's status and size has changed in the catalog.
        r = requests.get(f'{self.url_base}/query', headers=headers, params={
            'op': 'execute_genquery',
            'query': f"select DATA_REPL_STATUS, DATA_SIZE where COLL_NAME = '{os.path.dirname(data_object)}' and DATA_NAME = '{os.path.basename(data_object)}'"
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(result['rows'][0][0], '0')
        self.assertEqual(result['rows'][0][1], '15')

        # Remove the data object.
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'remove',
            'lpath': data_object,
            'catalog-only': 0,
            'no-trash': 1
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

    def test_remove_operation_returns_an_error_when_catalog_only_is_1_and_no_trash_is_1(self):
        r = requests.post(self.url_endpoint, headers={'Authorization': 'Bearer ' + self.rodsadmin_bearer_token}, data={
            'op': 'remove',
            'lpath': 'ignored',
            'catalog-only': 1,
            'no-trash': 1
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 400)

    def test_remove_operation_supports_unregistering_all_replicas(self):
        rodsadmin_headers = {'Authorization': 'Bearer ' + self.rodsadmin_bearer_token}
        data_object = f'/{self.zone_name}/home/{self.rodsadmin_username}/remove_op_unreg.txt'
        resc_name = 'remove_op_unreg_resc'

        try:
            # Create a non-empty data object.
            self.logger.debug('Creating data object')
            r = requests.post(self.url_endpoint, headers=rodsadmin_headers, data={
                'op': 'write',
                'lpath': data_object,
                'bytes': 'hello, this message was written via the iRODS HTTP API!'
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Create a unixfilesystem resource.
            self.logger.debug('Creating resource')
            r = requests.post(f'{self.url_base}/resources', headers=rodsadmin_headers, data={
                'op': 'create',
                'name': resc_name,
                'type': 'unixfilesystem',
                'host': self.server_hostname,
                'vault-path': os.path.join('/tmp', f'{resc_name}_vault')
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Replicate the data object.
            self.logger.debug('Replicating replica to new resource')
            r = requests.post(self.url_endpoint, headers=rodsadmin_headers, data={
                'op': 'replicate',
                'lpath': data_object,
                'dst-resource': resc_name
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Get the physical path of the replicas.
            # We include the replica number so the replicas are listed in the order in which
            # they were created.
            coll_name = os.path.dirname(data_object)
            data_name = os.path.basename(data_object)
            r = requests.get(f'{self.url_base}/query', headers=rodsadmin_headers, params={
                'op': 'execute_genquery',
                'parser': 'genquery1',
                'query': f"select DATA_REPL_NUM, DATA_PATH where COLL_NAME = '{coll_name}' and DATA_NAME = '{data_name}'"
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            self.assertEqual(len(result['rows']), 2)

            replica_paths = result['rows']
            self.logger.debug(f'replica 0 => [{replica_paths[0][1]}]')
            self.logger.debug(f'replica 1 => [{replica_paths[1][1]}]')

            # Unregister all replicas.
            self.logger.debug(f'Calling remove op to unregister all replicas')
            r = requests.post(self.url_endpoint, headers=rodsadmin_headers, data={
                'op': 'remove',
                'lpath': data_object,
                'catalog-only': 1
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Because the user running the tests may not have permission to view the
            # files in vault, we have to register them and show iRODS can read them.
            # This proves the files were left in-place.
            r = requests.post(self.url_endpoint, headers=rodsadmin_headers, data={
                'op': 'register',
                'lpath': data_object,
                'ppath': replica_paths[0][1],
                'resource': 'demoResc'
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            r = requests.post(self.url_endpoint, headers=rodsadmin_headers, data={
                'op': 'register',
                'lpath': data_object,
                'ppath': replica_paths[1][1],
                'resource': resc_name,
                'as-additional-replica': 1
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Calculate a checksum for each replica.
            # This serves as proof that things are working as expected because
            # calculating a checksum requires reading the physical file.
            for replica_number in range(2):
                with self.subTest(f'Calculating checksum for replica [{replica_number}] of data object [{data_object}]'):
                    r = requests.post(self.url_endpoint, headers=rodsadmin_headers, data={
                        'op': 'calculate_checksum',
                        'lpath': data_object,
                        'replica-number': replica_number
                    })
                    self.logger.debug(r.content)
                    self.assertEqual(r.status_code, 200)
                    result = r.json()
                    self.assertEqual(result['irods_response']['status_code'], 0)
                    self.assertEqual(result['checksum'], 'sha2:1SgRcbKcy3+4fjwMvf7xQNG5OZmiYzBVbNuMIgiWbBE=')

        finally:
            # Remove the data object.
            r = requests.post(self.url_endpoint, headers=rodsadmin_headers, data={
                'op': 'remove',
                'lpath': data_object,
                'catalog-only': 0,
                'no-trash': 1
            })
            self.logger.debug(r.content)

            # Remove resource.
            r = requests.post(f'{self.url_base}/resources', headers=rodsadmin_headers, data={
                'op': 'remove',
                'name': resc_name
            })
            self.logger.debug(r.content)

    def test_attempting_to_read_non_existent_data_object_results_in_an_error(self):
        rodsuser_headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}
        r = requests.get(self.url_endpoint, headers=rodsuser_headers, params={
            'op': 'read',
            'lpath': f'/{self.zone_name}/home/{self.rodsuser_username}/does_not_exist'
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 404)

    def test_attempting_to_read_data_object_with_insufficient_permissions_results_in_an_error(self):
        rodsadmin_headers = {'Authorization': 'Bearer ' + self.rodsadmin_bearer_token}
        rodsuser_headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}
        data_object = f'/{self.zone_name}/home/{self.rodsadmin_username}/http_api_invalid_values_for_read_op.txt'

        try:
            # Create a data object as the rodsadmin.
            r = requests.post(self.url_endpoint, headers=rodsadmin_headers, data={
                'op': 'touch',
                'lpath': data_object
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Attempting to read a data object without appropriate permissions will
            # result in an error.
            r = requests.get(self.url_endpoint, headers=rodsuser_headers, params={
                'op': 'read',
                'lpath': data_object
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 404)

        finally:
            # Remove the data object.
            r = requests.post(self.url_endpoint, headers=rodsadmin_headers, data={
                'op': 'remove',
                'lpath': data_object,
                'catalog-only': 0,
                'no-trash': 1
            })
            self.logger.debug(r.content)

    def test_passing_invalid_values_to_offset_and_count_parameters_for_read_operation_results_in_an_error(self):
        rodsuser_headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}
        data_object = f'/{self.zone_name}/home/{self.rodsuser_username}/http_api_invalid_values_for_read_op.txt'

        try:
            # Create a data object.
            r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
                'op': 'touch',
                'lpath': data_object
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Show an invalid value for the "offset" parameter results in an http error.
            r = requests.get(self.url_endpoint, headers=rodsuser_headers, params={
                'op': 'read',
                'lpath': data_object,
                'offset': 'triggers_error'
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 400)

            # Show an invalid value for the "count" parameter results in an http error.
            r = requests.get(self.url_endpoint, headers=rodsuser_headers, params={
                'op': 'read',
                'lpath': data_object,
                'count': 'triggers_error'
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 400)

        finally:
            # Remove the data object.
            r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
                'op': 'remove',
                'lpath': data_object,
                'catalog-only': 0,
                'no-trash': 1
            })
            self.logger.debug(r.content)

    def test_server_reports_error_when_http_method_is_not_supported(self):
        do_test_server_reports_error_when_http_method_is_not_supported(self)

    def test_server_reports_error_when_op_is_not_supported(self):
        do_test_server_reports_error_when_op_is_not_supported(self)

    @unittest.skip('Test needs to be implemented.')
    def test_return_error_on_missing_parameters(self):
        pass

    def test_server_correctly_handles_embedded_CRLFs_in_multipart_form_data(self):
        headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}
        data_object = f'/{self.zone_name}/home/{self.rodsuser_username}/embedded_crlfs.txt'

        try:
            # Create a data object using multipart/form-data encoding.
            # The body of the request will contain an embedded CRLF. The CRLF is what triggers
            # the bug in the original parser. This would cause the data to be truncated.
            # For more information, see issue #273.
            content = 'look, an embedded \r\n CRLF.'
            r = requests.post(self.url_endpoint, headers=headers, files={
                'op': 'write',
                'lpath': data_object,
                'bytes': content
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Show the iRODS server recorded the correct size for the data object.
            r = requests.get(self.url_endpoint, headers=headers, params={
                'op': 'stat',
                'lpath': data_object
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            self.assertEqual(int(result['size']), len(content))

            # Show the data object stored in iRODS contains the expected data.
            r = requests.get(self.url_endpoint, headers=headers, params={
                'op': 'read',
                'lpath': data_object
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.content.decode('utf-8'), content)

        finally:
            # Remove the data object.
            r = requests.post(self.url_endpoint, headers=headers, data={
                'op': 'remove',
                'lpath': data_object,
                'catalog-only': 0,
                'no-trash': 1
            })
            self.logger.debug(r.content)

    def test_server_does_not_segfault_when_parallel_write_init_is_invoked_multiple_times_on_same_logical_path(self):
        headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}
        data_object = f'/{self.zone_name}/home/{self.rodsuser_username}/parallel_write_no_segfaults.txt'

        # Indicates whether the parallel_write_shutdown operation needs to be called.
        # This is used for clean up when errors occur.
        invoke_parallel_write_shutdown = False

        try:
            # Start a parallel-write operation.
            r = requests.post(self.url_endpoint, headers=headers, data={
                'op': 'parallel_write_init',
                'lpath': data_object,
                'stream-count': 2
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            parallel_write_handle = result['parallel_write_handle']

            # Make sure the parallel_write_shutdown operation is invoked now that we
            # have a parallel-write handle.
            invoke_parallel_write_shutdown = True

            # Show that invoking the operation a second time for the same data object
            # no longer results in the server segfaulting. Instead, the server returns
            # an error, as originally intended.
            r = requests.post(self.url_endpoint, headers=headers, data={
                'op': 'parallel_write_init',
                'lpath': data_object,
                'stream-count': 2
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 500)

        finally:
            # End the parallel write in case something failed.
            # This avoids leaking of parallel-write resources in the HTTP API server.
            if invoke_parallel_write_shutdown:
                r = requests.post(self.url_endpoint, headers=headers, data={
                    'op': 'parallel_write_shutdown',
                    'parallel-write-handle': parallel_write_handle
                })
                self.logger.debug(r.content)

            # Remove the data object.
            r = requests.post(self.url_endpoint, headers=headers, data={
                'op': 'remove',
                'lpath': data_object,
                'catalog-only': 0,
                'no-trash': 1
            })
            self.logger.debug(r.content)

    def test_server_does_not_segfault_when_parallel_write_init_is_invoked_with_insufficient_permissions_on_logical_path(self):
        headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}
        data_object = f'/{self.zone_name}/home/{self.rodsadmin_username}/no_perms_no_segfaults.txt'

        # Show that invoking the parallel_write_init operation fails with an error
        # if the user does not have the appropriate permissions for opening the data object.
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'parallel_write_init',
            'lpath': data_object,
            'stream-count': 2
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 500)

class test_information_endpoint(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        setup_class(cls, {'endpoint_name': 'info', 'init_rodsadmin': False})

    def setUp(self):
        self.assertFalse(self._class_init_error, 'Class initialization failed. Cannot continue.')

    def test_expected_properties_exist_in_json_structure(self):
        r = requests.get(self.url_endpoint)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        info = r.json()
        self.assertIn('api_version', info)
        self.assertIn('build', info)
        self.assertIn('irods_zone', info)
        self.assertIn('max_number_of_parallel_write_streams', info)
        self.assertIn('max_number_of_rows_per_catalog_query', info)
        self.assertIn('max_size_of_request_body_in_bytes', info)
        self.assertIn('openid_connect_enabled', info)

    def test_server_reports_error_when_http_method_is_not_supported(self):
        do_test_server_reports_error_when_http_method_is_not_supported(self)

class test_query_endpoint(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        setup_class(cls, {'endpoint_name': 'query'})

    @classmethod
    def tearDownClass(cls):
        tear_down_class(cls)

    def setUp(self):
        self.assertFalse(self._class_init_error, 'Class initialization failed. Cannot continue.')

    def test_support_for_genquery1(self):
        headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}
        params = {'op': 'execute_genquery', 'parser': 'genquery1', 'query': 'select COLL_NAME'}
        r = requests.get(self.url_endpoint, headers=headers, params=params)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertGreater(len(result['rows']), 0)

    def test_genquery1_no_distinct_option(self):
        rodsadmin_headers = {'Authorization': 'Bearer ' + self.rodsadmin_bearer_token}
        rodsuser_headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}

        # Create a data object on the default resource.
        data_object = os.path.join('/', self.zone_name, 'home', self.rodsuser_username, 'test_genquery1_no_distinct.txt')
        r = requests.post(f'{self.url_base}/data-objects', headers=rodsuser_headers, data={'op': 'touch', 'lpath': data_object})
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)

        resc_name = 'test_ufs_genquery1_no_distinct'

        try:
            # Create a unixfilesystem resource.
            r = requests.post(f'{self.url_base}/resources', headers=rodsadmin_headers, data={
                'op': 'create',
                'name': resc_name,
                'type': 'unixfilesystem',
                'host': self.server_hostname,
                'vault-path': os.path.join('/tmp', f'{resc_name}_vault')
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Replicate the data object.
            r = requests.post(f'{self.url_base}/data-objects', headers=rodsuser_headers, data={
                'op': 'replicate',
                'lpath': data_object,
                'dst-resource': resc_name
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            coll_name = os.path.dirname(data_object)
            data_name = os.path.basename(data_object)
            r = requests.get(self.url_endpoint, headers=rodsuser_headers, params={
                'op': 'execute_genquery',
                'parser': 'genquery1',
                'distinct': 0,
                'query': f"select DATA_NAME where COLL_NAME = '{coll_name}' and DATA_NAME = '{data_name}'"
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            self.assertEqual(len(result['rows']), 2)

        finally:
            # Remove the data object.
            r = requests.post(f'{self.url_base}/data-objects', headers=rodsuser_headers, data={
                'op': 'remove',
                'lpath': data_object,
                'catalog-only': 0,
                'no-trash': 1
            })
            self.logger.debug(r.content)

            # Remove resource.
            r = requests.post(f'{self.url_base}/resources', headers=rodsadmin_headers, data={
                'op': 'remove',
                'name': resc_name
            })
            self.logger.debug(r.content)

    def test_genquery1_case_sensitivity_option(self):
        headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}
        data_object = os.path.join('/', self.zone_name, 'home', self.rodsuser_username, 'test_GenQuery1_CaSE_SENsiTIvE.TxT')

        try:
            # Create a data object with mixed case letters in the name.
            r = requests.post(f'{self.url_base}/data-objects', headers=headers, data={
                'op': 'touch',
                'lpath': data_object
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Show case-insensitive search finds the data object.
            # The use of .upper() and .lower() are just to demonstrate that the case-ness of the
            # input arguments doesn't matter.
            coll_name = os.path.dirname(data_object)
            data_name = os.path.basename(data_object)
            r = requests.get(self.url_endpoint, headers=headers, params={
                'op': 'execute_genquery',
                'parser': 'genquery1',
                'case-sensitive': 0,
                'query': f"select DATA_NAME where COLL_NAME = '{coll_name.upper()}' and DATA_NAME = '{data_name.lower()}'"
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            self.assertEqual(len(result['rows']), 1)
            self.assertEqual(result['rows'][0][0], data_name)

            # Show case-sensitive search does NOT find the data object.
            coll_name = os.path.dirname(data_object)
            data_name = os.path.basename(data_object)
            r = requests.get(self.url_endpoint, headers=headers, params={
                'op': 'execute_genquery',
                'parser': 'genquery1',
                'case-sensitive': 1,
                'query': f"select DATA_NAME where COLL_NAME = '{coll_name.upper()}' and DATA_NAME = '{data_name.lower()}'"
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            self.assertEqual(len(result['rows']), 0)

        finally:
            # Remove the data object.
            r = requests.post(f'{self.url_base}/data-objects', headers=headers, data={
                'op': 'remove',
                'lpath': data_object,
                'catalog-only': 0,
                'no-trash': 1
            })
            self.logger.debug(r.content)

    @unittest.skip('Test needs to be implemented.')
    def test_genquery1_zone_option(self):
        pass

    def test_genquery2_query(self):
        if not config.test_config.get('run_genquery2_tests', False):
            self.skipTest('GenQuery2 tests not enabled. Check [run_genquery2_tests] in test configuration file.')

        headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}
        r = requests.get(self.url_endpoint, headers=headers, params={
            'op': 'execute_genquery',
            'parser': 'genquery2',
            'query': 'select COLL_NAME'
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertGreater(len(result['rows']), 0)
        self.assertIn(['/tempZone/home/http_api'], result['rows'])
        self.assertIn(['/tempZone/trash/home/http_api'], result['rows'])

    def test_genquery2_sql_only_option(self):
        if not config.test_config.get('run_genquery2_tests', False):
            self.skipTest('GenQuery2 tests not enabled. Check [run_genquery2_tests] in test configuration file.')

        headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}
        r = requests.get(self.url_endpoint, headers=headers, params={
            'op': 'execute_genquery',
            'parser': 'genquery2',
            'query': 'select COLL_NAME',
            'sql-only': 1
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertGreater(len(result['sql']), 0)

    def test_support_for_specific_queries(self):
        headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}
        collection_path = os.path.join('/', self.zone_name, 'home', self.rodsuser_username, 'common_ops')
        r = requests.get(self.url_endpoint, headers=headers, params={
            'op': 'execute_specific_query',
            'name': 'ShowCollAcls',
            'args': collection_path,
            'count': 100
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertGreaterEqual(len(result['rows']), 0)

    def test_adding_and_removing_specific_queries(self):
        rodsadmin_headers = {'Authorization': 'Bearer ' + self.rodsadmin_bearer_token}
        rodsuser_headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}
        specific_query_name = 'get_rodsgroup_token_id'

        # Show that rodsusers are NOT allowed to add specific queries.
        r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
            'op': 'add_specific_query',
            'name': specific_query_name,
            'sql': "select token_id from R_TOKN_MAIN where token_name = 'rodsgroup'"
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], irods_error_codes.SYS_NO_API_PRIV)

        # Show that only rodsadmin are allowed to add specific queries.
        r = requests.post(self.url_endpoint, headers=rodsadmin_headers, data={
            'op': 'add_specific_query',
            'name': specific_query_name,
            'sql': "select token_id from R_TOKN_MAIN where token_name = 'rodsgroup'"
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        try:
            # Use the new specific query.
            r = requests.get(self.url_endpoint, headers=rodsuser_headers, params={
                'op': 'execute_specific_query',
                'name': specific_query_name
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)

            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            self.assertEqual(result['rows'][0][0], '200') # The token ID of rodsgroup is always 200.

            # Show that rodsusers are NOT allowed to remove specific queries.
            r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
                'op': 'remove_specific_query',
                'name': specific_query_name
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], irods_error_codes.SYS_NO_API_PRIV)

        finally:
            # Show that only rodsadmins are allowed to remove specific queries.
            r = requests.post(self.url_endpoint, headers=rodsadmin_headers, data={
                'op': 'remove_specific_query',
                'name': specific_query_name
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

    def test_server_reports_error_when_http_method_is_not_supported(self):
        do_test_server_reports_error_when_http_method_is_not_supported(self)

    def test_server_reports_error_when_op_is_not_supported(self):
        do_test_server_reports_error_when_op_is_not_supported(self)

class test_resources_endpoint(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        setup_class(cls, {'endpoint_name': 'resources', 'create_rodsuser': False})

    @classmethod
    def tearDownClass(cls):
        tear_down_class(cls)

    def setUp(self):
        self.assertFalse(self._class_init_error, 'Class initialization failed. Cannot continue.')

    def test_common_operations(self):
        headers = {'Authorization': 'Bearer ' + self.rodsadmin_bearer_token}

        hostname = self.server_hostname
        resc_repl = 'test_repl'
        resc_ufs0 = 'test_ufs0'
        resc_ufs1 = 'test_ufs1'

        # Create three resources (replication w/ two unixfilesystem resources).
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'create',
            'name': resc_repl,
            'type': 'replication'
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)

        # Show the replication resource was created.
        r = requests.get(self.url_endpoint, headers=headers, params={'op': 'stat', 'name': resc_repl})
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(result['exists'], True)
        self.assertIn('id', result['info'])
        self.assertEqual(result['info']['name'], resc_repl)
        self.assertEqual(result['info']['type'], 'replication')
        self.assertEqual(result['info']['zone'], self.zone_name)
        self.assertEqual(result['info']['host'], 'EMPTY_RESC_HOST')
        self.assertEqual(result['info']['vault_path'], 'EMPTY_RESC_PATH')
        self.assertIn('status', result['info'])
        self.assertIn('context', result['info'])
        self.assertIn('comments', result['info'])
        self.assertIn('information', result['info'])
        self.assertIn('free_space', result['info'])
        self.assertIn('free_space_last_modified', result['info'])
        self.assertEqual(result['info']['parent_id'], '')
        self.assertIn('created', result['info'])
        self.assertIn('last_modified', result['info'])
        self.assertIn('last_modified_millis', result['info'])

        # Capture the replication resource's id.
        # This resource is going to be the parent of the unixfilesystem resources.
        # This value is needed to verify the relationship.
        resc_repl_id = result['info']['id']

        for resc_name in [resc_ufs0, resc_ufs1]:
            with self.subTest(f'Create and attach resource [{resc_name}] to [{resc_repl}]'):
                vault_path = os.path.join('/tmp', f'{resc_name}_vault')

                # Create a unixfilesystem resource.
                r = requests.post(self.url_endpoint, headers=headers, data={
                    'op': 'create',
                    'name': resc_name,
                    'type': 'unixfilesystem',
                    'host': hostname,
                    'vault-path': vault_path
                })
                self.logger.debug(r.content)
                self.assertEqual(r.status_code, 200)
                self.assertEqual(r.json()['irods_response']['status_code'], 0)

                # Add the unixfilesystem resource as a child of the replication resource.
                r = requests.post(self.url_endpoint, headers=headers, data={
                    'op': 'add_child',
                    'parent-name': resc_repl,
                    'child-name': resc_name
                })
                self.logger.debug(r.content)
                self.assertEqual(r.status_code, 200)
                self.assertEqual(r.json()['irods_response']['status_code'], 0)

                # Show that the resource was created and configured successfully.
                r = requests.get(self.url_endpoint, headers=headers, params={'op': 'stat', 'name': resc_name})
                self.logger.debug(r.content)
                self.assertEqual(r.status_code, 200)

                result = r.json()
                self.assertEqual(result['irods_response']['status_code'], 0)
                self.assertEqual(result['exists'], True)
                self.assertIn('id', result['info'])
                self.assertEqual(result['info']['name'], resc_name)
                self.assertEqual(result['info']['type'], 'unixfilesystem')
                self.assertEqual(result['info']['zone'], self.zone_name)
                self.assertEqual(result['info']['host'], hostname)
                self.assertEqual(result['info']['vault_path'], vault_path)
                self.assertIn('status', result['info'])
                self.assertIn('context', result['info'])
                self.assertIn('comments', result['info'])
                self.assertIn('information', result['info'])
                self.assertIn('free_space', result['info'])
                self.assertIn('free_space_last_modified', result['info'])
                self.assertEqual(result['info']['parent_id'], resc_repl_id)
                self.assertIn('created', result['info'])
                self.assertIn('last_modified', result['info'])

        # Create a data object targeting the replication resource.
        data_object = os.path.join('/', self.zone_name, 'home', self.rodsadmin_username, 'test_object_for_resources')
        contents = 'hello, iRODS HTTP API!'
        r = requests.post(f'{self.url_base}/data-objects', headers=headers, data={
            'op': 'write',
            'lpath': data_object,
            'resource': resc_repl,
            'bytes': contents,
            'offset': 0
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Show there are two replicas under the replication resource hierarchy.
        r = requests.get(f'{self.url_base}/query', headers=headers, params={
            'op': 'execute_genquery',
            'query': f"select DATA_NAME, RESC_NAME where DATA_NAME = '{os.path.basename(data_object)}'"
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(len(result['rows']), 2)

        resc_tuple = (result['rows'][0][1], result['rows'][1][1])
        self.assertIn(resc_tuple, [(resc_ufs0, resc_ufs1), (resc_ufs1, resc_ufs0)])

        # Trim a replica.
        r = requests.post(f'{self.url_base}/data-objects', headers=headers, data={
            'op': 'trim',
            'lpath': data_object,
            'replica-number': 0
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)

        # Show there is only one replica under the replication resource hierarchy.
        r = requests.get(f'{self.url_base}/query', headers=headers, params={
            'op': 'execute_genquery',
            'query': f"select DATA_NAME, RESC_NAME where DATA_NAME = '{os.path.basename(data_object)}'"
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(len(result['rows']), 1)

        # Launch rebalance.
        r = requests.post(self.url_endpoint, headers=headers, data={'op': 'rebalance', 'name': resc_repl})
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)

        # Give the rebalance operation time to complete!
        time.sleep(3)

        #
        # Clean-up
        #

        # Remove the data object.
        r = requests.post(f'{self.url_base}/data-objects', headers=headers, data={
            'op': 'remove',
            'lpath': data_object,
            'catalog-only': 0,
            'no-trash': 1
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)

        # Remove resources.
        for resc_name in [resc_ufs0, resc_ufs1]:
            with self.subTest(f'Detach and remove resource [{resc_name}] from [{resc_repl}]'):
                # Detach ufs resource from the replication resource.
                r = requests.post(self.url_endpoint, headers=headers, data={
                    'op': 'remove_child',
                    'parent-name': resc_repl,
                    'child-name': resc_name
                })
                self.logger.debug(r.content)
                self.assertEqual(r.status_code, 200)

                result = r.json()
                self.assertEqual(result['irods_response']['status_code'], 0)

                # Remove ufs resource.
                r = requests.post(self.url_endpoint, headers=headers, data={'op': 'remove', 'name': resc_name})
                self.logger.debug(r.content)
                self.assertEqual(r.status_code, 200)

                result = r.json()
                self.assertEqual(result['irods_response']['status_code'], 0)

                # Show that the resource no longer exists.
                r = requests.get(self.url_endpoint, headers=headers, params={'op': 'stat', 'name': resc_name})
                self.logger.debug(r.content)
                self.assertEqual(r.status_code, 200)

                result = r.json()
                self.assertEqual(result['irods_response']['status_code'], 0)
                self.assertEqual(result['exists'], False)

        # Remove replication resource.
        r = requests.post(self.url_endpoint, headers=headers, data={'op': 'remove', 'name': resc_repl})
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)

        # Show that the resource no longer exists.
        r = requests.get(self.url_endpoint, headers=headers, params={'op': 'stat', 'name': resc_repl})
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(result['exists'], False)

    def test_modifying_metadata_atomically(self):
        headers = {'Authorization': 'Bearer ' + self.rodsadmin_bearer_token}
        resource = 'demoResc'

        # Add metadata to the resource.
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'modify_metadata',
            'name': resource,
            'operations': json.dumps([
                {
                    'operation': 'add',
                    'attribute': 'a1',
                    'value': 'v1',
                    'units': 'u1'
                }
            ])
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Show the metadata exists on the resource.
        r = requests.get(f'{self.url_base}/query', headers=headers, params={
            'op': 'execute_genquery',
            'query': "select RESC_NAME where META_RESC_ATTR_NAME = 'a1' and META_RESC_ATTR_VALUE = 'v1' and META_RESC_ATTR_UNITS = 'u1'"
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(result['rows'][0][0], resource)

        # Remove the metadata from the resource.
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'modify_metadata',
            'name': resource,
            'operations': json.dumps([
                {
                    'operation': 'remove',
                    'attribute': 'a1',
                    'value': 'v1',
                    'units': 'u1'
                }
            ])
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Show the metadata no longer exists on the resource.
        r = requests.get(f'{self.url_base}/query', headers=headers, params={
            'op': 'execute_genquery',
            'query': "select RESC_NAME where META_RESC_ATTR_NAME = 'a1' and META_RESC_ATTR_VALUE = 'v1' and META_RESC_ATTR_UNITS = 'u1'"
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(len(result['rows']), 0)

    def test_modifying_resource_properties(self):
        headers = {'Authorization': 'Bearer ' + self.rodsadmin_bearer_token}
        resource = 'test_modifying_resource_properties_original'

        # Create a new resource.
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'create',
            'name': resource,
            'type': 'replication'
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        try:
            # The list of updates to apply in sequence.
            property_map = [
                ('name',        'test_modifying_resource_properties_renamed'),
                ('type',        'passthru'),
                ('host',        'example.org'),
                ('vault_path',  '/tmp/test_modifying_resource_properties_vault'),
                ('status',      'down'),
                ('status',      'up'),
                ('comments',    'test_modifying_resource_properties_comments'),
                ('information', 'test_modifying_resource_properties_information'),
                ('free_space',  'test_modifying_resource_properties_free_space'),
                ('context',     'test_modifying_resource_properties_context')
            ]

            # Apply each update to the resource and verify that each one results
            # in the expected results.
            for p, v in property_map:
                with self.subTest(f'Setting property [{p}] to value [{v}]'):
                    # Change a property of the resource.
                    r = requests.post(self.url_endpoint, headers=headers, data={
                        'op': 'modify',
                        'name': resource,
                        'property': p,
                        'value': v
                    })
                    self.logger.debug(r.content)
                    self.assertEqual(r.status_code, 200)
                    self.assertEqual(r.json()['irods_response']['status_code'], 0)

                    # Make sure to update the "resource" variable following a successful rename.
                    if 'name' == p:
                        self.logger.debug('Resource name updated successfully. Capturing new name.')
                        resource = v

                    # Show the property was modified.
                    r = requests.get(self.url_endpoint, headers=headers, params={
                        'op': 'stat',
                        'name': resource
                    })
                    self.logger.debug(r.content)
                    self.assertEqual(r.status_code, 200)
                    result = r.json()
                    self.assertEqual(result['irods_response']['status_code'], 0)
                    self.assertEqual(result['info'][p], v)

        finally:
            # Remove the resource.
            r = requests.post(self.url_endpoint, headers=headers, data={
                'op': 'remove',
                'name': resource
            })
            self.logger.debug(r.content)

    def test_setting_invalid_value_for_status_property_results_in_an_error(self):
        headers = {'Authorization': 'Bearer ' + self.rodsadmin_bearer_token}
        resource = 'test_modifying_resource_properties_original'

        # Create a new resource.
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'create',
            'name': resource,
            'type': 'replication'
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        try:
            # Capture the status of the resource.
            r = requests.get(self.url_endpoint, headers=headers, params={
                'op': 'stat',
                'name': resource
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            original_status = result['info']['status']

            # Attempt to set the status of the resource to an invalid value.
            r = requests.post(self.url_endpoint, headers=headers, data={
                'op': 'modify',
                'name': resource,
                'property': 'status',
                'value': 'invalid'
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 400)

            # Show the property has not changed.
            r = requests.get(self.url_endpoint, headers=headers, params={
                'op': 'stat',
                'name': resource
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            self.assertEqual(result['info']['status'], original_status)

        finally:
            # Remove the resource.
            r = requests.post(self.url_endpoint, headers=headers, data={
                'op': 'remove',
                'name': resource
            })
            self.logger.debug(r.content)

    def test_server_reports_error_when_http_method_is_not_supported(self):
        do_test_server_reports_error_when_http_method_is_not_supported(self)

    def test_server_reports_error_when_op_is_not_supported(self):
        do_test_server_reports_error_when_op_is_not_supported(self)

class test_rules_endpoint(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        setup_class(cls, {'endpoint_name': 'rules'})

    @classmethod
    def tearDownClass(cls):
        tear_down_class(cls)

    def setUp(self):
        self.assertFalse(self._class_init_error, 'Class initialization failed. Cannot continue.')

    def test_list_all_rule_engine_plugins(self):
        headers = {'Authorization': 'Bearer ' + self.rodsadmin_bearer_token}
        r = requests.get(self.url_endpoint, headers=headers, params={'op': 'list_rule_engines'})
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertGreater(len(result['rule_engine_plugin_instances']), 0)

    def test_execute_rule(self):
        headers = {'Authorization': 'Bearer ' + self.rodsadmin_bearer_token}
        test_msg = 'This was run by the iRODS HTTP API test suite!'

        # Execute rule text against the iRODS rule language.
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'execute',
            'rep-instance': 'irods_rule_engine_plugin-irods_rule_language-instance',
            'rule-text': f'writeLine("stdout", "{test_msg}")'
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
 
        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(result['stderr'], None)

        # The REP always appends a newline character to the result. While we could trim the result,
        # it is better to append a newline character to the expected result to guarantee things align.
        self.assertEqual(result['stdout'], test_msg + '\n')

    def test_remove_delay_rule(self):
        headers = {'Authorization': 'Bearer ' + self.rodsadmin_bearer_token}
        rep_instance = 'irods_rule_engine_plugin-irods_rule_language-instance'

        # Schedule a delay rule to execute in the distant future.
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'execute',
            'rep-instance': rep_instance,
            'rule-text': f'delay("<INST_NAME>{rep_instance}</INST_NAME><PLUSET>1h</PLUSET>") {{ writeLine("serverLog", "iRODS HTTP API"); }}'
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
 
        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)

        # Find the delay rule we just created.
        # This query assumes the test suite is running on a system where no other delay
        # rules are being created.
        r = requests.get(f'{self.url_base}/query', headers=headers, params={
            'op': 'execute_genquery',
            'query': 'select max(RULE_EXEC_ID)'
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
 
        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(len(result['rows']), 1)

        # Remove the delay rule.
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'remove_delay_rule',
            'rule-id': str(result['rows'][0][0])
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
 
        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)

    def test_server_reports_error_when_http_method_is_not_supported(self):
        do_test_server_reports_error_when_http_method_is_not_supported(self)

    def test_server_reports_error_when_op_is_not_supported(self):
        do_test_server_reports_error_when_op_is_not_supported(self)

class test_tickets_endpoint(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        setup_class(cls, {'endpoint_name': 'tickets'})

    @classmethod
    def tearDownClass(cls):
        tear_down_class(cls)

    def setUp(self):
        self.assertFalse(self._class_init_error, 'Class initialization failed. Cannot continue.')

    def test_create_and_remove_ticket_for_data_object(self):
        headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}
        data_object = os.path.join('/', self.zone_name, 'home', self.rodsuser_username, 'test_object')

        # Create a data object.
        r = requests.post(f'{self.url_base}/data-objects', headers=headers, data={'op': 'touch', 'lpath': data_object})
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)

        # Create a ticket.
        ticket_type = 'read'
        ticket_use_count = 1000
        ticket_seconds_until_expiration = 3600
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'create',
            'lpath': data_object,
            'type': ticket_type,
            'use-count': ticket_use_count,
            'seconds-until-expiration': ticket_seconds_until_expiration
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        ticket_string = result['ticket']
        self.assertGreater(len(ticket_string), 0)

        # TODO Show the ticket exists and has the properties we defined during creation.
        # We can use GenQuery for this, but it does seem better to provide a convenience operation
        # for this.
        r = requests.get(f'{self.url_base}/query', headers=headers, params={
            'op': 'execute_genquery',
            'query': 'select TICKET_STRING, TICKET_TYPE, TICKET_DATA_NAME, TICKET_USES_LIMIT, TICKET_EXPIRY'
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(result['rows'][0][0], ticket_string)
        self.assertEqual(result['rows'][0][1], ticket_type)
        self.assertEqual(result['rows'][0][2], os.path.basename(data_object))
        self.assertEqual(result['rows'][0][3], str(ticket_use_count))
        self.assertGreater(int(result['rows'][0][4]), 0)

        # Remove the ticket.
        r = requests.post(self.url_endpoint, headers=headers, data={'op': 'remove', 'name': ticket_string})
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)

        # Show the ticket no longer exists.
        r = requests.get(f'{self.url_base}/query', headers=headers, params={
            'op': 'execute_genquery',
            'query': 'select TICKET_STRING'
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(len(result['rows']), 0)

        # Remove the data object.
        r = requests.post(f'{self.url_base}/data-objects', headers=headers, data={
            'op': 'remove',
            'lpath': data_object,
            'catalog-only': 0,
            'no-trash': 1
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)

    def test_create_and_remove_ticket_for_collection(self):
        headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}

        # Create a write ticket.
        ticket_type = 'write'
        ticket_path = os.path.join('/', self.zone_name, 'home', self.rodsuser_username) 
        ticket_use_count = 2000
        ticket_groups = 'public'
        ticket_hosts = self.server_hostname
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'create',
            'lpath': ticket_path,
            'type': ticket_type,
            'use-count': ticket_use_count,
            'seconds-until-expiration': '3600',
            'users': f'{self.rodsadmin_username},{self.rodsuser_username}',
            'groups': ticket_groups,
            'hosts': ticket_hosts
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        ticket_string = result['ticket']
        self.assertGreater(len(ticket_string), 0)

        # TODO Show the ticket exists and has the properties we defined during creation.
        # We can use GenQuery for this, but it does seem better to provide a convenience operation
        # for this.
        r = requests.get(f'{self.url_base}/query', headers=headers, params={
            'op': 'execute_genquery',
            'query': 'select TICKET_STRING, TICKET_TYPE, TICKET_COLL_NAME, TICKET_USES_LIMIT, TICKET_ALLOWED_USER_NAME, TICKET_ALLOWED_GROUP_NAME, TICKET_ALLOWED_HOST'
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(result['rows'][0][0], ticket_string)
        self.assertEqual(result['rows'][0][1], ticket_type)
        self.assertEqual(result['rows'][0][2], ticket_path)
        self.assertEqual(result['rows'][0][3], str(ticket_use_count))
        self.assertIn(result['rows'][0][4], [self.rodsadmin_username, self.rodsuser_username])
        self.assertEqual(result['rows'][0][5], ticket_groups)
        self.assertGreater(len(result['rows'][0][6]), 0)

        # Remove the ticket.
        r = requests.post(self.url_endpoint, headers=headers, data={'op': 'remove', 'name': ticket_string})
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)

        # Show the ticket no longer exists.
        r = requests.get(f'{self.url_base}/query', headers=headers, params={
            'op': 'execute_genquery',
            'query': 'select TICKET_STRING'
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(len(result['rows']), 0)

    @unittest.skip('Test and HTTP API operation need to be implemented.')
    def test_modification_of_ticket_properties(self):
        pass

    def test_server_reports_error_when_http_method_is_not_supported(self):
        do_test_server_reports_error_when_http_method_is_not_supported(self)

    def test_server_reports_error_when_op_is_not_supported(self):
        do_test_server_reports_error_when_op_is_not_supported(self)

class test_users_groups_endpoint(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        setup_class(cls, {'endpoint_name': 'users-groups'})

    @classmethod
    def tearDownClass(cls):
        tear_down_class(cls)

    def setUp(self):
        self.assertFalse(self._class_init_error, 'Class initialization failed. Cannot continue.')

    def test_create_stat_and_remove_rodsuser(self):
        new_username = 'test_user_rodsuser'
        user_type = 'rodsuser'
        headers = {'Authorization': 'Bearer ' + self.rodsadmin_bearer_token}

        # Create a new user.
        data = {'op': 'create_user', 'name': new_username, 'zone': self.zone_name, 'user-type': user_type}
        r = requests.post(self.url_endpoint, headers=headers, data=data)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
 
        # Stat the user.
        params = {'op': 'stat', 'name': new_username, 'zone': self.zone_name}
        r = requests.get(self.url_endpoint, headers=headers, params=params)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        stat_info = r.json()
        self.assertEqual(stat_info['irods_response']['status_code'], 0)
        self.assertEqual(stat_info['exists'], True)
        self.assertIn('id', stat_info)
        self.assertEqual(stat_info['local_unique_name'], f'{new_username}#{self.zone_name}')
        self.assertEqual(stat_info['type'], user_type)

        # Remove the user.
        data = {'op': 'remove_user', 'name': new_username, 'zone': self.zone_name}
        r = requests.post(self.url_endpoint, headers=headers, data=data)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

    def test_create_stat_and_remove_rodsadmin(self):
        new_username = 'test_user_rodsadmin'
        user_type = 'rodsadmin'
        headers = {'Authorization': 'Bearer ' + self.rodsadmin_bearer_token}

        # Create a new user.
        data = {'op': 'create_user', 'name': new_username, 'zone': self.zone_name, 'user-type': user_type}
        r = requests.post(self.url_endpoint, headers=headers, data=data)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
 
        # Stat the user.
        params = {'op': 'stat', 'name': new_username, 'zone': self.zone_name}
        r = requests.get(self.url_endpoint, headers=headers, params=params)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        stat_info = r.json()
        self.assertEqual(stat_info['irods_response']['status_code'], 0)
        self.assertEqual(stat_info['exists'], True)
        self.assertIn('id', stat_info)
        self.assertEqual(stat_info['local_unique_name'], f'{new_username}#{self.zone_name}')
        self.assertEqual(stat_info['type'], user_type)

        # Remove the user.
        data = {'op': 'remove_user', 'name': new_username, 'zone': self.zone_name}
        r = requests.post(self.url_endpoint, headers=headers, data=data)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

    def test_create_stat_and_remove_groupadmin(self):
        new_username = 'test_user_groupadmin'
        user_type = 'groupadmin'
        headers = {'Authorization': 'Bearer ' + self.rodsadmin_bearer_token}

        # Create a new user.
        data = {'op': 'create_user', 'name': new_username, 'zone': self.zone_name, 'user-type': user_type}
        r = requests.post(self.url_endpoint, headers=headers, data=data)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
 
        # Stat the user.
        params = {'op': 'stat', 'name': new_username, 'zone': self.zone_name}
        r = requests.get(self.url_endpoint, headers=headers, params=params)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        stat_info = r.json()
        self.assertEqual(stat_info['irods_response']['status_code'], 0)
        self.assertEqual(stat_info['exists'], True)
        self.assertIn('id', stat_info)
        self.assertEqual(stat_info['local_unique_name'], f'{new_username}#{self.zone_name}')
        self.assertEqual(stat_info['type'], user_type)

        # Remove the user.
        data = {'op': 'remove_user', 'name': new_username, 'zone': self.zone_name}
        r = requests.post(self.url_endpoint, headers=headers, data=data)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

    def test_add_remove_user_to_and_from_group(self):
        headers = {'Authorization': 'Bearer ' + self.rodsadmin_bearer_token}

        # Create a new group.
        new_group = 'test_group'
        data = {'op': 'create_group', 'name': new_group}
        r = requests.post(self.url_endpoint, headers=headers, data=data)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Stat the group.
        params = {'op': 'stat', 'name': new_group}
        r = requests.get(self.url_endpoint, headers=headers, params=params)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        stat_info = r.json()
        self.assertEqual(stat_info['irods_response']['status_code'], 0)
        self.assertEqual(stat_info['exists'], True)
        self.assertIn('id', stat_info)
        self.assertEqual(stat_info['type'], 'rodsgroup')

        # Create a new user.
        new_username = 'test_user_rodsuser'
        user_type = 'rodsuser'
        data = {'op': 'create_user', 'name': new_username, 'zone': self.zone_name, 'user-type': user_type}
        r = requests.post(self.url_endpoint, headers=headers, data=data)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)
 
        # Add user to group.
        data = {'op': 'add_to_group', 'group': new_group, 'user': new_username, 'zone': self.zone_name}
        r = requests.post(self.url_endpoint, headers=headers, data=data)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Show that the user is a member of the group.
        params = {'op': 'is_member_of_group', 'group': new_group, 'user': new_username, 'zone': self.zone_name}
        r = requests.get(self.url_endpoint, headers=headers, params=params)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(result['is_member'], True)

        # Remove user from group.
        data = {'op': 'remove_from_group', 'group': new_group, 'user': new_username, 'zone': self.zone_name}
        r = requests.post(self.url_endpoint, headers=headers, data=data)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Remove the user.
        data = {'op': 'remove_user', 'name': new_username, 'zone': self.zone_name}
        r = requests.post(self.url_endpoint, headers=headers, data=data)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Remove group.
        data = {'op': 'remove_group', 'name': new_group}
        r = requests.post(self.url_endpoint, headers=headers, data=data)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Show that the group no longer exists.
        params = {'op': 'stat', 'name': new_group}
        r = requests.get(self.url_endpoint, headers=headers, params=params)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        stat_info = r.json()
        self.assertEqual(stat_info['irods_response']['status_code'], 0)
        self.assertEqual(stat_info['exists'], False)

    def test_only_a_rodsadmin_can_change_the_type_of_a_user(self):
        headers = {'Authorization': 'Bearer ' + self.rodsadmin_bearer_token}

        # Create a new user.
        new_username = 'test_user_rodsuser'
        user_type = 'rodsuser'
        data = {'op': 'create_user', 'name': new_username, 'zone': self.zone_name, 'user-type': user_type}
        r = requests.post(self.url_endpoint, headers=headers, data=data)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Show that a rodsadmin can change the type of the new user.
        new_user_type = 'groupadmin'
        data = {'op': 'set_user_type', 'name': new_username, 'zone': self.zone_name, 'new-user-type': new_user_type}
        r = requests.post(self.url_endpoint, headers=headers, data=data)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Show that a non-admin cannot change the new user's password.
        headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}
        data = {'op': 'set_user_type', 'name': new_username, 'zone': self.zone_name, 'new-user-type': 'rodsuser'}
        r = requests.post(self.url_endpoint, headers=headers, data=data)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], irods_error_codes.SYS_NO_API_PRIV)

        # Show that the user type matches the type set by the rodsadmin.
        params = {'op': 'stat', 'name': new_username, 'zone': self.zone_name}
        r = requests.get(self.url_endpoint, headers=headers, params=params)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        stat_info = r.json()
        self.assertEqual(stat_info['irods_response']['status_code'], 0)
        self.assertEqual(stat_info['exists'], True)
        self.assertEqual(stat_info['local_unique_name'], f'{new_username}#{self.zone_name}')
        self.assertEqual(stat_info['type'], new_user_type)

        # Remove the user.
        headers = {'Authorization': 'Bearer ' + self.rodsadmin_bearer_token}
        data = {'op': 'remove_user', 'name': new_username, 'zone': self.zone_name}
        r = requests.post(self.url_endpoint, headers=headers, data=data)
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

    def test_rodsusers_cannot_change_passwords(self):
        headers = {'Authorization': 'Bearer ' + self.rodsadmin_bearer_token}

        # Show that a non-admin cannot change the new user's password.
        headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'set_password',
            'name': self.rodsuser_username,
            'zone': self.zone_name,
            'new-password': 'not_going_to_work'
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], irods_error_codes.SYS_NO_API_PRIV)

        # Authenticate as the user to prove the first password modification was successful.
        r = requests.post(f'{self.url_base}/authenticate', auth=(self.rodsuser_username, config.test_config['rodsuser']['password']))
        self.assertEqual(r.status_code, 200)
        self.assertGreater(len(r.text), 0)

    def test_listing_all_users_in_zone(self):
        r = requests.get(self.url_endpoint, headers={'Authorization': f'Bearer {self.rodsuser_bearer_token}'}, params={'op': 'users'})
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertIn({'name': self.rodsadmin_username, 'zone': self.zone_name}, result['users'])
        self.assertIn({'name': self.rodsuser_username, 'zone': self.zone_name}, result['users'])

    def test_listing_all_groups_in_zone(self):
        headers = {'Authorization': f'Bearer {self.rodsadmin_bearer_token}'}

        # Create a new group.
        new_group = 'test_group'
        r = requests.post(self.url_endpoint, headers=headers, data={'op': 'create_group', 'name': new_group})
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Get all groups.
        r = requests.get(self.url_endpoint, headers={'Authorization': f'Bearer {self.rodsuser_bearer_token}'}, params={'op': 'groups'})
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertIn('public', result['groups'])
        self.assertIn(new_group, result['groups'])

        # Remove the new group.
        r = requests.post(self.url_endpoint, headers=headers, data={'op': 'remove_group', 'name': new_group})
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

    def test_modifying_metadata_atomically(self):
        headers = {'Authorization': 'Bearer ' + self.rodsadmin_bearer_token}
        username = self.rodsuser_username

        # Add metadata to the user.
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'modify_metadata',
            'name': username,
            'operations': json.dumps([
                {
                    'operation': 'add',
                    'attribute': 'a1',
                    'value': 'v1',
                    'units': 'u1'
                }
            ])
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Show the metadata exists on the user.
        r = requests.get(f'{self.url_base}/query', headers=headers, params={
            'op': 'execute_genquery',
            'query': "select USER_NAME where META_USER_ATTR_NAME = 'a1' and META_USER_ATTR_VALUE = 'v1' and META_USER_ATTR_UNITS = 'u1'"
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(result['rows'][0][0], username)

        # Remove the metadata from the user.
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'modify_metadata',
            'name': username,
            'operations': json.dumps([
                {
                    'operation': 'remove',
                    'attribute': 'a1',
                    'value': 'v1',
                    'units': 'u1'
                }
            ])
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        # Show the metadata no longer exists on the user.
        r = requests.get(f'{self.url_base}/query', headers=headers, params={
            'op': 'execute_genquery',
            'query': "select USER_NAME where META_USER_ATTR_NAME = 'a1' and META_USER_ATTR_VALUE = 'v1' and META_USER_ATTR_UNITS = 'u1'"
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)
        self.assertEqual(len(result['rows']), 0)

    def test_server_reports_error_when_http_method_is_not_supported(self):
        do_test_server_reports_error_when_http_method_is_not_supported(self)

    def test_server_reports_error_when_op_is_not_supported(self):
        do_test_server_reports_error_when_op_is_not_supported(self)

    @unittest.skip('Test needs to be implemented.')
    def test_create_user_returns_error_when_missing_required_parameters(self):
        pass

    @unittest.skip('Test needs to be implemented.')
    def test_remove_user_returns_error_when_missing_required_parameters(self):
        pass

    @unittest.skip('Test needs to be implemented.')
    def test_create_group_returns_error_when_missing_required_parameters(self):
        pass

    @unittest.skip('Test needs to be implemented.')
    def test_remove_group_returns_error_when_missing_required_parameters(self):
        pass

    @unittest.skip('Test needs to be implemented.')
    def test_add_to_group_returns_error_when_missing_required_parameters(self):
        pass

    @unittest.skip('Test needs to be implemented.')
    def test_remove_from_group_returns_error_when_missing_required_parameters(self):
        pass

    @unittest.skip('Test needs to be implemented.')
    def test_listing_users_returns_error_when_missing_required_parameters(self):
        pass

    @unittest.skip('Test needs to be implemented.')
    def test_listing_groups_returns_error_when_missing_required_parameters(self):
        pass

    @unittest.skip('Test needs to be implemented.')
    def test_listing_members_of_group_returns_error_when_missing_required_parameters(self):
        pass

    @unittest.skip('Test needs to be implemented.')
    def test_is_member_of_group_returns_error_when_missing_required_parameters(self):
        pass

    @unittest.skip('Test needs to be implemented.')
    def test_stat_returns_error_when_missing_required_parameters(self):
        pass

class test_zones_endpoint(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        setup_class(cls, {'endpoint_name': 'zones'})

    @classmethod
    def tearDownClass(cls):
        tear_down_class(cls)

    def setUp(self):
        self.assertFalse(self._class_init_error, 'Class initialization failed. Cannot continue.')

    def test_report_operation(self):
        headers = {'Authorization': 'Bearer ' + self.rodsadmin_bearer_token}
        r = requests.get(self.url_endpoint, headers=headers, params={'op': 'report'})
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)

        result = r.json()
        self.assertEqual(result['irods_response']['status_code'], 0)

        zone_report = result['zone_report']
        self.assertIn('schema_version', zone_report)
        self.assertIn('zones', zone_report)
        self.assertGreaterEqual(len(zone_report['zones']), 1)

    def test_adding_removing_and_modifying_zones(self):
        headers = {'Authorization': 'Bearer ' + self.rodsadmin_bearer_token}

        # Add a remote zone to the local zone.
        # The new zone will not have any connection information or anything else.
        zone_name = 'other_zone'
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'add',
            'name': zone_name
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        try:
            # Show the new zone exists by executing the stat operation on it.
            r = requests.get(self.url_endpoint, headers=headers, params={
                'op': 'stat',
                'name': zone_name
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)

            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            self.assertEqual(result['exists'], True)
            self.assertEqual(result['info']['name'], zone_name)
            self.assertEqual(result['info']['type'], 'remote')
            self.assertEqual(result['info']['connection_info'], '')
            self.assertEqual(result['info']['comment'], '')

            # The properties to update.
            property_map = [
                ('name',            'other_zone_renamed'),
                ('connection_info', 'example.org:1247'),
                ('comment',         'updated comment')
            ]

            # Change the properties of the new zone.
            for p, v in property_map:
                with self.subTest(f'Setting property [{p}] to value [{v}]'):
                    r = requests.post(self.url_endpoint, headers=headers, data={
                        'op': 'modify',
                        'name': zone_name,
                        'property': p,
                        'value': v 
                    })
                    self.logger.debug(r.content)
                    self.assertEqual(r.status_code, 200)
                    self.assertEqual(r.json()['irods_response']['status_code'], 0)

                    # Capture the new name of the zone following its renaming.
                    if 'name' == p:
                        zone_name = v

                    # Show the new zone was modified successfully.
                    r = requests.get(self.url_endpoint, headers=headers, params={
                        'op': 'stat',
                        'name': zone_name
                    })
                    self.logger.debug(r.content)
                    self.assertEqual(r.status_code, 200)

                    result = r.json()
                    self.assertEqual(result['irods_response']['status_code'], 0)
                    self.assertEqual(result['exists'], True)
                    self.assertEqual(result['info'][p], v)

        finally:
            # Remove the remote zone.
            r = requests.post(self.url_endpoint, headers=headers, data={
                'op': 'remove',
                'name': zone_name
            })
            self.logger.debug(r.content)

    def test_setting_invalid_connection_info_results_in_an_error(self):
        headers = {'Authorization': 'Bearer ' + self.rodsadmin_bearer_token}

        # Add a remote zone to the local zone.
        # The new zone will not have any connection information or anything else.
        zone_name = 'other_zone'
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'add',
            'name': zone_name
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], 0)

        try:
            # Capture the new zone's connection information.
            r = requests.get(self.url_endpoint, headers=headers, params={
                'op': 'stat',
                'name': zone_name
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)

            result = r.json()
            self.assertEqual(result['irods_response']['status_code'], 0)
            self.assertEqual(result['exists'], True)
            original_conn_info = result['info']['connection_info']

            # The properties to update.
            value_error_map = [
                ('example.org:', irods_error_codes.CAT_INVALID_ARGUMENT),
                (':1247', irods_error_codes.CAT_HOSTNAME_INVALID),
                (':', irods_error_codes.CAT_HOSTNAME_INVALID),
                ('', irods_error_codes.CAT_INVALID_ARGUMENT),
                ('example.org', irods_error_codes.CAT_INVALID_ARGUMENT)
            ]

            # Attempt to change the properties of the new zone.
            for conn_info, ec in value_error_map:
                with self.subTest(f'Invalid connection info [{conn_info}] results in error code [{ec}]'):
                    r = requests.post(self.url_endpoint, headers=headers, data={
                        'op': 'modify',
                        'name': zone_name,
                        'property': 'connection_info',
                        'value': conn_info 
                    })
                    self.logger.debug(r.content)
                    self.assertEqual(r.status_code, 200)
                    self.assertEqual(r.json()['irods_response']['status_code'], ec)

                    # Show the new zone was not changed.
                    r = requests.get(self.url_endpoint, headers=headers, params={
                        'op': 'stat',
                        'name': zone_name
                    })
                    self.logger.debug(r.content)
                    self.assertEqual(r.status_code, 200)

                    result = r.json()
                    self.assertEqual(result['irods_response']['status_code'], 0)
                    self.assertEqual(result['exists'], True)
                    self.assertEqual(result['info']['connection_info'], original_conn_info)

        finally:
            # Remove the remote zone.
            r = requests.post(self.url_endpoint, headers=headers, data={
                'op': 'remove',
                'name': zone_name
            })
            self.logger.debug(r.content)

    @unittest.skip('TODO(#290): Enable once "iadmin modzonecollacl" is fully understood.')
    def test_modifying_permissions_of_zone_collection(self):
        rodsadmin_headers = {'Authorization': 'Bearer ' + self.rodsadmin_bearer_token}
        rodsuser_headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}
        zone_name = 'tempZone'

        try:
            # Show the rodsuser cannot list the contents of the zone collection.
            r = requests.get(f'{self.url_base}/collections', headers=rodsuser_headers, params={
                'op': 'list',
                'lpath': f'/{zone_name}'
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 400)
            self.assertEqual(r.json()['irods_response']['status_code'], irods_error_codes.NOT_A_COLLECTION)

            # Give the rodsuser permission to read the contents of the zone collection.
            r = requests.post(self.url_endpoint, headers=rodsadmin_headers, data={
                'op': 'set_zone_collection_permission',
                'name': zone_name,
                'user': self.rodsuser_username,
                'permission': 'read'
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Show the rodsuser can now list the contents of the zone collection.
            r = requests.get(f'{self.url_base}/collections', headers=rodsuser_headers, params={
                'op': 'list',
                'lpath': f'/{zone_name}'
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Remove the rodsuser's permissions from the zone collection.
            r = requests.post(self.url_endpoint, headers=rodsadmin_headers, data={
                'op': 'set_zone_collection_permission',
                'name': zone_name,
                'user': self.rodsuser_username,
                'permission': 'null'
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()['irods_response']['status_code'], 0)

            # Show the rodsuser can no longer list the contents of the zone collection.
            r = requests.get(f'{self.url_base}/collections', headers=rodsuser_headers, params={
                'op': 'list',
                'lpath': f'/{zone_name}'
            })
            self.logger.debug(r.content)
            self.assertEqual(r.status_code, 400)
            self.assertEqual(r.json()['irods_response']['status_code'], irods_error_codes.NOT_A_COLLECTION)

        finally:
            # Remove the rodsuser's permissions from the zone collection.
            r = requests.post(self.url_endpoint, headers=rodsadmin_headers, data={
                'op': 'set_zone_collection_permission',
                'name': zone_name,
                'user': self.rodsuser_username,
                'permission': 'null'
            })
            self.logger.debug(r.content)

    def test_non_admin_users_are_not_allowed_to_add_remove_or_modify_zones(self):
        headers = {'Authorization': 'Bearer ' + self.rodsuser_bearer_token}

        # Non-admins are not allowed to add zones.
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'add',
            'name': 'other_zone'
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], irods_error_codes.SYS_NO_API_PRIV)

        # Non-admins are not allowed to remove zones.
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'remove',
            'name': 'tempZone'
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], irods_error_codes.SYS_NO_API_PRIV)

        # Non-admins are not allowed to modify zones.
        r = requests.post(self.url_endpoint, headers=headers, data={
            'op': 'modify',
            'name': 'tempZone',
            'property': 'comment',
            'value': 'nope'
        })
        self.logger.debug(r.content)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()['irods_response']['status_code'], irods_error_codes.SYS_NO_API_PRIV)

        # TODO(#290): Disabled until "iadmin modzonecollacl" is fully understood.
        # Non-admins are not allowed to modify ACLs of zone collection.
        #r = requests.post(self.url_endpoint, headers=rodsuser_headers, data={
        #    'op': 'set_zone_collection_permission',
        #    'name': zone_name,
        #    'user': self.rodsuser_username,
        #    'permission': 'read'
        #})
        #self.logger.debug(r.content)
        #self.assertEqual(r.status_code, 200)
        #self.assertEqual(r.json()['irods_response']['status_code'], irods_error_codes.SYS_NO_API_PRIV)

    def test_server_reports_error_when_http_method_is_not_supported(self):
        do_test_server_reports_error_when_http_method_is_not_supported(self)

    def test_server_reports_error_when_op_is_not_supported(self):
        do_test_server_reports_error_when_op_is_not_supported(self, test_http_post_method=False)

if __name__ == '__main__':
    unittest.main()
