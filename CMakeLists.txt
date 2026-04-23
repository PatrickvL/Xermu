cmake_minimum_required(VERSION 3.20)
project(xbox_guided_executor CXX)
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

add_executable(xbox_executor
    src/trace_builder.cpp
    src/executor.cpp
    src/main.cpp
)
target_include_directories(xbox_executor PRIVATE src)
target_link_libraries(xbox_executor PRIVATE Zydis)

if(MSVC)
    message(WARNING "MSVC: dispatch_trace() trampoline is GCC/Clang-only. "
                    "Provide a MASM .asm with the same logic for MSVC builds.")
    target_compile_options(xbox_executor PRIVATE /W3)
else()
    target_compile_options(xbox_executor PRIVATE -Wall -Wextra -O2)
    if(UNIX)
        target_link_libraries(xbox_executor PRIVATE m)
    endif()
endif()
