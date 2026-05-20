#!/usr/bin/env bash
set -e

cd "$(dirname "$0")"
cmake --build build
./build/userspawn

