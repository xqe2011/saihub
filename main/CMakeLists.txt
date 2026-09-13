file(GLOB_RECURSE SRC_LIST ${COMPONENT_DIR}/src/*.c)
set(SRC_FILES "")
foreach(file ${SRC_LIST})
    list(APPEND SRC_FILES ${file})
endforeach()

idf_component_register(
    SRCS "${SRC_FILES}"
    INCLUDE_DIRS "include"
    EMBED_FILES "../openapi.json"
    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_server json driver esp_timer lwip
)
