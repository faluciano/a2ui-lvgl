# A2UI v1.0 core: protocol model, data model, expressions. No UI dependency.
#
# Builds as an ESP-IDF component (idf.py) or, outside ESP-IDF, as a plain
# CMake static library target `a2ui_core` (alias `a2ui::core`).
#
# The library is written without exceptions or RTTI and is always compiled
# with -fno-exceptions -fno-rtti, matching ESP-IDF defaults, even if the
# application enables them.

# ESP-IDF evaluates this file in CMake script mode first, where
# CONFIGURE_DEPENDS is not allowed; there, re-run `idf.py reconfigure` after
# adding sources.
if(CMAKE_SCRIPT_MODE_FILE)
  file(GLOB_RECURSE A2UI_CORE_SRCS "${CMAKE_CURRENT_LIST_DIR}/src/*.cpp")
else()
  file(GLOB_RECURSE A2UI_CORE_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/src/*.cpp")
endif()
set(A2UI_CORE_NO_EH $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-fno-exceptions -fno-rtti>)

if(ESP_PLATFORM)
  idf_component_register(SRCS ${A2UI_CORE_SRCS}
                         INCLUDE_DIRS include)
  target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
  target_compile_options(${COMPONENT_LIB} PRIVATE ${A2UI_CORE_NO_EH})
  if(CONFIG_A2UI_COMPACT_CATALOG)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE A2UI_COMPACT_CATALOG=1)
  endif()
  return()
endif()

cmake_minimum_required(VERSION 3.16)

add_library(a2ui_core STATIC ${A2UI_CORE_SRCS})
add_library(a2ui::core ALIAS a2ui_core)
target_include_directories(a2ui_core PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(a2ui_core PUBLIC cxx_std_17)
target_compile_options(a2ui_core PRIVATE ${A2UI_CORE_NO_EH})

# basic_catalog::load() loads the compact catalog (schemas in flash).
option(A2UI_COMPACT_CATALOG "Make basic_catalog::load() load the compact basic catalog" OFF)
if(A2UI_COMPACT_CATALOG)
  target_compile_definitions(a2ui_core PRIVATE A2UI_COMPACT_CATALOG=1)
endif()
