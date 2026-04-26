cmake_minimum_required(VERSION 3.20)
project(guided_executor CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ---------------------------------------------------------------------------
# Zydis — x86 instruction decoder
# ---------------------------------------------------------------------------
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
# SDL3 — cross-platform windowing, input, and Vulkan surface
# ---------------------------------------------------------------------------
FetchContent_Declare(sdl3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.2.14
    GIT_SHALLOW    TRUE
)
set(SDL_SHARED  OFF CACHE BOOL "" FORCE)
set(SDL_STATIC  ON  CACHE BOOL "" FORCE)
set(SDL_TEST    OFF CACHE BOOL "" FORCE)
set(SDL_VULKAN OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(sdl3)

# ---------------------------------------------------------------------------
# Dear ImGui — immediate-mode debug/UI overlay
# ---------------------------------------------------------------------------
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.8
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

# ---------------------------------------------------------------------------
# Vulkan-Headers + volk — Vulkan without requiring the SDK installed.
# volk dynamically loads vulkan-1.dll at runtime.
# ---------------------------------------------------------------------------
FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG        vulkan-sdk-1.4.313.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(vulkan_headers)

FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG        vulkan-sdk-1.4.313.0
    GIT_SHALLOW    TRUE
)
set(VOLK_HEADERS_ONLY OFF CACHE BOOL "" FORCE)
set(VOLK_INSTALL      OFF CACHE BOOL "" FORCE)
# Point volk at our fetched Vulkan headers
set(VULKAN_HEADERS_INSTALL_DIR "${vulkan_headers_SOURCE_DIR}" CACHE PATH "" FORCE)
FetchContent_MakeAvailable(volk)

# Build ImGui as a static library with SDL3 + Vulkan backends
set(IMGUI_DIR ${imgui_SOURCE_DIR})
add_library(imgui_lib STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
)
target_include_directories(imgui_lib PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(imgui_lib PUBLIC SDL3::SDL3-static Vulkan::Headers volk)
# Tell ImGui's Vulkan backend to use volk for function loading
target_compile_definitions(imgui_lib PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK)


# ---------------------------------------------------------------------------
# Common source files (shared between executables)
# ---------------------------------------------------------------------------
set(COMMON_SOURCES
    src/cpu/trace_builder.cpp
    src/cpu/executor.cpp
    src/cpu/jit_helpers.cpp
    src/cpu/privileged.cpp
    src/cpu/veh_handler.cpp
)

if(MSVC)
    enable_language(ASM_MASM)
    list(APPEND COMMON_SOURCES src/cpu/dispatch_trace.asm)
endif()

# ---------------------------------------------------------------------------
# Test: basic inline tests (formerly main.cpp — byte-array self-tests)
# ---------------------------------------------------------------------------
add_executable(test_basic ${COMMON_SOURCES} src/tests/test_basic.cpp)
target_include_directories(test_basic PRIVATE src src/cpu)
target_link_libraries(test_basic PRIVATE Zydis)

if(MSVC)
    target_compile_options(test_basic PRIVATE /W3)
else()
    target_compile_options(test_basic PRIVATE -Wall -Wextra -O2)
    if(UNIX)
        target_link_libraries(test_basic PRIVATE m)
    endif()
endif()

# ---------------------------------------------------------------------------
# Main executable — Xermu emulator with SDL3/ImGui/Vulkan UI
# ---------------------------------------------------------------------------
add_executable(xermu ${COMMON_SOURCES} src/main.cpp)
target_include_directories(xermu PRIVATE src src/cpu)
target_link_libraries(xermu PRIVATE Zydis imgui_lib SDL3::SDL3-static volk)

if(MSVC)
    target_compile_options(xermu PRIVATE /W3)
else()
    target_compile_options(xermu PRIVATE -Wall -Wextra -O2)
    if(UNIX)
        target_link_libraries(xermu PRIVATE m dl pthread)
    endif()
endif()

# ---------------------------------------------------------------------------
# Test runner executable (loads flat 32-bit .bin files)
# ---------------------------------------------------------------------------
add_executable(test_runner ${COMMON_SOURCES} src/tests/test_runner.cpp)
target_include_directories(test_runner PRIVATE src src/cpu)
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
# PE loader unit test (standalone — no JIT, no Zydis)
# ---------------------------------------------------------------------------
add_executable(test_pe_loader src/tests/test_pe_loader.cpp)
target_include_directories(test_pe_loader PRIVATE src src/cpu)
if(MSVC)
    target_compile_options(test_pe_loader PRIVATE /W3)
else()
    target_compile_options(test_pe_loader PRIVATE -Wall -Wextra -O2)
endif()

# ---------------------------------------------------------------------------
# PGRAPH state shadow unit test (standalone — no JIT, no Zydis)
# ---------------------------------------------------------------------------
add_executable(test_pgraph src/tests/test_pgraph.cpp)
target_include_directories(test_pgraph PRIVATE src src/cpu)
if(MSVC)
    target_compile_options(test_pgraph PRIVATE /W3)
else()
    target_compile_options(test_pgraph PRIVATE -Wall -Wextra -O2)
endif()

# ---------------------------------------------------------------------------
# NASM test binary assembly (optional — only if NASM is installed)
# ---------------------------------------------------------------------------
find_program(NASM_EXECUTABLE nasm)

set(TEST_ASM_FILES
    src/tests/alu.asm
    src/tests/memory.asm
    src/tests/flow.asm
    src/tests/fpu.asm
    src/tests/sse.asm
    src/tests/advanced.asm
    src/tests/string.asm
    src/tests/segment.asm
    src/tests/indirect.asm
    src/tests/privileged.asm
    src/tests/misc.asm
    src/tests/smc.asm
    src/tests/interrupt.asm
    src/tests/paging.asm
    src/tests/xbox.asm
    src/tests/hle.asm
    src/tests/linking.asm
    src/tests/pic.asm
    src/tests/pit.asm
    src/tests/nv2a_timer.asm
    src/tests/pcrtc.asm
    src/tests/smbus.asm
    src/tests/nv2a_gpu.asm
    src/tests/esp_mem.asm
    src/tests/esp_lea.asm
    src/tests/apu.asm
    src/tests/ide.asm
    src/tests/usb.asm
    src/tests/mtrr.asm
    src/tests/gdt_tss.asm
    src/tests/flash.asm
    src/tests/pfifo.asm
    src/tests/ioapic.asm
    src/tests/pci.asm
    src/tests/pvideo.asm
    src/tests/pbus.asm
    src/tests/pramdac.asm
    src/tests/ptimer_alarm.asm
    src/tests/smbus_smc.asm
    src/tests/ide_dma.asm
    src/tests/usb_ohci.asm
    src/tests/flash_cmd.asm
    src/tests/apu_voice.asm
    src/tests/nv2a_pmc.asm
    src/tests/xbox_io.asm
    src/tests/fastmem.asm
    src/tests/smc_stress.asm
    src/tests/div_error.asm
    src/tests/mmio_alu.asm
    src/tests/ud2.asm
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
                    -I${CMAKE_CURRENT_SOURCE_DIR}/src/tests/
                    -o ${bin_file}
                    ${CMAKE_CURRENT_SOURCE_DIR}/${asm_file}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${asm_file}
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/tests/harness.inc
            COMMENT "Assembling ${asm_file} → ${test_name}.bin"
        )
        list(APPEND TEST_BINS ${bin_file})
    endforeach()

    add_custom_target(test_bins ALL DEPENDS ${TEST_BINS})

    # CTest integration
    enable_testing()

    # C++ unit tests (no NASM dependency)
    add_test(NAME pe_loader COMMAND test_pe_loader)
    add_test(NAME pgraph    COMMAND test_pgraph)

    foreach(asm_file ${TEST_ASM_FILES})
        get_filename_component(test_name ${asm_file} NAME_WE)
        set(bin_file ${CMAKE_CURRENT_BINARY_DIR}/tests/${test_name}.bin)
        if(test_name STREQUAL "xbox" OR test_name STREQUAL "hle" OR test_name STREQUAL "pic" OR test_name STREQUAL "pit" OR test_name STREQUAL "nv2a_timer" OR test_name STREQUAL "pcrtc" OR test_name STREQUAL "smbus" OR test_name STREQUAL "nv2a_gpu" OR test_name STREQUAL "apu" OR test_name STREQUAL "ide" OR test_name STREQUAL "usb" OR test_name STREQUAL "flash" OR test_name STREQUAL "pfifo" OR test_name STREQUAL "ioapic" OR test_name STREQUAL "pci" OR test_name STREQUAL "pvideo" OR test_name STREQUAL "pbus" OR test_name STREQUAL "pramdac" OR test_name STREQUAL "ptimer_alarm" OR test_name STREQUAL "smbus_smc" OR test_name STREQUAL "ide_dma" OR test_name STREQUAL "usb_ohci" OR test_name STREQUAL "flash_cmd" OR test_name STREQUAL "apu_voice" OR test_name STREQUAL "nv2a_pmc" OR test_name STREQUAL "xbox_io" OR test_name STREQUAL "fastmem")
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
