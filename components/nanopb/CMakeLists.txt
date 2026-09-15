set(NANOPB_UPSTREAM "${CMAKE_CURRENT_LIST_DIR}/upstream")

if(NOT EXISTS "${NANOPB_UPSTREAM}/pb.h")
    message(FATAL_ERROR
        "nanopb submodule missing at ${NANOPB_UPSTREAM}. "
        "Run: git submodule update --init --recursive components/nanopb/upstream")
endif()

idf_component_register(
    SRCS
        "${NANOPB_UPSTREAM}/pb_common.c"
        "${NANOPB_UPSTREAM}/pb_encode.c"
        "${NANOPB_UPSTREAM}/pb_decode.c"
    INCLUDE_DIRS
        "upstream"
)

# Pointer-backed generated fields (e.g. large trace arrays) need malloc support.
target_compile_definitions(${COMPONENT_LIB} PUBLIC PB_ENABLE_MALLOC=1)
