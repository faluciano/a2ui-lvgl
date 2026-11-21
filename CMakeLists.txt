# Plain CMake entry point (not used by ESP-IDF, which reads components/*/).
#
#   add_subdirectory(a2ui-lvgl)            # or FetchContent
#   target_link_libraries(app PRIVATE a2ui::core a2ui::lvgl)
#
# a2ui::core has no dependencies. a2ui::lvgl is added when an `lvgl` target
# (LVGL 9.6 or later) exists before this directory is added.
#
# Building this directory on its own also builds LVGL from third_party/lvgl
# and the render_sample host tool (A2UI_BUILD_TOOLS).

cmake_minimum_required(VERSION 3.16)
project(a2ui_lvgl VERSION 0.1.0 LANGUAGES C CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(A2UI_IS_TOP_LEVEL ON)
else()
  set(A2UI_IS_TOP_LEVEL OFF)
endif()
option(A2UI_BUILD_TOOLS "Build third_party/lvgl and the render_sample host tool" ${A2UI_IS_TOP_LEVEL})

add_subdirectory(components/a2ui_core)

if(A2UI_BUILD_TOOLS AND NOT TARGET lvgl)
  if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/third_party/lvgl/lvgl.h")
    message(FATAL_ERROR "third_party/lvgl is missing. Run: git submodule update --init third_party/lvgl")
  endif()
  set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/test/lvgl/lv_conf.h" CACHE PATH "" FORCE)
  set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
  set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(CONFIG_LV_USE_THORVG OFF CACHE BOOL "" FORCE)
  set(LV_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/lvgl EXCLUDE_FROM_ALL)
endif()

if(TARGET lvgl)
  add_subdirectory(components/a2ui_lvgl)
endif()

if(A2UI_BUILD_TOOLS)
  add_executable(render_sample
    tools/render_sample/render_sample.cpp
    tools/render_sample/png_writer.cpp
    test/lvgl/lvgl_env.cpp)
  target_include_directories(render_sample PRIVATE test/lvgl)
  target_link_libraries(render_sample PRIVATE a2ui::lvgl a2ui::core)
endif()
