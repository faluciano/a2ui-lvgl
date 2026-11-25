idf_component_register(SRCS "main.cpp" "display.cpp" "wifi.cpp"
                       PRIV_REQUIRES a2ui_core a2ui_lvgl a2ui_transport_ws
                                     esp_lcd esp_driver_spi esp_driver_gpio
                                     esp_wifi esp_netif esp_event nvs_flash esp_timer)
