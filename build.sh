#!/bin/sh
gcc src/*.c -Wall -O2 -s -o bin/main $(pkg-config --libs x11 xext libpng)
