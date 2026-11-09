# SPDX-License-Identifier: MIT
# cmake -DIN=<file> -DOUT=<header> -DNAME=<symbol> -P embed.cmake
# Writes `static const unsigned char NAME[] = {...}; static const unsigned long NAME_size`.
file(READ ${IN} hex HEX)
string(LENGTH "${hex}" hexLen)
math(EXPR size "${hexLen} / 2")
string(REGEX REPLACE "(([0-9a-f][0-9a-f]){24})" "\\1\n" hex "${hex}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
file(WRITE ${OUT} "// Generated from ${IN} by embed.cmake. Do not edit.\nstatic const unsigned char ${NAME}[] = {\n${hex}\n};\nstatic const unsigned long ${NAME}_size = ${size}ul;\n")
