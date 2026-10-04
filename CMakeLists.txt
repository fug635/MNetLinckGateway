cmake_minimum_required(VERSION 3.16)

project(MNetGateway VERSION 0.1 LANGUAGES C CXX)

# Шлюз MNet: переносимый узел MNode (core, чистый C без ОС и malloc), приложение mnetgw
# (COM и TCP на Win32 или POSIX) и тесты. Dbg подключает этот каталог через add_subdirectory
# и берёт только библиотеку MNode.

set(MNETGW_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(MNETGW_TOP_LEVEL ON)
endif()

option(MNETGW_BUILD_APP "Собирать приложение mnetgw" ${MNETGW_TOP_LEVEL})
option(MNETGW_BUILD_TESTS "Собирать тесты узла" ${MNETGW_TOP_LEVEL})

# Формат пакета (MNet.h) и разбор потока (MNetCodec.h) — общий код ядра; своей копии нет.
if(WIN32)
    set(MNETGW_DEFAULT_COMMON_INC "Z:/home/roman/MurphyKernel_v2/src/CommonInc")
else()
    set(MNETGW_DEFAULT_COMMON_INC "/home/roman/MurphyKernel_v2/src/CommonInc")
endif()
set(MURPHY_COMMON_INC "${MNETGW_DEFAULT_COMMON_INC}" CACHE PATH "Каталог CommonInc из репозитория MurphyKernel_v2")
if(NOT EXISTS "${MURPHY_COMMON_INC}/MNetCodec.h")
    message(FATAL_ERROR "MNetCodec.h не найден в MURPHY_COMMON_INC=${MURPHY_COMMON_INC}")
endif()

add_library(MNode STATIC core/MNode.h core/MNode.c)
target_include_directories(MNode PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/core" "${MURPHY_COMMON_INC}")
set_target_properties(MNode PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(MNode PRIVATE -Wall -Wextra)
endif()

if(MNETGW_BUILD_APP)
    if(WIN32)
        add_executable(mnetgw app/mnetgw.c app/platform.h app/platform_win.c)
        target_link_libraries(mnetgw PRIVATE MNode ws2_32)
    else()
        add_executable(mnetgw app/mnetgw.c app/platform.h app/platform_posix.c)
        target_link_libraries(mnetgw PRIVATE MNode)
    endif()
    set_target_properties(mnetgw PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    install(TARGETS mnetgw RUNTIME DESTINATION bin)
endif()

if(MNETGW_BUILD_TESTS)
    enable_testing()
    add_executable(test_mnode tests/test_mnode.cpp)
    target_link_libraries(test_mnode PRIVATE MNode)
    set_target_properties(test_mnode PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_test(NAME test_mnode COMMAND test_mnode)
endif()
