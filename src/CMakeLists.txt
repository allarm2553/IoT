FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(
    SRCS ${app_sources}
    REQUIRES
        microlink
        esp_wifi
        esp_netif
        nvs_flash
        esp_event
        mdns
)
