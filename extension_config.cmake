# This file is included by DuckDB's build system. It specifies which extension to load

# DuckDB's own version detection (duckdb_extension_generate_version) passes the tag pattern to git with literal
# quotes, so it never matches a tag and always reports the commit hash. Use the release tag when building one.
find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --exact-match --match "v*.*.*"
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        RESULT_VARIABLE ARCGIS_GIT_RESULT
        OUTPUT_VARIABLE ARCGIS_GIT_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
if(NOT Git_FOUND OR ARCGIS_GIT_RESULT)
    set(ARCGIS_GIT_TAG "")
endif()

duckdb_extension_load(arcgis
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
    EXTENSION_VERSION "${ARCGIS_GIT_TAG}"
)
