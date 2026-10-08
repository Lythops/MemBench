#!/usr/bin/env bash
# Build the ROM. Must run under MSYS bash (SDK mounted at /pyrite64-sdk):
#   /c/msys64/usr/bin/bash -lc "/c/Nintendo64/MemBench/build.sh"
cd /c/Nintendo64/MemBench || exit 1
make "$@"
