#!/bin/sh
mkdir -p bin
gcc src/*.c -Wall -O2 -s -o bin/xpic $(pkg-config --libs x11 xext xcomposite libpng)
