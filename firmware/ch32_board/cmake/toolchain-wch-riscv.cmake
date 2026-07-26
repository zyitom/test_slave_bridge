# Bare-metal RISC-V toolchain for the WCH CH32H417 (Qingke V5F core).
#
# The Qingke V5F implements RV32IMAFC plus WCH's proprietary "xw" compressed
# extension. We deliberately build WITHOUT xw so that a stock upstream
# riscv32-unknown-elf GCC can be used (the same multilib toolchain already used
# for rmcs_board), keeping the whole repo on one unified build. The cost is a
# small code-size increase; correctness is unaffected.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv32)

# CMake re-runs this file inside every try_compile sub-project, where -D cache
# entries from the outer configure are NOT visible. Without this the compiler
# probe would fail (or silently fall back to PATH) whenever the toolchain is
# selected with -DWCH_TOOLCHAIN_PATH= rather than the environment variable.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES WCH_TOOLCHAIN_PATH WCH_TOOLCHAIN_PREFIX)

# Locate the cross toolchain. Priority:
#   1. -DWCH_TOOLCHAIN_PATH=<dir with bin/>
#   2. environment WCH_TOOLCHAIN_PATH
#   3. environment GNURISCV_TOOLCHAIN_PATH (repo convention, see rmcs_board)
#   4. whatever is on PATH
if(NOT WCH_TOOLCHAIN_PATH AND DEFINED ENV{WCH_TOOLCHAIN_PATH})
    set(WCH_TOOLCHAIN_PATH "$ENV{WCH_TOOLCHAIN_PATH}")
endif()
if(NOT WCH_TOOLCHAIN_PATH AND DEFINED ENV{GNURISCV_TOOLCHAIN_PATH})
    file(
        GLOB _wch_tc_dirs
        "$ENV{GNURISCV_TOOLCHAIN_PATH}/rv32*/bin"
        "$ENV{GNURISCV_TOOLCHAIN_PATH}/bin"
    )
    if(_wch_tc_dirs)
        list(GET _wch_tc_dirs 0 _wch_tc_bin)
        cmake_path(GET _wch_tc_bin PARENT_PATH WCH_TOOLCHAIN_PATH)
    endif()
endif()

# The tool prefix is not fixed: a stock upstream build uses riscv32-unknown-elf-,
# while MounRiver's MRS_Toolchain ships several GCCs side by side under
# Toolchain/, each with its own prefix -- "RISC-V Embedded GCC15" is
# riscv32-wch-elf- and "RISC-V Embedded GCC12" is riscv-wch-elf-. Probe instead
# of hardcoding, and let -DWCH_TOOLCHAIN_PREFIX= force one.
#
# The MRS "RISC-V Embedded GCC" (no suffix, riscv-none-embed-) is deliberately
# absent from this list: it is GCC 8.2, which cannot compile the C++23 this
# repository requires.
if(NOT WCH_TOOLCHAIN_PREFIX AND DEFINED ENV{WCH_TOOLCHAIN_PREFIX})
    set(WCH_TOOLCHAIN_PREFIX "$ENV{WCH_TOOLCHAIN_PREFIX}")
endif()

if(WCH_TOOLCHAIN_PREFIX)
    set(_tc_prefix_candidates "${WCH_TOOLCHAIN_PREFIX}")
else()
    set(_tc_prefix_candidates "riscv32-wch-elf-" "riscv-wch-elf-" "riscv32-unknown-elf-")
endif()

set(_tc_prefix "")
if(WCH_TOOLCHAIN_PATH)
    set(_tc_bin "${WCH_TOOLCHAIN_PATH}/bin")
    foreach(_candidate IN LISTS _tc_prefix_candidates)
        if(EXISTS "${_tc_bin}/${_candidate}gcc")
            set(_tc_prefix "${_candidate}")
            break()
        endif()
    endforeach()
    if(NOT _tc_prefix)
        message(
            FATAL_ERROR
            "No RISC-V GCC found under ${_tc_bin}. Tried prefixes: "
            "${_tc_prefix_candidates}. Point WCH_TOOLCHAIN_PATH at the directory "
            "that CONTAINS bin/ (for MounRiver that is one of the per-version "
            "directories under MRS_Toolchain_*/Toolchain/, not Toolchain/ itself), "
            "or set WCH_TOOLCHAIN_PREFIX explicitly."
        )
    endif()
    set(CMAKE_C_COMPILER   "${_tc_bin}/${_tc_prefix}gcc")
    set(CMAKE_CXX_COMPILER "${_tc_bin}/${_tc_prefix}g++")
    set(CMAKE_ASM_COMPILER "${_tc_bin}/${_tc_prefix}gcc")
    set(CMAKE_OBJCOPY      "${_tc_bin}/${_tc_prefix}objcopy" CACHE FILEPATH "")
    set(CMAKE_SIZE         "${_tc_bin}/${_tc_prefix}size"    CACHE FILEPATH "")
else()
    foreach(_candidate IN LISTS _tc_prefix_candidates)
        find_program(_wch_tc_gcc "${_candidate}gcc")
        if(_wch_tc_gcc)
            set(_tc_prefix "${_candidate}")
            break()
        endif()
    endforeach()
    if(NOT _tc_prefix)
        message(
            FATAL_ERROR
            "No RISC-V GCC on PATH. Tried: ${_tc_prefix_candidates}. Set "
            "WCH_TOOLCHAIN_PATH to a toolchain root containing bin/."
        )
    endif()
    set(CMAKE_C_COMPILER   "${_tc_prefix}gcc")
    set(CMAKE_CXX_COMPILER "${_tc_prefix}g++")
    set(CMAKE_ASM_COMPILER "${_tc_prefix}gcc")
    set(CMAKE_OBJCOPY      "${_tc_prefix}objcopy" CACHE FILEPATH "")
    set(CMAKE_SIZE         "${_tc_prefix}size"    CACHE FILEPATH "")
endif()

message(STATUS "ch32_board RISC-V toolchain: ${CMAKE_C_COMPILER}")

set(WCH_ARCH_FLAGS "-march=rv32imafc_zicsr_zifencei -mabi=ilp32f -mcmodel=medany")

set(CMAKE_C_FLAGS_INIT   "${WCH_ARCH_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${WCH_ARCH_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${WCH_ARCH_FLAGS} -x assembler-with-cpp")

# Do not try to link a full hosted test program during compiler detection.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
