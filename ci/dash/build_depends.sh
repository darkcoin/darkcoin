#!/usr/bin/env bash
# Copyright (c) 2021-2023 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# This script is executed inside the builder image

export LC_ALL=C.UTF-8

set -e

source ./ci/dash/matrix.sh

unset CC; unset CXX
unset DISPLAY

mkdir -p $CACHE_DIR/depends
mkdir -p $CACHE_DIR/sdk-sources

ln -s $CACHE_DIR/depends ${DEPENDS_DIR}/built
ln -s $CACHE_DIR/sdk-sources ${DEPENDS_DIR}/sdk-sources

if [[ "${HOST}" == "x86_64-apple-darwin" ]]; then
    ./ci/dash/setup-sdk.sh
fi

make $MAKEJOBS -C depends HOST=$HOST $DEP_OPTS
