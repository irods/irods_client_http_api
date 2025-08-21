#!/bin/bash

set -x

# Compile the HTTP API project
mkdir /_build_http_api
cd /_build_http_api
cmake -GNinja -DIRODS_BUILD_WITH_WERROR=NO /http_api_source
ninja package

# Copy any generated deb packages
if ls ./*.deb 1> /dev/null 2>&1; then
    cp ./*.deb /packages_output
fi

# Copy any generated rpm packages
if ls ./*.rpm 1> /dev/null 2>&1; then
    cp ./*.rpm /packages_output
fi
