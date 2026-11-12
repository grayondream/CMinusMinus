cmake_minimum_required(VERSION 3.20)

project(CDD LANGUAGES C CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

message("CMAKE_BUILD_TYPE: ${CMAKE_BUILD_TYPE}")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")

if(DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

if(LLVM_PACKAGE_VERSION VERSION_LESS 18)
    message(FATAL_ERROR "LLVM version must be >= 18, found ${LLVM_PACKAGE_VERSION}")
endif()

add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})

# Memory backend: the bundled mimalloc is the allocator for both the compiler
# and programs linked by it. Keep a deterministic static build (no shared
# library / upstream tests) and expose it as libmimalloc.a in build/lib.
set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(MI_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MI_OVERRIDE ON CACHE BOOL "" FORCE)
# Keep the allocator fast even in Debug compiler builds. Upstream's Debug
# defaults (guard pages, full stats/profiling, invariant checks) are unsuitable
# for a compiler runtime and dominated startup/alloc cost.
set(MI_DEBUG OFF CACHE STRING "" FORCE)
set(MI_SECURE OFF CACHE STRING "" FORCE)
set(MI_GUARDED OFF CACHE STRING "" FORCE)
set(MI_STATS OFF CACHE STRING "" FORCE)
set(MI_PROFILE OFF CACHE STRING "" FORCE)
set(MI_ALLOW_THP DEFAULT CACHE STRING "" FORCE)
add_subdirectory(thirdparty/mimalloc EXCLUDE_FROM_ALL)
set_target_properties(mimalloc-static PROPERTIES OUTPUT_NAME mimalloc)
# Upstream static builds use -fvisibility=hidden. The compiler's embedded JIT
# resolves mi_* from the process, so give the public mimalloc entry points
# default visibility (the executable additionally exports its dynamic symbols).
if(NOT MSVC)
    target_compile_options(mimalloc-static PRIVATE -fvisibility=default)
endif()

add_subdirectory(src)

option(BUILD_TESTS "Build unit tests" ON)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Stdlib and allocator are installed as data/runtime dependencies, not baked
# into the compiler binary.
install(TARGETS cdd mimalloc-static
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY libs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/cdd/libs
        FILES_MATCHING PATTERN "*.cdd")
