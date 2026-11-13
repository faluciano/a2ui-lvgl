# LVGL framework adapter for the A2UI core.
#
# Builds as an ESP-IDF component (idf.py; LVGL comes from the component
# registry, see idf_component.yml) or, outside ESP-IDF, as a plain CMake
# static library target `a2ui_lvgl` (alias `a2ui::lvgl`). For the plain CMake
# build, add LVGL first so that the `lvgl` target exists (for example
# add_subdirectory(third_party/lvgl) with your lv_conf.h on the include path);
# the a2ui_core target is added automatically if it is missing.

# ESP-IDF evaluates this file in CMake script mode first, where
# CONFIGURE_DEPENDS is not allowed; there, re-run `idf.py reconfigure` after
# adding sources.
if(CMAKE_SCRIPT_MODE_FILE)
  file(GLOB_RECURSE A2UI_LVGL_SRCS "${CMAKE_CURRENT_LIST_DIR}/src/*.cpp")
else()
  file(GLOB_RECURSE A2UI_LVGL_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/src/*.cpp")
endif()
set(A2UI_LVGL_NO_EH $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-fno-exceptions -fno-rtti>)

if(ESP_PLATFORM)
  idf_component_register(SRCS ${A2UI_LVGL_SRCS}
                         INCLUDE_DIRS include
                         REQUIRES a2ui_core lvgl)
  target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
  target_compile_options(${COMPONENT_LIB} PRIVATE ${A2UI_LVGL_NO_EH})
  return()
endif()

cmake_minimum_required(VERSION 3.16)

if(NOT TARGET a2ui_core)
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../a2ui_core"
                   "${CMAKE_CURRENT_BINARY_DIR}/a2ui_core")
endif()
if(NOT TARGET lvgl)
  message(FATAL_ERROR "a2ui_lvgl: no `lvgl` target. Add LVGL (v9.6 or later) before this directory.")
endif()

add_library(a2ui_lvgl STATIC ${A2UI_LVGL_SRCS})
add_library(a2ui::lvgl ALIAS a2ui_lvgl)
target_include_directories(a2ui_lvgl PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(a2ui_lvgl PUBLIC cxx_std_17)
target_compile_options(a2ui_lvgl PRIVATE ${A2UI_LVGL_NO_EH})
target_link_libraries(a2ui_lvgl PUBLIC a2ui_core lvgl)
