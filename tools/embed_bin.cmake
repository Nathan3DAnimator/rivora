# cmake -DIN=<file> -DOUT=<file.inc> -DNAME=<array> -P embed_bin.cmake
# Writes the bytes of IN as "static const unsigned char NAME[] = { ... };" (the branding's pictures: src/brand.c)
file(READ "${IN}" HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," BYTES "${HEX}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n" BYTES "${BYTES}")
file(WRITE "${OUT}" "static const unsigned char ${NAME}[] = {\n${BYTES}\n};\n")
