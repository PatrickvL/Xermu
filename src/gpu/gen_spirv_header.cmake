# gen_spirv_header.cmake — Convert a SPIR-V binary to a C uint32_t array header.
# Usage: cmake -DSPV_FILE=path/to/shader.spv -DHEADER_FILE=path/to/output.h
#              -DARRAY_NAME=shader_name -P gen_spirv_header.cmake

file(READ "${SPV_FILE}" SPV_DATA HEX)
string(LENGTH "${SPV_DATA}" SPV_HEX_LEN)
math(EXPR SPV_BYTE_COUNT "${SPV_HEX_LEN} / 2")
math(EXPR SPV_WORD_COUNT "${SPV_BYTE_COUNT} / 4")

# Build uint32_t array from little-endian hex pairs
set(ARRAY_BODY "")
set(COL 0)
math(EXPR LAST_WORD "${SPV_WORD_COUNT} - 1")
foreach(I RANGE 0 ${LAST_WORD})
    math(EXPR BYTE0_POS "${I} * 8 + 0")
    math(EXPR BYTE1_POS "${I} * 8 + 2")
    math(EXPR BYTE2_POS "${I} * 8 + 4")
    math(EXPR BYTE3_POS "${I} * 8 + 6")
    string(SUBSTRING "${SPV_DATA}" ${BYTE0_POS} 2 B0)
    string(SUBSTRING "${SPV_DATA}" ${BYTE1_POS} 2 B1)
    string(SUBSTRING "${SPV_DATA}" ${BYTE2_POS} 2 B2)
    string(SUBSTRING "${SPV_DATA}" ${BYTE3_POS} 2 B3)
    # SPIR-V is little-endian: byte0 is LSB
    string(APPEND ARRAY_BODY "0x${B3}${B2}${B1}${B0}u,")
    math(EXPR COL "${COL} + 1")
    if(COL EQUAL 8)
        string(APPEND ARRAY_BODY "\n    ")
        set(COL 0)
    endif()
endforeach()

file(WRITE "${HEADER_FILE}"
"// Auto-generated from ${SPV_FILE} — do not edit.\n"
"#pragma once\n"
"#include <cstdint>\n"
"static const uint32_t ${ARRAY_NAME}[] = {\n"
"    ${ARRAY_BODY}\n"
"};\n"
"static const uint32_t ${ARRAY_NAME}_size = ${SPV_WORD_COUNT};\n"
)
