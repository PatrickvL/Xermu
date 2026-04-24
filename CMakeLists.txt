cmake_minimum_required(VERSION 3.20)
project(guided_executor CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(zydis
    GIT_REPOSITORY https://github.com/zyantific/zydis.git
    GIT_TAG        v4.1.0
    GIT_SHALLOW    TRUE
)
# Minimal build: no formatter, no encoder, no tools
# NOTE: ZYDIS_MINIMAL_MODE strips semantic info but keeps full operand decode
#       (base/index/scale/disp). If operand detail comes back empty, set OFF.
set(ZYDIS_BUILD_TOOLS       OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES    OFF CACHE BOOL "" FORCE)
set(ZYDIS_FEATURE_FORMATTER OFF CACHE BOOL "" FORCE)
set(ZYDIS_FEATURE_ENCODER   OFF CACHE BOOL "" FORCE)
set(ZYDIS_MINIMAL_MODE      OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(zydis)

# ---------------------------------------------------------------------------
# Common source files (shared between executables)
# ---------------------------------------------------------------------------
set(COMMON_SOURCES
    src/trace_builder.cpp
    src/executor.cpp
)

if(MSVC)
    enable_language(ASM_MASM)
    list(APPEND COMMON_SOURCES src/dispatch_trace.asm)
endif()

# ---------------------------------------------------------------------------
# Main executable (self-tests with embedded byte arrays)
# ---------------------------------------------------------------------------
add_executable(guided_executor ${COMMON_SOURCES} src/main.cpp)
target_include_directories(guided_executor PRIVATE src)
target_link_libraries(guided_executor PRIVATE Zydis)

if(MSVC)
    target_compile_options(guided_executor PRIVATE /W3)
else()
    target_compile_options(guided_executor PRIVATE -Wall -Wextra -O2)
    if(UNIX)
        target_link_libraries(guided_executor PRIVATE m)
    endif()
endif()

# ---------------------------------------------------------------------------
# Test runner executable (loads flat 32-bit .bin files)
# ---------------------------------------------------------------------------
add_executable(test_runner ${COMMON_SOURCES} src/test_runner.cpp)
target_include_directories(test_runner PRIVATE src)
target_link_libraries(test_runner PRIVATE Zydis)

if(MSVC)
    target_compile_options(test_runner PRIVATE /W3)
else()
    target_compile_options(test_runner PRIVATE -Wall -Wextra -O2)
    if(UNIX)
        target_link_libraries(test_runner PRIVATE m)
    endif()
endif()

# ---------------------------------------------------------------------------
# NASM test binary assembly (optional — only if NASM is installed)
# ---------------------------------------------------------------------------
find_program(NASM_EXECUTABLE nasm)

set(TEST_ASM_FILES
    tests/alu.asm
    tests/memory.asm
    tests/flow.asm
    tests/fpu.asm
    tests/sse.asm
    tests/advanced.asm
    tests/string.asm
    tests/segment.asm
    tests/indirect.asm
    tests/privileged.asm
    tests/misc.asm
    tests/smc.asm
    tests/interrupt.asm
    tests/paging.asm
    tests/xbox.asm
    tests/hle.asm
    tests/linking.asm
    tests/pic.asm
    tests/pit.asm
    tests/nv2a_timer.asm
    tests/pcrtc.asm
    tests/smbus.asm
)

if(NASM_EXECUTABLE)
    message(STATUS "Found NASM: ${NASM_EXECUTABLE}")

    set(TEST_BINS "")
    foreach(asm_file ${TEST_ASM_FILES})
        get_filename_component(test_name ${asm_file} NAME_WE)
        set(bin_file ${CMAKE_CURRENT_BINARY_DIR}/tests/${test_name}.bin)
        add_custom_command(
            OUTPUT  ${bin_file}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/tests
            COMMAND ${NASM_EXECUTABLE} -f bin
                    -I${CMAKE_CURRENT_SOURCE_DIR}/tests/
                    -o ${bin_file}
                    ${CMAKE_CURRENT_SOURCE_DIR}/${asm_file}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${asm_file}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness.inc
            COMMENT "Assembling ${asm_file} → ${test_name}.bin"
        )
        list(APPEND TEST_BINS ${bin_file})
    endforeach()

    add_custom_target(test_bins ALL DEPENDS ${TEST_BINS})

    # CTest integration
    enable_testing()
    foreach(asm_file ${TEST_ASM_FILES})
        get_filename_component(test_name ${asm_file} NAME_WE)
        set(bin_file ${CMAKE_CURRENT_BINARY_DIR}/tests/${test_name}.bin)
        if(test_name STREQUAL "xbox" OR test_name STREQUAL "hle" OR test_name STREQUAL "pic" OR test_name STREQUAL "pit" OR test_name STREQUAL "nv2a_timer" OR test_name STREQUAL "pcrtc" OR test_name STREQUAL "smbus")
            add_test(
                NAME    ${test_name}
                COMMAND test_runner --xbox ${bin_file}
            )
        else()
            add_test(
                NAME    ${test_name}
                COMMAND test_runner ${bin_file}
            )
        endif()
    endforeach()
else()
    message(WARNING "NASM not found — NASM test binaries will not be assembled. "
                    "Install NASM to enable: https://www.nasm.us/")
endif()
