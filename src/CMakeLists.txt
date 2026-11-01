file(GLOB_RECURSE SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
list(FILTER SOURCES EXCLUDE REGEX ".*main\\.cpp$")

add_library(my_llvm_c_lib ${SOURCES})

target_include_directories(my_llvm_c_lib PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# mimalloc is the process allocator (JIT/tests) and is linked into programs
# produced by the driver. PUBLIC keeps the allocator available to the tests and
# to every consumer of the compiler library.
target_link_libraries(my_llvm_c_lib PUBLIC mimalloc-static)
target_include_directories(my_llvm_c_lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/thirdparty/mimalloc/include>
)

find_package(spdlog CONFIG QUIET)
if(spdlog_FOUND)
    target_link_libraries(my_llvm_c_lib PUBLIC spdlog::spdlog)
else()
    include(FetchContent)
    FetchContent_Declare(spdlog
        URL https://github.com/gabime/spdlog/archive/refs/tags/v1.14.1.tar.gz)
    FetchContent_MakeAvailable(spdlog)
    target_link_libraries(my_llvm_c_lib PUBLIC spdlog::spdlog)
endif()

find_package(cxxopts CONFIG QUIET)
if(cxxopts_FOUND)
    target_link_libraries(my_llvm_c_lib PUBLIC cxxopts::cxxopts)
else()
    include(FetchContent)
    FetchContent_Declare(cxxopts
        URL https://github.com/jarro2783/cxxopts/archive/refs/tags/v3.3.1.tar.gz)
    FetchContent_MakeAvailable(cxxopts)
    target_link_libraries(my_llvm_c_lib PUBLIC cxxopts::cxxopts)
endif()

if(LLVM_LINK_LLVM_DYLIB)
    target_link_libraries(my_llvm_c_lib PUBLIC LLVM)
else()
    llvm_map_components_to_libnames(llvm_libs
        core
        support
        irreader
        bitreader
        bitwriter
        analysis
        transformutils
        instcombine
        scalaropts
        passes
        orcjit
        target
        mc
        mcparser
        mcdisassembler
        x86asmparser
        x86codegen
        x86desc
        x86info
        x86disassembler
    )
    target_link_libraries(my_llvm_c_lib PUBLIC ${llvm_libs})
endif()

target_compile_definitions(my_llvm_c_lib PRIVATE
    RESOURCE_DIR="${CMAKE_SOURCE_DIR}/resources"
    STD_DIR="${CMAKE_SOURCE_DIR}/libs"
    SMC_INSTALL_STD_DIR="${CMAKE_INSTALL_FULL_DATADIR}/smc/libs"
    MIMALLOC_LIB_DIR="${CMAKE_BINARY_DIR}/lib"
    SMC_INSTALL_MIMALLOC_DIR="${CMAKE_INSTALL_FULL_LIBDIR}"
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(my_llvm_c_lib PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG)
else()
    target_compile_definitions(my_llvm_c_lib PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO)
endif()

add_executable(my_llvm_c main.cpp)
set_target_properties(my_llvm_c PROPERTIES ENABLE_EXPORTS ON)
target_include_directories(my_llvm_c PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(my_llvm_c PRIVATE my_llvm_c_lib)
target_compile_definitions(my_llvm_c PRIVATE
    RESOURCE_DIR="${CMAKE_SOURCE_DIR}/resources"
    STD_DIR="${CMAKE_SOURCE_DIR}/libs"
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(my_llvm_c PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG)
else()
    target_compile_definitions(my_llvm_c PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO)
endif()

add_custom_target(copy_compile_commands ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_BINARY_DIR}/compile_commands.json"
        "${CMAKE_SOURCE_DIR}/compile_commands.json"
    COMMENT "Copying compile_commands.json to source directory"
)