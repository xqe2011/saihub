file(GLOB_RECURSE SRC_LIST ${COMPONENT_DIR}/src/*.c)
set(SRC_FILES "")
foreach(file ${SRC_LIST})
    list(APPEND SRC_FILES ${file})
endforeach()

idf_component_register(
    SRCS "${SRC_FILES}"
    INCLUDE_DIRS "include"
    EMBED_FILES "../openapi.json" "../mcp.json" "embed/portal.html.gz" "embed/control.html.gz"
)
