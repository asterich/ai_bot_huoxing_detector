cmake_minimum_required(VERSION 4.4.4)

# CMake 4.4's standard-library module support is experimental and version-gated.
# Keep this gate aligned with the CMake version pinned by Pixi.
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "f35a9ac6-8463-4d38-8eec-5d6008153e7d")
project(huoxing VERSION 0.2.0 LANGUAGES CXX)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16.1)
    message(FATAL_ERROR "GCC >= 16.1 is required")
  endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 21)
    message(FATAL_ERROR "Clang >= 21 is required for the tested C++26 modules baseline")
  endif()
else()
  message(FATAL_ERROR "Use GCC >=16.1 or Clang >=21")
endif()
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_CXX_MODULE_STD ON)
if(NOT 26 IN_LIST CMAKE_CXX_COMPILER_IMPORT_STD)
  message(FATAL_ERROR "The selected toolchain does not provide a C++26 std module")
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
find_package(OpenCV 4 REQUIRED COMPONENTS core imgproc features2d calib3d imgcodecs)
find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(fmt 11 CONFIG REQUIRED)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/schema.sql" HUOXING_SCHEMA)
configure_file(src/schema.hpp.in generated/schema.hpp @ONLY)
add_library(huoxing_core)
target_sources(huoxing_core PUBLIC FILE_SET CXX_MODULES FILES
  src/modules/types.cppm
  src/modules/messages.cppm
  src/modules/repository.cppm
  src/modules/repository_sqlite.cppm
  src/modules/vision.cppm
  src/modules/format.cppm
  src/modules/hashing.cppm
  src/modules/sqlite.cppm
  src/modules/features.cppm
  src/modules/matching.cppm
  src/modules/database.cppm
  src/modules/detector.cppm
  src/modules/huoxing.cppm)
target_sources(huoxing_core PUBLIC FILE_SET CXX_MODULES FILES
  src/modules/forward_types.cppm
  src/modules/forward_database.cppm
  src/modules/forward.cppm)
target_include_directories(huoxing_core PUBLIC ${OpenCV_INCLUDE_DIRS} PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
if(TARGET SQLite3::SQLite3)
  set(HUOXING_SQLITE_TARGET SQLite3::SQLite3)
else()
  set(HUOXING_SQLITE_TARGET SQLite::SQLite3)
endif()
target_link_libraries(huoxing_core PUBLIC ${OpenCV_LIBS} ${HUOXING_SQLITE_TARGET} OpenSSL::Crypto fmt::fmt)
target_compile_options(huoxing_core PRIVATE -Wall -Wextra -Wpedantic)
option(HUOXING_PYTHON "Build Python binding" ON)
if(HUOXING_PYTHON)
  find_package(Python REQUIRED COMPONENTS Interpreter Development.Module)
  find_package(pybind11 CONFIG REQUIRED)
  pybind11_add_module(_native src/binding.cpp)
  target_link_libraries(_native PRIVATE huoxing_core)
  install(TARGETS _native LIBRARY DESTINATION huoxing RUNTIME DESTINATION huoxing)
endif()
include(CTest)
if(BUILD_TESTING)
  add_executable(core_tests tests/core_tests.cpp)
  target_link_libraries(core_tests PRIVATE huoxing_core Threads::Threads)
  add_test(NAME core_tests COMMAND core_tests)
endif()
