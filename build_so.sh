#!/bin/bash
clang -g -shared -fpic -nostartfiles -ldl feature.c -O0 -o code/feature.so
