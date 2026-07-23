# Bare-metal RISC-V toolchain for the WCH CH32H417 (Qingke V5F core).
#
# The Qingke V5F implements RV32IMAFC plus WCH's proprietary "xw" compressed
# extension. We deliberately build WITHOUT xw so that a stock upstream
# riscv32-unknown-elf GCC can be used (the same multilib toolchain already used
# for rmcs_board), keeping the whole repo on one unified build. The cost is a
# small code-size increase; correctness is unaffected.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv32)

# Locate the cross toolchain. Priority:
#   1. -DWCH_TOOLCHAIN_PATH=<dir with bin/>
#   2. environment GNURISCV_TOOLCHAIN_PATH (repo convention, see rmcs_board)
#   3. plain riscv32-unknown-elf-gcc on PATH
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

set(_tc_prefix "riscv32-unknown-elf-")
if(WCH_TOOLCHAIN_PATH)
    set(_tc_bin "${WCH_TOOLCHAIN_PATH}/bin")
    set(CMAKE_C_COMPILER   "${_tc_bin}/${_tc_prefix}gcc")
    set(CMAKE_CXX_COMPILER "${_tc_bin}/${_tc_prefix}g++")
    set(CMAKE_ASM_COMPILER "${_tc_bin}/${_tc_prefix}gcc")
    set(CMAKE_OBJCOPY      "${_tc_bin}/${_tc_prefix}objcopy" CACHE FILEPATH "")
    set(CMAKE_SIZE         "${_tc_bin}/${_tc_prefix}size"    CACHE FILEPATH "")
else()
    set(CMAKE_C_COMPILER   "${_tc_prefix}gcc")
    set(CMAKE_CXX_COMPILER "${_tc_prefix}g++")
    set(CMAKE_ASM_COMPILER "${_tc_prefix}gcc")
    set(CMAKE_OBJCOPY      "${_tc_prefix}objcopy" CACHE FILEPATH "")
    set(CMAKE_SIZE         "${_tc_prefix}size"    CACHE FILEPATH "")
endif()

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
