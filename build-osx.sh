#!/usr/bin/env bash
#set -eux
xcodebuild -version
MACOSX_DEPLOYMENT_TARGET=10.8

brew install cmake

cd build
cmake -D LIBTYPE:STRING=STATIC -D CMAKE_OSX_DEPLOYMENT_TARGET:STRING=10.8 ..
make -j4
sudo make install

cat /Users/distiller/project/build/CMakeFiles/CMakeOutput.log

cat /Users/distiller/project/build/CMakeFiles/CMakeError.log
