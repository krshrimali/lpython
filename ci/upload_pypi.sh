#!/bin/bash

set -e
set -x

if [[ $CI_COMMIT_TAG == "" ]]; then
    # Development version
    export TWINE_REPOSITORY_URL="https://test.pypi.org/legacy/"
    set +x
    export TWINE_USERNAME=${TEST_PYPI_USERNAME}
    export TWINE_PASSWORD=${TEST_PYPI_PASSWORD}
    set -x
else
    # Release version
    export TWINE_REPOSITORY_URL="https://upload.pypi.org/legacy/"
    set +x
    export TWINE_USERNAME=${PYPI_USERNAME}
    export TWINE_PASSWORD=${PYPI_PASSWORD}
    set -x
fi

lfortran_version=$(<version)

twine upload dist/lfortran-${lfortran_version}.tar.gz
