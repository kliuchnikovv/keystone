# The WAMR-backed PluginEngine for the brain (Plan 1b start). Portable headers
# (core/plugin_*) live in `core`; this component implements the engine seam and
# the native host-import shims on top of the vendored WAMR (components/wamr).
idf_component_register(
    SRCS wamr_engine.cpp wasm_host.cpp
    INCLUDE_DIRS include
    REQUIRES core hubproto wamr pthread)
