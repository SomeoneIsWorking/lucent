cmake_minimum_required(VERSION 3.16)
project(lucent VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(lucent STATIC
  src/config.cpp
  src/log.cpp
)
add_library(lucent::lucent ALIAS lucent)

target_include_directories(lucent PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(lucent PUBLIC cxx_std_20)

if(MSVC)
  target_compile_options(lucent PRIVATE /W4)
else()
  target_compile_options(lucent PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Tests are for developing lucent itself; a consumer adding this via add_subdirectory or FetchContent
# does not build them.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  option(LUCENT_BUILD_TESTS "Build lucent's tests" ON)
  if(LUCENT_BUILD_TESTS)
    enable_testing()
    add_executable(lucent_tests tests/test_lucent.cpp)
    # The Channel tests spawn threads to prove a handle tracks the enabled set under concurrency.
    find_package(Threads REQUIRED)
    target_link_libraries(lucent_tests PRIVATE lucent Threads::Threads)
    add_test(NAME lucent_tests COMMAND lucent_tests)
  endif()
endif()

include(GNUInstallDirs)
install(TARGETS lucent EXPORT lucentTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT lucentTargets
        FILE lucentTargets.cmake
        NAMESPACE lucent::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lucent)
