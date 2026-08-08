cmake_minimum_required(VERSION 3.19)

project(macdeploytool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

list(APPEND COMPILE_OPTIONS
  -Wall
  -Wextra
  -Wpedantic
  -Wunused
  -Wshadow
  -Wundef
  -Wuninitialized
  -Wredundant-decls
  -Wcast-align
  -Winit-self
  -Wmissing-include-dirs
  -Wmissing-declarations
  -Wstrict-overflow=2
  -Wunused-parameter
  -Wformat=2
  -Wdisabled-optimization
  $<$<COMPILE_LANGUAGE:CXX>:-Woverloaded-virtual>
  $<$<COMPILE_LANGUAGE:CXX>:-Wold-style-cast>
)
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  list(APPEND COMPILE_OPTIONS -Wno-unused-command-line-argument)
endif()

add_compile_options(${COMPILE_OPTIONS})

add_definitions(
  -DQT_STRICT_ITERATORS
  -DQT_NO_CAST_FROM_BYTEARRAY
  -DQT_USE_QSTRINGBUILDER
  -DQT_NO_URL_CAST_FROM_STRING
  -DQT_NO_CAST_TO_ASCII
  -DQT_NO_NARROWING_CONVERSIONS_IN_CONNECT
  -DQT_NO_FOREACH
  -DQT_ASCII_CAST_WARNINGS
  -DQT_NO_CAST_FROM_ASCII
  -DQT_NO_KEYWORDS
  -DQT_NO_SIGNALS_SLOTS_KEYWORDS
  -DQT_USE_NODISCARD_FILE_OPEN
)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core)

#qt_add_resources(APPIMAGEDEPLOY_RESOURCES data/data.qrc)

add_executable(macdeploytool
  src/main.cpp
  src/logging.cpp
  src/frameworkinfo.cpp
  src/frameworkinfo.h
  src/deploymentinfo.cpp
  src/deploymentinfo.h
  src/macdeploytool.cpp
  src/macdeploytool.h
  ${APPIMAGEDEPLOY_RESOURCES}
)
target_include_directories(macdeploytool PUBLIC src)
target_link_libraries(macdeploytool PUBLIC Qt6::Core)
if(APPLE)
  target_link_libraries(macdeploytool PUBLIC "-framework Foundation")
endif()

install(TARGETS macdeploytool RUNTIME DESTINATION bin COMPONENT macdeploytool)
