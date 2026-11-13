# The sample surface is an upstream example file, embedded at build time
# (a NUL-terminated text blob: _binary_13_coffee_order_json_start).
idf_component_register(SRCS "main.cpp"
                       PRIV_REQUIRES a2ui_core a2ui_lvgl esp_lcd esp_driver_spi esp_driver_gpio
                       EMBED_TXTFILES "../../../third_party/a2ui/catalogs/basic/v1/examples/13_coffee-order.json")
