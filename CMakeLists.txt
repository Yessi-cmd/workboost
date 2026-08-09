cmake_minimum_required(VERSION 3.20)

project(WorkBoost VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "WorkBoost targets Windows 10 or later.")
endif()

option(WORKBOOST_BUILD_TESTS "Build WorkBoost unit tests" ON)

# Install artifacts must run on a clean Windows 10 machine without a
# separately installed compiler runtime. Keep all MSVC targets on the same
# runtime model, and make MinGW executables self-contained.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY
      "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

function(workboost_enable_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc /utf-8)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

function(workboost_enable_standalone_runtime target)
  if(MINGW)
    target_link_options(${target} PRIVATE
      -static
      -static-libgcc
      -static-libstdc++
    )
  endif()
endfunction()

add_library(workboost_core
  src/app/coding_mode_command.cpp
  src/app/elevated_action_handler.cpp
  src/app/locale.cpp
  src/app/session_manager.cpp
  src/core/benchmark/benchmark.cpp
  src/core/config/config.cpp
  src/core/diagnosis/diagnosis_engine.cpp
  src/core/helper/helper_protocol.cpp
  src/core/logging/logger.cpp
  src/core/model/types.cpp
  src/core/optimization/optimization.cpp
  src/core/policy/protection_policy.cpp
  src/core/policy/service_protection_policy.cpp
  src/platform/windows/elevated_helper_client.cpp
  src/platform/windows/process_action_api.cpp
  src/platform/windows/helper_pipe.cpp
  src/platform/windows/serial_port_api.cpp
  src/platform/windows/service_api.cpp
  src/platform/windows/startup_api.cpp
  src/platform/windows/startup_benchmark_api.cpp
  src/platform/windows/system_collector.cpp
  src/platform/windows/system_hardware.cpp
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
  bcrypt
  iphlpapi
  pdh
  psapi
  setupapi
  shell32
  user32
  ws2_32
)

workboost_enable_warnings(workboost_core)

add_library(workboost_dashboard_model
  src/gui/dashboard_model.cpp
  src/gui/dashboard_renderer.cpp
)
target_include_directories(workboost_dashboard_model PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_compile_features(workboost_dashboard_model PUBLIC cxx_std_17)
target_link_libraries(workboost_dashboard_model PUBLIC workboost_core gdi32)
workboost_enable_warnings(workboost_dashboard_model)

add_executable(workboost
  src/main.cpp
  src/cli/cli.cpp
  src/gui/dashboard.cpp
)
# Both executables stay on the console subsystem on purpose: PowerShell and
# cmd capture stdout and wait for exit only for console-subsystem processes,
# which the CLI commands and the installed-CLI CI smoke test depend on. The
# dashboard instead calls FreeConsole() at startup (see cli.cpp) so it runs
# without a terminal window and can keep living in the system tray.
target_link_libraries(workboost PRIVATE
  workboost_core
  workboost_dashboard_model
  comdlg32
  gdi32
  user32
)
workboost_enable_warnings(workboost)
workboost_enable_standalone_runtime(workboost)

add_executable(workboost_elevated
  src/elevated/main.cpp
)
set_target_properties(workboost_elevated PROPERTIES
  OUTPUT_NAME "WorkBoostElevated"
)
target_link_libraries(workboost_elevated PRIVATE workboost_core)
workboost_enable_warnings(workboost_elevated)
workboost_enable_standalone_runtime(workboost_elevated)

install(TARGETS workboost workboost_elevated RUNTIME DESTINATION bin)
install(DIRECTORY config/ DESTINATION bin/config)

if(WORKBOOST_BUILD_TESTS)
  enable_testing()
  add_executable(workboost_tests tests/unit/unit_tests.cpp)
  target_link_libraries(workboost_tests PRIVATE
    workboost_core
    workboost_dashboard_model
  )
  workboost_enable_warnings(workboost_tests)
  workboost_enable_standalone_runtime(workboost_tests)
  add_test(NAME workboost_unit_tests COMMAND workboost_tests)
endif()
