# WebSocket transport for a2ui_core (ESP-IDF only), part of the agent_surface
# example.
#
# src/agent_session.cpp is platform-neutral (it needs only a2ui_core); the
# host Makefile builds it for the end-to-end test. src/ws_transport.cpp is the
# esp_websocket_client glue.

idf_component_register(SRCS "src/agent_session.cpp" "src/ws_transport.cpp"
                       INCLUDE_DIRS include
                       REQUIRES a2ui_core esp_websocket_client)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE
                       $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
