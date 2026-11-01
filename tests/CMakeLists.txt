find_package(GTest QUIET)
if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
    )
    FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)

file(GLOB_RECURSE TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(compiler_tests ${TEST_SOURCES})
# LLJIT tests resolve std.mem's mimalloc entry points from the process.
set_target_properties(compiler_tests PROPERTIES ENABLE_EXPORTS ON)

target_include_directories(compiler_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_definitions(compiler_tests PRIVATE
    TEST_INPUT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources/test_input"
    E2E_RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/e2e/resources"
    SNAPSHOT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/diagnostics/snapshots"
    STD_DIR="${CMAKE_SOURCE_DIR}/libs"
    SPEC_DIR="${CMAKE_SOURCE_DIR}/docs/spec"
)

target_link_libraries(compiler_tests
    PRIVATE
        my_llvm_c_lib
        GTest::gtest_main
)

# P1-04: 系统 GTest（无 gmock 库）时跳过——测试源未使用 gmock；
# FetchContent 路径下 gmock 可用，保持链接。
if(TARGET GTest::gmock)
    target_link_libraries(compiler_tests PRIVATE GTest::gmock)
endif()

gtest_discover_tests(compiler_tests)