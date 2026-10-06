PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=arcgis
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Runs test/sql/arcgis_mock.test against the mock ArcGIS server in test/mock (installs httpfs from the network)
test_mock:
	./test/mock/run_mock_tests.sh ./build/release/test/unittest

.PHONY: test_mock
