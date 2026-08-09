cmake_minimum_required(VERSION 3.20)

project(WorkBoost VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "WorkBoost targets Windows 10 or later.")
endif()

option(WORKBOOST_BUILD_TESTS "Build WorkBoost unit tests" ON)

add_library(workboost_core
  src/app/session_manager.cpp
  src/core/config/config.cpp
  src/core/diagnosis/diagnosis_engine.cpp
  src/core/model/types.cpp
  src/core/optimization/optimization.cpp
  src/core/policy/protection_policy.cpp
  src/platform/windows/system_collector.cpp
  src/platform/windows/windows_utils.cpp
)

target_include_directories(workboost_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(workboost_core PUBLIC cxx_std_17)
target_compile_definitions(workboost_core PUBLIC
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _WIN32_WINNT=0x0A00
  NTDDI_VERSION=0x0A000000
)
target_link_libraries(workboost_core PUBLIC
  advapi32
  iphlpapi
  pdh
  psapi
  setupapi
  ws2_32
)

if(MSVC)
  target_compile_options(workboost_core PRIVATE /W4 /permissive- /EHsc /utf-8)
else()
  target_compile_options(workboost_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(workboost
  src/app.rc
  src/main.cpp
  src/cli/cli.cpp
)
target_link_libraries(workboost PRIVATE workboost_core)

install(TARGETS workboost RUNTIME DESTINATION bin)
install(DIRECTORY config/ DESTINATION config)

if(WORKBOOST_BUILD_TESTS)
  enable_testing()
  add_executable(workboost_tests tests/unit/unit_tests.cpp)
  target_link_libraries(workboost_tests PRIVATE workboost_core)
  add_test(NAME workboost_unit_tests COMMAND workboost_tests)
endif()
