cmake_minimum_required(VERSION 3.16)
project(lucent VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(LUCENT_FORMAT_BACKEND "auto" CACHE STRING "Logger formatter: auto, std, fmt, or none")
set_property(CACHE LUCENT_FORMAT_BACKEND PROPERTY STRINGS auto std fmt none)
if(LUCENT_FORMAT_BACKEND STREQUAL "fmt")
  find_package(fmt CONFIG REQUIRED)
elseif(NOT LUCENT_FORMAT_BACKEND STREQUAL "auto" AND
       NOT LUCENT_FORMAT_BACKEND STREQUAL "std" AND
       NOT LUCENT_FORMAT_BACKEND STREQUAL "none")
  message(FATAL_ERROR "LUCENT_FORMAT_BACKEND must be auto, std, fmt, or none")
endif()

add_library(lucent STATIC
  src/config.cpp
  src/content.cpp
  src/cvar.cpp
  src/http.cpp
  src/log.cpp
  src/log_c.cpp
  src/platform.cpp
  src/touch.cpp
)
add_library(lucent::lucent ALIAS lucent)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(_lucent_build_zip_default ON)
else()
  set(_lucent_build_zip_default OFF)
endif()
option(LUCENT_BUILD_ZIP "Build lucent's zlib-backed ZIP extraction target"
       ${_lucent_build_zip_default})
unset(_lucent_build_zip_default)

if(LUCENT_BUILD_ZIP)
  find_package(ZLIB REQUIRED)
  add_library(lucent_zip STATIC src/zip.cpp)
  add_library(lucent::zip ALIAS lucent_zip)
  target_link_libraries(lucent_zip PUBLIC ZLIB::ZLIB)
  target_include_directories(lucent_zip PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  )
  target_compile_features(lucent_zip PUBLIC cxx_std_20)
  set_target_properties(lucent_zip PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

find_package(Threads REQUIRED)
target_link_libraries(lucent PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(lucent PUBLIC ws2_32)
endif()
if(LUCENT_FORMAT_BACKEND STREQUAL "fmt")
  target_link_libraries(lucent PUBLIC fmt::fmt)
  target_compile_definitions(lucent PUBLIC LUCENT_USE_FMT=1 LUCENT_DISABLE_STD_FORMAT=1)
elseif(LUCENT_FORMAT_BACKEND STREQUAL "none")
  target_compile_definitions(lucent PUBLIC LUCENT_DISABLE_STD_FORMAT=1)
endif()

target_include_directories(lucent PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(lucent PUBLIC cxx_std_20)
set_target_properties(lucent PROPERTIES POSITION_INDEPENDENT_CODE ON)

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
    target_link_libraries(lucent_tests PRIVATE lucent Threads::Threads)
    add_test(NAME lucent_tests COMMAND lucent_tests)

    add_executable(lucent_http_tests tests/test_http.cpp)
    target_link_libraries(lucent_http_tests PRIVATE lucent Threads::Threads)
    add_test(NAME lucent_http_tests COMMAND lucent_http_tests)

    add_executable(lucent_touch_tests tests/test_touch.cpp)
    target_link_libraries(lucent_touch_tests PRIVATE lucent)
    add_test(NAME lucent_touch_tests COMMAND lucent_touch_tests)

    # Keep the Android-side contact lifetime state machine executable without requiring an Android
    # SDK, device, or SDL Java source tree. LucentActivity's MotionEvent adapter is compiled by an
    # Android consumer; this test owns the platform-independent state it delegates to.
    find_package(Java COMPONENTS Development QUIET)
    if(Java_JAVAC_EXECUTABLE)
      get_filename_component(_lucent_javac "${Java_JAVAC_EXECUTABLE}" REALPATH)
      get_filename_component(_lucent_java_bin "${_lucent_javac}" DIRECTORY)
      set(_lucent_java_runtime "${_lucent_java_bin}/java")
      if(NOT EXISTS "${_lucent_java_runtime}")
        message(FATAL_ERROR
          "Lucent found javac at ${Java_JAVAC_EXECUTABLE}, but its matching java runtime is missing")
      endif()
      set(_lucent_android_touch_classes "${CMAKE_CURRENT_BINARY_DIR}/android_touch_contacts")
      set(_lucent_android_touch_test
        "${CMAKE_CURRENT_SOURCE_DIR}/platforms/android/tests/io/github/someoneisworking/lucent/LucentTouchContactsTest.java")
      add_custom_command(
        OUTPUT "${_lucent_android_touch_classes}/io/github/someoneisworking/lucent/LucentTouchContactsTest.class"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_lucent_android_touch_classes}"
        COMMAND "${_lucent_javac}" --release 17 -d "${_lucent_android_touch_classes}"
          "${CMAKE_CURRENT_SOURCE_DIR}/platforms/android/java/io/github/someoneisworking/lucent/LucentTouchContacts.java"
          "${_lucent_android_touch_test}"
        DEPENDS
          "${CMAKE_CURRENT_SOURCE_DIR}/platforms/android/java/io/github/someoneisworking/lucent/LucentTouchContacts.java"
          "${_lucent_android_touch_test}"
        VERBATIM)
      add_custom_target(lucent_android_touch_contacts_classes ALL
        DEPENDS "${_lucent_android_touch_classes}/io/github/someoneisworking/lucent/LucentTouchContactsTest.class")
      add_test(NAME lucent_android_touch_contacts
        COMMAND "${_lucent_java_runtime}" -cp "${_lucent_android_touch_classes}"
          io.github.someoneisworking.lucent.LucentTouchContactsTest)
    endif()

    add_executable(lucent_platform_tests tests/test_platform.cpp)
    target_link_libraries(lucent_platform_tests PRIVATE lucent)
    add_test(NAME lucent_platform_tests COMMAND lucent_platform_tests)

    add_executable(lucent_cvar_tests tests/test_cvar.cpp)
    target_link_libraries(lucent_cvar_tests PRIVATE lucent)
    add_test(NAME lucent_cvar_tests COMMAND lucent_cvar_tests)

    add_executable(lucent_content_tests tests/test_content.cpp)
    target_link_libraries(lucent_content_tests PRIVATE lucent)
    add_test(NAME lucent_content_tests COMMAND lucent_content_tests
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sha256.txt")

    add_executable(lucent_core_portability_tests tests/test_core_portability.cpp)
    target_compile_definitions(lucent_core_portability_tests PRIVATE LUCENT_DISABLE_STD_FORMAT=1)
    target_link_libraries(lucent_core_portability_tests PRIVATE lucent)
    add_test(NAME lucent_core_portability_tests COMMAND lucent_core_portability_tests)

    if(TARGET lucent_zip)
      add_executable(lucent_zip_tests tests/test_zip.cpp)
      target_link_libraries(lucent_zip_tests PRIVATE lucent::zip)
      add_test(NAME lucent_zip_tests COMMAND lucent_zip_tests)
    endif()

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

    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME cpp_quality
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_cpp_quality.sh"
              "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME source_structure
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_source_structure.py")
    add_test(NAME source_structure_selftest
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_source_structure.py"
              --selftest)
  endif()
endif()

include(GNUInstallDirs)
install(TARGETS lucent EXPORT lucentTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(TARGET lucent_zip)
  install(TARGETS lucent_zip EXPORT lucentTargets
          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT lucentTargets
        FILE lucentTargets.cmake
        NAMESPACE lucent::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lucent)
