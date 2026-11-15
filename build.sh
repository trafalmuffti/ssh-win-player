#!/bin/sh
# Cross-compile SshWinPlayer.exe on Linux with mingw-w64.
#   apt-get install g++-mingw-w64-x86-64-posix
# Output: build/SshWinPlayer.exe  (needs libmpv-2.dll next to it at runtime)
set -e
cd "$(dirname "$0")"
CXX=${CXX:-x86_64-w64-mingw32-g++}
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
mkdir -p build
$WINDRES -I res res/app.rc -O coff -o build/app.res.o
$CXX -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-cast-function-type \
    -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0A00 \
    -Ithird_party/mpv-include \
    src/main.cpp src/sshconn.cpp build/app.res.o \
    -o build/SshWinPlayer.exe \
    -municode -mwindows -static -s \
    -lcomctl32 -lcomdlg32 -lshell32 -lshlwapi -lole32 -luuid -lgdi32
echo "built build/SshWinPlayer.exe"
