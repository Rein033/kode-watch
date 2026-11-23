idf_component_register(
    SRCS "os.c" "kk.c" "board.c" "net.c" "wifi_ui.c" "kfont.c" "kos_assets.c" "kody.c" "storage.c" "ota.c" "apps.c" "craft.c" "sound.c" "imu.c" "ble.c" "rlog.c" "crash.c" "fs.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_driver_gpio esp_driver_i2c esp_driver_usb_serial_jtag nvs_flash esp_timer esp_wifi esp_netif esp_event mbedtls esp_partition esp_rom fatfs wear_levelling spi_flash sdmmc esp_driver_sdmmc app_update esp_https_ota esp_http_client bareiron bt nimble_peer esp_coex espcoredump esp_pm esp_hw_support
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE KW_HW=1)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format-truncation)
