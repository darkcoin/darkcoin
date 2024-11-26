#!/usr/bin/env bash
# Copyright (c) 2024 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

## WARNING! THIS SCRIPT IS ALSO COPIED INTO THE GUIX CONTAINER ##
## MAKE SURE THIS SCRIPT DOES NOT RELY ON EXTERNAL SCRIPTS AS  ##
## DOCKER WILL NOT COPY THEM TO THE CONTAINER                  ##

SDK_URL="${SDK_URL:-https://bitcoincore.org/depends-sources/sdks}"
SDK_PATH="${SDK_PATH:-depends/SDKs}"
SDK_SRCS="${SDK_SOURCES:-depends/sdk-sources}"
XCODE_VERSION="${XCODE_VERSION:-15.0}"
XCODE_RELEASE="${XCODE_RELEASE:-15A240d}"
XCODE_ARCHIVE="Xcode-${XCODE_VERSION}-${XCODE_RELEASE}-extracted-SDK-with-libcxx-headers"
XCODE_AR_PATH="${SDK_SRCS}/${XCODE_ARCHIVE}.tar.gz"

mkdir -p "${SDK_PATH}" "${SDK_SRCS}"
if [ ! -f "${XCODE_AR_PATH}" ]; then
    echo "Downloading macOS SDK..."
    curl --location --fail "${SDK_URL}/${XCODE_ARCHIVE}.tar.gz" -o "${XCODE_AR_PATH}"
fi
if [ -f "${XCODE_AR_PATH}" ] && [ ! -d "${SDK_PATH}/${XCODE_ARCHIVE}" ]; then
    echo "Extracting macOS SDK..."
    tar -C "${SDK_PATH}" -xf "${XCODE_AR_PATH}"
fi
