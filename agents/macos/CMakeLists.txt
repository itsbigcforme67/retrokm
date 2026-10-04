# Build with Retro68 (https://github.com/autc04/Retro68):
#   mkdir build && cd build
#   cmake .. -DCMAKE_TOOLCHAIN_FILE=<Retro68-build>/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
#   make
# Produces RetroKM.bin (MacBinary), RetroKM.dsk (floppy image) and RetroKM.APPL.
cmake_minimum_required(VERSION 3.5)
project(RetroKM C)

add_application(RetroKM
    rkm_mac.c
    ../../common/rkm_proto.c
    rkm_mac.r
    CREATOR "RKMa")
