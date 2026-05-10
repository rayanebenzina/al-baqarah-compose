# Reads a SPIR-V binary (multiple of 4 bytes) and writes a C++ header that
# embeds the bytecode as a `static const uint32_t <SYM>[]` array.
#
# Invoked from add_custom_command:
#   ${CMAKE_COMMAND} -DSRC=<spv> -DDST=<header> -DSYM=<name> -P embed_spirv.cmake

if(NOT SRC OR NOT DST OR NOT SYM)
    message(FATAL_ERROR "embed_spirv: SRC, DST, SYM required")
endif()

file(READ "${SRC}" HEX_DATA HEX)
string(LENGTH "${HEX_DATA}" HEX_LEN)
math(EXPR BYTES "${HEX_LEN} / 2")
math(EXPR WORDS "${BYTES} / 4")
math(EXPR MOD4 "${BYTES} % 4")
if(NOT MOD4 EQUAL 0)
    message(FATAL_ERROR "embed_spirv: ${SRC} is ${BYTES} bytes, not 4-aligned")
endif()

set(LINES "")
set(I 0)
set(LINE "")
set(PER_LINE 0)
while(I LESS HEX_LEN)
    string(SUBSTRING "${HEX_DATA}" ${I} 2 B0)
    math(EXPR I1 "${I} + 2")
    string(SUBSTRING "${HEX_DATA}" ${I1} 2 B1)
    math(EXPR I2 "${I} + 4")
    string(SUBSTRING "${HEX_DATA}" ${I2} 2 B2)
    math(EXPR I3 "${I} + 6")
    string(SUBSTRING "${HEX_DATA}" ${I3} 2 B3)
    if(LINE STREQUAL "")
        set(LINE "    0x${B3}${B2}${B1}${B0}")
    else()
        set(LINE "${LINE}, 0x${B3}${B2}${B1}${B0}")
    endif()
    math(EXPR PER_LINE "${PER_LINE} + 1")
    if(PER_LINE EQUAL 8)
        set(LINES "${LINES}${LINE},\n")
        set(LINE "")
        set(PER_LINE 0)
    endif()
    math(EXPR I "${I} + 8")
endwhile()
if(NOT LINE STREQUAL "")
    set(LINES "${LINES}${LINE}\n")
endif()

file(WRITE "${DST}"
"// Generated from ${SRC} — do not edit.
#pragma once
#include <cstdint>
#include <cstddef>
static const uint32_t ${SYM}[] = {
${LINES}};
static const size_t ${SYM}_SIZE = sizeof(${SYM});
")
