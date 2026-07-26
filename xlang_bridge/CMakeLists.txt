cmake_minimum_required(VERSION 3.20)

project(electron_xlang_bridge VERSION 0.1.0 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(XLangSource REQUIRED)
find_package(Threads REQUIRED)

add_library(electron_xlang_bridge SHARED
  include/xlang_bridge.h
  src/xlang_bridge.cpp
  ${XLANG_API_SOURCES})

target_compile_features(electron_xlang_bridge PRIVATE cxx_std_17)
target_compile_definitions(electron_xlang_bridge PRIVATE
  XLANG_BRIDGE_IMPLEMENTATION)
if(WIN32)
  # XLang's portable loader selects Win32 APIs with this legacy macro. Define
  # it for every Windows toolchain, including clang-cl, not just MSVC.
  target_compile_definitions(electron_xlang_bridge PRIVATE WIN32)
endif()
target_include_directories(electron_xlang_bridge
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
  PRIVATE
    "${XLANG_API_DIR}")
target_link_libraries(electron_xlang_bridge PRIVATE
  Threads::Threads
  ${CMAKE_DL_LIBS})

set_target_properties(electron_xlang_bridge PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
  OUTPUT_NAME electron_xlang_bridge
  PREFIX "")

if(MSVC)
  # XLang's API shim and the bridge are one binary and must use one CRT.
  set_property(TARGET electron_xlang_bridge PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded")
  target_compile_options(electron_xlang_bridge PRIVATE
    /W4 /permissive- /EHsc)
else()
  target_compile_options(electron_xlang_bridge PRIVATE
    -Wall -Wextra -Wpedantic)
endif()

include(GNUInstallDirs)
install(TARGETS electron_xlang_bridge
  RUNTIME DESTINATION .
  LIBRARY DESTINATION .
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES include/xlang_bridge.h
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

include(CTest)
if(BUILD_TESTING)
  add_library(xlang_bridge_event_test MODULE
    tests/event_test_module.cpp
    "${XLANG_API_DIR}/value.cpp")
  target_compile_features(xlang_bridge_event_test PRIVATE cxx_std_17)
  target_include_directories(xlang_bridge_event_test PRIVATE
    "${XLANG_API_DIR}")
  target_link_libraries(xlang_bridge_event_test PRIVATE
    Threads::Threads
    ${CMAKE_DL_LIBS})
  set_target_properties(xlang_bridge_event_test PROPERTIES
    OUTPUT_NAME xlang_bridge_event_test)
  if(WIN32)
    target_compile_definitions(xlang_bridge_event_test PRIVATE WIN32)
  endif()
  if(MSVC)
    set_property(TARGET xlang_bridge_event_test PROPERTY
      MSVC_RUNTIME_LIBRARY "MultiThreaded")
    target_compile_options(xlang_bridge_event_test PRIVATE
      /W4 /permissive- /EHsc)
  else()
    target_compile_options(xlang_bridge_event_test PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()

  add_executable(electron_xlang_bridge_smoke
    tests/bridge_smoke.cpp)
  add_dependencies(electron_xlang_bridge_smoke
    xlang_bridge_event_test)
  target_compile_features(electron_xlang_bridge_smoke PRIVATE cxx_std_17)
  target_include_directories(electron_xlang_bridge_smoke PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(electron_xlang_bridge_smoke PRIVATE
    electron_xlang_bridge
    Threads::Threads)
  if(MSVC)
    set_property(TARGET electron_xlang_bridge_smoke PROPERTY
      MSVC_RUNTIME_LIBRARY "MultiThreaded")
    target_compile_options(electron_xlang_bridge_smoke PRIVATE /EHsc)
  endif()

  set(XLANG_TEST_RUNTIME_DIR "" CACHE PATH
    "Optional runtime directory for the XLang bridge smoke test")
  if(XLANG_TEST_RUNTIME_DIR)
    add_test(NAME electron_xlang_bridge_smoke
      COMMAND electron_xlang_bridge_smoke "${XLANG_TEST_RUNTIME_DIR}")
  endif()
endif()
