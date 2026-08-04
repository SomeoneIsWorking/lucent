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

# The two variables lucent reads for ITSELF — which debug channels are on, and where output goes.
# A consumer whose users already type another name sets these before add_subdirectory/FetchContent:
#
#     set(LUCENT_CHANNEL_ENV  "PSXPORT_DEBUG")
#     set(LUCENT_LOG_FILE_ENV "PSXPORT_LOG_FILE")
#     add_subdirectory(vendor/lucent)
#
# Build-time and not a runtime call ON PURPOSE: a setter has to be invoked by something, and whatever
# logs before it runs is silently dropped. Baked in, the very first lucent::debug() in the process —
# including one from a static initialiser — already reads the right variable.
foreach(_lucent_env LUCENT_CHANNEL_ENV LUCENT_LOG_FILE_ENV)
  if(${_lucent_env})
    target_compile_definitions(lucent PRIVATE ${_lucent_env}="${${_lucent_env}}")
  endif()
endforeach()

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

    # A SECOND binary, compiled from lucent's sources with the consumer-name macros set, because
    # that is the thing under test: `lucent` itself is built with the defaults, and a compile-time
    # name cannot be exercised by linking against a library that did not get it.
    add_executable(lucent_env_name_tests
      tests/test_channel_env_name.cpp src/config.cpp src/log.cpp)
    target_include_directories(lucent_env_name_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(lucent_env_name_tests PRIVATE
      LUCENT_CHANNEL_ENV="MYAPP_DEBUG" LUCENT_LOG_FILE_ENV="MYAPP_LOG_FILE")
    add_test(NAME lucent_env_name_tests COMMAND lucent_env_name_tests)
    # Set in the ENVIRONMENT, not by the test, on purpose: the call under test happens before main(),
    # so there is no point at which the test could have set it itself.
    set_tests_properties(lucent_env_name_tests PROPERTIES ENVIRONMENT "MYAPP_DEBUG=early")
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
