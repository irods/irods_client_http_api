#! /bin/bash

set -x

# Compile the HTTP API project.
mkdir /_build_http_api
cd /_build_http_api
cmake -GNinja /http_api_source
ninja package
cp ./*.deb /packages_output
