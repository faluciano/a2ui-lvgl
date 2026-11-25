# A2UI agent surface example for ESP-IDF: draws what a WebSocket agent sends.
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(a2ui_agent_surface)

# ESP32-P4 only: esp_hosted (pulled in by esp_wifi_remote) has an unused static
# function in its eh_host_feat_wifi library, and tools/build_examples.sh fails
# on any warning.
if(TARGET eh_host_feat_wifi)
  target_compile_options(eh_host_feat_wifi PRIVATE -Wno-unused-function)
endif()
