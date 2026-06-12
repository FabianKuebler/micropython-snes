#!/bin/sh
# Fetch and unpack the Calypsi 65816 toolchain into toolchain/ (gitignored).
# By installing/using it you accept its license (personal hobby use only):
# see toolchain/usr/local/lib/calypsi-65816-5.17/LICENSE after extraction.
set -e
cd "$(dirname "$0")/.."
VER=5.17
URL=https://github.com/hth313/Calypsi-tool-chains/releases/download/$VER/calypsi-65816-$VER.deb
mkdir -p toolchain
curl -sL -o /tmp/calypsi-65816.deb "$URL"
dpkg-deb -x /tmp/calypsi-65816.deb toolchain/
toolchain/usr/local/lib/calypsi-65816-$VER/bin/cc65816 --version
