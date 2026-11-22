# The coffee order card is the example's copy of the upstream sample; the
# stress messages come from ../gen_stress.py. Each is embedded as a
# NUL-terminated text blob (_binary_<name>_json_start).
idf_component_register(SRCS "main.cpp"
                       PRIV_REQUIRES a2ui_core a2ui_lvgl esp_timer
                       EMBED_TXTFILES "../../../examples/basic_surface/main/coffee_order.json"
                                      "stress25.json" "stress50.json")
