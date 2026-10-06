cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME arcgis)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

set(EXTENSION_SOURCES
    src/arcgis_extension.cpp
    src/arcgis_cache.cpp
    src/arcgis_catalog.cpp
    src/arcgis_geometry.cpp
    src/arcgis_http.cpp
    src/arcgis_query.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})
# SHA-256 of cache keys (arcgis_cache.cpp); the static extension gets it from duckdb_static
target_link_libraries(${LOADABLE_EXTENSION_NAME} duckdb_mbedtls)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
