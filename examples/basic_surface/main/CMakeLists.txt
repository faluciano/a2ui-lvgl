# The sample surface (a copy of the upstream A2UI example
# catalogs/basic/v1/examples/13_coffee-order.json) is embedded at build time
# as a NUL-terminated text blob: _binary_coffee_order_json_start.
idf_component_register(SRCS "main.cpp"
                       PRIV_REQUIRES a2ui_core a2ui_lvgl esp_lcd esp_driver_spi esp_driver_gpio
                       EMBED_TXTFILES "coffee_order.json")
