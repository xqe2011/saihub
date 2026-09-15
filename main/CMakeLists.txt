file(GLOB_RECURSE SRC_LIST ${COMPONENT_DIR}/src/*.c)
set(SRC_FILES "")
foreach(file ${SRC_LIST})
    list(APPEND SRC_FILES ${file})
endforeach()

set(NANOPB_UPSTREAM "${PROJECT_DIR}/components/nanopb/upstream")
set(NANOPB_GENERATOR "${NANOPB_UPSTREAM}/generator/nanopb_generator.py")

# During early expansion, only register sources; generation needs a real build tree.
# Do not set REQUIRES/PRIV_REQUIRES here: main must keep its implicit all-components deps.
if(CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_register(
        SRCS "${SRC_FILES}"
        INCLUDE_DIRS "include"
        EMBED_FILES "../openapi.json" "../mcp.json" "../api.proto" "embed/portal.html.gz" "embed/control.html.gz"
    )
    return()
endif()

if(NOT EXISTS "${NANOPB_GENERATOR}")
    message(FATAL_ERROR
        "nanopb generator missing at ${NANOPB_GENERATOR}. "
        "Run: git submodule update --init --recursive components/nanopb/upstream")
endif()

# Generate api.pb.c / api.pb.h from api.proto via nanopb (requires protoc + protobuf Python package).
set(API_PROTO "${PROJECT_DIR}/api.proto")
set(API_OPTIONS "${PROJECT_DIR}/api.options")
set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto")
set(API_PB_C "${PROTO_GEN_DIR}/api.pb.c")
set(API_PB_H "${PROTO_GEN_DIR}/api.pb.h")
file(MAKE_DIRECTORY "${PROTO_GEN_DIR}")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(PROTOC_EXECUTABLE protoc REQUIRED)

set(NANOPB_GEN_DEPS
    "${API_PROTO}"
    "${API_OPTIONS}"
    "${NANOPB_GENERATOR}"
    "${NANOPB_UPSTREAM}/generator/__init__.py"
    "${NANOPB_UPSTREAM}/generator/proto/__init__.py"
    "${NANOPB_UPSTREAM}/generator/proto/_utils.py"
    "${NANOPB_UPSTREAM}/generator/proto/nanopb.proto"
    "${NANOPB_UPSTREAM}/generator/proto/google/protobuf/descriptor.proto"
)

# Avoid writing nanopb_pb2.py into the submodule checkout.
set(NANOPB_PB2_TEMP_DIR "${CMAKE_CURRENT_BINARY_DIR}/nanopb_pb2_tmp")
file(MAKE_DIRECTORY "${NANOPB_PB2_TEMP_DIR}")

# Configure-time generation so the source exists for idf_component_register.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
            "NANOPB_PB2_TEMP_DIR=${NANOPB_PB2_TEMP_DIR}"
            "${Python3_EXECUTABLE}" "${NANOPB_GENERATOR}"
            -I "${PROJECT_DIR}"
            -D "${PROTO_GEN_DIR}"
            "${API_PROTO}"
    RESULT_VARIABLE API_PROTO_GEN_RC
    WORKING_DIRECTORY "${PROJECT_DIR}"
)
if(NOT API_PROTO_GEN_RC EQUAL 0)
    message(FATAL_ERROR "Failed to generate api.pb.c/h from api.proto (need protoc + protobuf Python package)")
endif()

add_custom_command(
    OUTPUT "${API_PB_C}" "${API_PB_H}"
    COMMAND "${CMAKE_COMMAND}" -E env
            "NANOPB_PB2_TEMP_DIR=${NANOPB_PB2_TEMP_DIR}"
            "${Python3_EXECUTABLE}" "${NANOPB_GENERATOR}"
            -I "${PROJECT_DIR}"
            -D "${PROTO_GEN_DIR}"
            "${API_PROTO}"
    DEPENDS ${NANOPB_GEN_DEPS}
    WORKING_DIRECTORY "${PROJECT_DIR}"
    COMMENT "Generating nanopb sources from api.proto"
    VERBATIM
)

idf_component_register(
    SRCS "${SRC_FILES}" "${API_PB_C}"
    INCLUDE_DIRS "include" "${PROTO_GEN_DIR}"
    EMBED_FILES "../openapi.json" "../mcp.json" "../api.proto" "embed/portal.html.gz" "embed/control.html.gz"
)

# Link the nanopb component without PRIV_REQUIRES so main keeps implicit deps.
idf_component_get_property(nanopb_lib nanopb COMPONENT_LIB)
target_link_libraries(${COMPONENT_LIB} PUBLIC ${nanopb_lib})

# Rebuild generated sources when proto or generator inputs change.
add_custom_target(api_proto_gen DEPENDS "${API_PB_C}" "${API_PB_H}")
add_dependencies(${COMPONENT_LIB} api_proto_gen)
