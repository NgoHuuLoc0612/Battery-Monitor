cmake_minimum_required(VERSION 3.18)
project(battery_monitor VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── pybind11 ──────────────────────────────────────────────────────────────────
find_package(Python3 COMPONENTS Interpreter Development REQUIRED)
find_package(pybind11 REQUIRED)

# ── Source files ──────────────────────────────────────────────────────────────
set(CORE_SOURCES
    src/battery_engine.cpp
)
set(BINDING_SOURCES
    src/bindings.cpp
)

# ── pybind11 module ───────────────────────────────────────────────────────────
pybind11_add_module(battery_core
    ${CORE_SOURCES}
    ${BINDING_SOURCES}
)

target_include_directories(battery_core PRIVATE include)

# ── Windows libraries ────────────────────────────────────────────────────────
if(WIN32)
    target_link_libraries(battery_core PRIVATE
        wbemuuid
        setupapi
        powrprof
        pdh
        cfgmgr32
    )
    target_compile_definitions(battery_core PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0601
        UNICODE
        _UNICODE
    )
    # ── GCC/MinGW flags (replaces MSVC /W3 /O2 /GL /GR- /EHsc) ──────────────
    if(MINGW OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(battery_core PRIVATE
            -Wall
            -O2
            -fexceptions
        )
        # MinGW: link against uuid for wbemuuid, and ole32/oleaut32 for WMI/COM
        target_link_libraries(battery_core PRIVATE
            uuid
            ole32
            oleaut32
        )
    else()
        # MSVC fallback (kept for completeness)
        target_compile_options(battery_core PRIVATE
            /W3 /WX- /O2 /GL /GR- /EHsc
        )
        target_link_options(battery_core PRIVATE /LTCG)
    endif()
endif()

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS battery_core DESTINATION ${CMAKE_SOURCE_DIR}/python)

# ── Build info ───────────────────────────────────────────────────────────────
message(STATUS "Battery Monitor Engine")
message(STATUS "  Python:    ${Python3_VERSION}")
message(STATUS "  pybind11:  ${pybind11_VERSION}")
message(STATUS "  Compiler:  ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "  Platform:  ${CMAKE_SYSTEM_NAME}")
