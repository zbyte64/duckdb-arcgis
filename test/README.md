# Testing this extension

`sql/` holds [SQLLogicTests](https://duckdb.org/dev/sqllogictest/intro.html):

- `arcgis.test` needs no server and runs with `make test`.
- `arcgis_mock.test` runs against `mock/arcgis_mock_server.py`, a small ArcGIS REST server that exercises pagination,
  truncated responses, tokens and errors returned with HTTP 200. Run it with `make test_mock` (starts the server and
  sets `ARCGIS_MOCK_URL`); it is skipped by `make test`. It installs httpfs, so it needs network access.
