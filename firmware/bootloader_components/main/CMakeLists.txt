# kodeOS's kboot for the watch: the stock bootloader, plus booting the app
# Kode-watch OS asked for (kapps_fmt.h, the apps region's request).
idf_component_register(SRCS "bootloader_start.c"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES bootloader bootloader_support spi_flash esp_hal_gpio esp_hal_usb)

# No -flto: LTO cannot see the ENTRY() reference from the linker script (see kodeOS).
target_compile_options(${COMPONENT_LIB} PRIVATE "-Oz" "-fno-unroll-loops" "-fno-unwind-tables" "-fno-asynchronous-unwind-tables")

idf_build_get_property(scripts BOOTLOADER_LINKER_SCRIPT)
target_linker_script(${COMPONENT_LIB} INTERFACE "${scripts}")
