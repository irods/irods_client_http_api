from config import test_config
import pytest
import requests
import base64
import re
import logging
import html

@pytest.fixture
def irods_http_api_url_base():
    return f'http://{test_config["host"]}:{test_config["port"]}{test_config["url_base"]}'

@pytest.fixture
def auth_url(irods_http_api_url_base):
    return f'{irods_http_api_url_base}/authenticate'


@pytest.mark.parametrize("username, password, expected_result", [('rods', 'rods', requests.codes.ok),
                                                                 ('', '', requests.codes.unauthorized),
                                                                 ('not', 'valid', requests.codes.unauthorized),
                                                                 ('a'*200, 'b'*200, requests.codes.unauthorized)])
def test_post_basic_login(username, password, expected_result, auth_url):
    res = requests.post(auth_url, auth=(username, password))

    # Got a good code, assume we passed...
    assert res.status_code == expected_result


@pytest.mark.parametrize("method", [requests.head,
                                    requests.get,
                                    requests.put,
                                    requests.delete,
                                    requests.patch])
def test_other_http_methods(method, auth_url):
    res = method(auth_url)
    assert res.status_code == requests.codes.method_not_allowed
