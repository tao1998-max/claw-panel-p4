idf_component_register(
    SRCS
        "src/lua_module_storage.c"
    INCLUDE_DIRS
        "src"
    REQUIRES
        cap_lua
        claw_core
)

# Upstream builds with warning flags that do not promote this to an error.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format-truncation)
