#!/bin/sh
# Tests src/port/bigalloc.c against a heap that loses big blocks as Circle's does.
set -e
cd "$(dirname "$0")"
mkdir -p build
cc -O1 -g -Wall -Wextra -o build/test_bigalloc test_bigalloc.c ../../src/port/bigalloc.c
./build/test_bigalloc
