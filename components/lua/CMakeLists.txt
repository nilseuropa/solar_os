set(LUA_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/lua/src")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/sources.txt")
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/sources.txt" LUA_SOURCES)
list(TRANSFORM LUA_SOURCES PREPEND "${LUA_SRC_DIR}/")

idf_component_register(
    SRCS ${LUA_SOURCES}
    INCLUDE_DIRS
        "${LUA_SRC_DIR}"
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-format
    -Wno-maybe-uninitialized
)
