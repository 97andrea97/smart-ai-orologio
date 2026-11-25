idf_component_register(
    SRCS main.c memoria.c rete.c seriale.c aggiorna.c
    INCLUDE_DIRS .
    PRIV_REQUIRES esp_driver_gpio esp_driver_usb_serial_jtag esp_app_format nvs_flash
                  esp_wifi esp_netif esp_event esp_http_client mbedtls json app_update)

# Come nell'esempio 02_lvgl_demo_v9 di Waveshare: avvisi di LVGL non bloccanti.
idf_build_get_property(build_components BUILD_COMPONENTS)
if("lvgl__lvgl" IN_LIST build_components)
    idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
    if(TARGET ${LVGL_LIB})
        target_compile_options(${LVGL_LIB} PRIVATE -Wno-error=attributes -Wno-error=cpp)
    endif()
endif()
