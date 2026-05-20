#!/usr/bin/env bash
set -e

cd "$(dirname "$0")"
rm -rf build

case "$1" in
    lsp)
        rm -rf compile_commands.json
        cmake -B build -G Ninja -Wno-dev \
            -DCMAKE_CXX_COMPILER=clang++ \
            -DCMAKE_BUILD_TYPE=Debug \
            -DCMAKE_CXX_FLAGS="-stdlib=libc++ -Wno-reserved-module-identifier" \
            -DCMAKE_EXE_LINKER_FLAGS="-stdlib=libc++"
        ln -s build/compile_commands.json ./
        ;;

    release)
        cmake -B build -G Ninja -Wno-dev \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON
        ;;

    *)
        echo -e "Invalid arg, options:\n\nlsp) Builds with clang, to get clangd lsp support\nrelease) Builds with system default & LTO\n\neg. gen.sh lsp"
        ;;
esac
