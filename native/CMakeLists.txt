cmake_minimum_required(VERSION 3.20)

project(mssv_native_process_supervision LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MSSV_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

set(MSSV_PROCESS_SUPERVISION_SOURCES
    src/process_spawn_error.cpp
    src/process_supervision_metrics.cpp
    src/process_supervisor.cpp
    src/spawn_admission_gate.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND MSSV_PROCESS_SUPERVISION_SOURCES src/linux_process_backend.cpp)
endif()

add_library(mssv_process_supervision STATIC ${MSSV_PROCESS_SUPERVISION_SOURCES})

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

add_executable(mssv_process_supervisor_failure_injection_tests
    tests/process_supervisor_failure_injection_tests.cpp
)

target_link_libraries(mssv_process_supervisor_failure_injection_tests PRIVATE mssv_process_supervision)

add_executable(mssv_process_supervision_metrics_tests
    tests/process_supervision_metrics_tests.cpp
)

target_link_libraries(mssv_process_supervision_metrics_tests PRIVATE mssv_process_supervision)

set(MSSV_TEST_TARGETS
    mssv_process_supervision_tests
    mssv_process_supervision_conformance_tests
    mssv_process_supervisor_failure_injection_tests
    mssv_process_supervision_metrics_tests
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(mssv_process_tree_fixture
        tests/process_tree_fixture.cpp
    )

    add_executable(mssv_linux_process_backend_integration_tests
        tests/linux_process_backend_integration_tests.cpp
    )

    target_link_libraries(mssv_linux_process_backend_integration_tests PRIVATE mssv_process_supervision)

    list(APPEND MSSV_TEST_TARGETS
        mssv_process_tree_fixture
        mssv_linux_process_backend_integration_tests
    )
endif()

foreach(test_target IN LISTS MSSV_TEST_TARGETS)
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
add_test(NAME mssv_process_supervisor_failure_injection_tests COMMAND mssv_process_supervisor_failure_injection_tests)
add_test(NAME mssv_process_supervision_metrics_tests COMMAND mssv_process_supervision_metrics_tests)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(
        NAME mssv_linux_process_backend_integration_tests
        COMMAND mssv_linux_process_backend_integration_tests $<TARGET_FILE:mssv_process_tree_fixture>
    )
endif()
