#!/usr/bin/env bash
set -eux
xcodebuild -version
MACOSX_DEPLOYMENT_TARGET=10.8
cd build
cmake -D LIBTYPE:STRING=STATIC -D CMAKE_OSX_DEPLOYMENT_TARGET:STRING=10.8 ..
make -j4
sudo make install