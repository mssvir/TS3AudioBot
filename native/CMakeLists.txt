cmake_minimum_required(VERSION 3.20)

project(mssv_native_process_supervision LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MSSV_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_library(mssv_process_supervision STATIC
    src/process_spawn_error.cpp
    src/spawn_admission_gate.cpp
)

target_include_directories(mssv_process_supervision
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(mssv_process_supervision PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(MSSV_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(mssv_process_supervision PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(mssv_process_supervision PRIVATE -fsanitize=address,undefined)
endif()

enable_testing()

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/../conformance/process_supervision_v1.tsv
    ${CMAKE_CURRENT_BINARY_DIR}/process_supervision_v1.tsv
    COPYONLY
)

add_executable(mssv_process_supervision_tests
    tests/process_supervision_tests.cpp
)

target_link_libraries(mssv_process_supervision_tests PRIVATE mssv_process_supervision)

add_executable(mssv_process_supervision_conformance_tests
    tests/process_supervision_conformance_tests.cpp
)

target_link_libraries(mssv_process_supervision_conformance_tests PRIVATE mssv_process_supervision)

foreach(test_target IN ITEMS mssv_process_supervision_tests mssv_process_supervision_conformance_tests)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${test_target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()

    if(MSSV_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${test_target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${test_target} PRIVATE -fsanitize=address,undefined)
    endif()
endforeach()

add_test(NAME mssv_process_supervision_tests COMMAND mssv_process_supervision_tests)
add_test(NAME mssv_process_supervision_conformance_tests COMMAND mssv_process_supervision_conformance_tests)
