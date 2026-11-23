# bareiron (p2r3, GPL-3.0) via kodediy/bareiron: the Minecraft Java 1.21.8 server
# KodeCraft is built on, as a component of Kode-watch OS (main/craft.c starts it).
file(GLOB srcs "${CMAKE_CURRENT_LIST_DIR}/src/*.c")
idf_component_register(SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_wifi esp_timer lwip freertos nvs_flash esp_event)
target_compile_definitions(${COMPONENT_LIB} PRIVATE "exit=bareiron_exit")
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-error -Wno-unused-variable -Wno-unused-function -Wno-implicit-fallthrough -Wno-format -Wno-maybe-uninitialized)
