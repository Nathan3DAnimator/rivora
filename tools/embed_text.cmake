# cmake -DIN=<file> -DOUT=<file.inc> -P embed_text.cmake
# Writes the bytes of IN as a C initializer list ending in 0x00 (the manual's text, included by src/docs.c).
file(READ "${IN}" HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," BYTES "${HEX}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n" BYTES "${BYTES}")
file(WRITE "${OUT}" "${BYTES}0x00\n")
