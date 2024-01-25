import pytest
import requests
import base64
import re
import logging
import html

# Pragmatic
# Search for IRODS_HTTP_API_BASE_URL
# and IRODS_CLIENT_VERSION
# TOOD: Consider turning this into a fixture
AUTH_URL = 'http://127.0.0.1:9000/irods-http-api/0.2.0/authenticate'

@pytest.mark.parametrize("username, password, expected_result", [('bob', 'bob', requests.codes.ok),
                                                                 ('', '', requests.codes.unauthorized),
                                                                 ('not', 'valid', requests.codes.bad_request),
                                                                 ('a'*200, 'b'*200, requests.codes.unauthorized)])
def test_oidc_resource_owner_password_credentials_login(username, password, expected_result):
    to_be_encoded = f'{username}:{password}'
    encoded_user_pass = base64.b64encode(to_be_encoded.encode())

    res = requests.post(AUTH_URL, headers={'Authorization': f'iRODS {encoded_user_pass.decode()}'})

    # Got a good code, assume we passed...
    assert res.status_code == expected_result

def test_oidc_authorization_code_login():
    # Session required for cookie
    s = requests.Session()

    # Go to login page
    # requests automatically redirects
    res = s.get(AUTH_URL)
    assert res.status_code == requests.codes.ok

    data = res.text

    # Extract the 'true' auth endpoint
    m = re.search('action="(?P<addr>.*)" ', data)
    assert m is not None

    # Handle '&amp;'
    extracted_url = html.unescape(m.group('addr'))
    logging.debug(f'extracted_url is: [{extracted_url}]')

    # Search for form action
    # Extract URL
    res = s.post(extracted_url, data={'username': 'bob', 'password': 'bob', 'credentialId': ''})
    assert res.status_code == requests.codes.ok

    # Make sure we weren't redirected back the the auth page
    assert re.match('<!DOCTYPE html>', res.text) is None

def test_oidc_authorization_code_non_irods_user():
    # Session required for cookie
    s = requests.Session()

    # Go to login page
    # requests automatically redirects
    res = s.get(AUTH_URL)
    assert res.status_code == requests.codes.ok

    data = res.text

    # Extract the 'true' auth endpoint
    m = re.search('action="(?P<addr>.*)" ', data)
    assert m is not None

    # Handle '&amp;'
    extracted_url = html.unescape(m.group('addr'))
    logging.debug(f'extracted_url is: [{extracted_url}]')

    # Search for form action
    # Extract URL
    res = s.post(extracted_url, data={'username': 'non_irods_user', 'password': 'bad', 'credentialId': ''})
    assert res.status_code == requests.codes.bad_request


@pytest.mark.parametrize("username, password, expected_result", [('rods', 'rods', requests.codes.ok),
                                                                 ('', '', requests.codes.unauthorized),
                                                                 ('not', 'valid', requests.codes.unauthorized),
                                                                 ('a'*200, 'b'*200, requests.codes.unauthorized)])
def test_post_basic_login(username, password, expected_result):
    res = requests.post(AUTH_URL, auth=(username, password))

    # Got a good code, assume we passed...
    assert res.status_code == expected_result


@pytest.mark.parametrize("state, code, expected_result", [('placeholder', 'bad_code', requests.codes.bad_request),
                                                          ('', 'bad_code', requests.codes.bad_request),
                                                          ('placeholder', '', requests.codes.bad_request),
                                                          ('a'*200, 'b'*200, requests.codes.bad_request)])
def test_get_oidc_errors(state, code, expected_result):
    res = requests.get(AUTH_URL, params={'state': state, 'code': code})

    assert res.status_code == expected_result

@pytest.mark.parametrize("method", [requests.head,
                                    requests.put,
                                    requests.delete,
                                    requests.patch])
def test_other_http_methods(method):
    res = method(AUTH_URL)
    assert res.status_code == requests.codes.method_not_allowed
