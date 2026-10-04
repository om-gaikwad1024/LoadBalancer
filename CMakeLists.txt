cmake_minimum_required(VERSION 3.25)

project(LoadBalancer VERSION 0.1.0 LANGUAGES C CXX)

if(NOT MSVC)
  message(FATAL_ERROR "This project targets MSVC on Windows only (plan II.5).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

# Plan IX fuzzing: MSVC libFuzzer + AddressSanitizer. Only the `fuzz` preset turns this on;
# it instruments every project target and builds lb_parser_fuzz.
option(LB_BUILD_FUZZ "Build the parser fuzz target with ASan + libFuzzer" OFF)
if(LB_BUILD_FUZZ)
  add_compile_options(/fsanitize=address)
endif()

find_package(nlohmann_json CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)

enable_testing()

add_subdirectory(src/engine)
add_subdirectory(src/app)
add_subdirectory(tools/mock_backend)
add_subdirectory(tests)

include(EngineNoMfc)
lb_check_engine_has_no_mfc(lb_engine "${CMAKE_CURRENT_SOURCE_DIR}/src/engine")
