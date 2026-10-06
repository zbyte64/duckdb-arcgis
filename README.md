# DuckDB ArcGIS extension

Query ArcGIS Server / ArcGIS Online / ArcGIS Enterprise feature and map services from DuckDB.

```sql
LOAD arcgis;

-- Discover services and layers
SELECT * FROM arcgis_services('https://sampleserver6.arcgisonline.com/arcgis/rest/services');
SELECT * FROM arcgis_layers('https://sampleserver6.arcgisonline.com/arcgis/rest/services/USA/MapServer');

-- Read a layer (all pages) into a table
CREATE TABLE counties AS
SELECT * FROM arcgis_query('https://sampleserver6.arcgisonline.com/arcgis/rest/services/USA/MapServer/3');

-- Server-side filtering and projection
SELECT name, pop2000, geometry
FROM arcgis_query('https://sampleserver6.arcgisonline.com/arcgis/rest/services/USA/MapServer/3',
                  where_clause := 'state_name = ''Texas''');
```

HTTP requests go through DuckDB's file system layer, i.e. the `httpfs` extension (autoloaded when
`autoload_known_extensions` is enabled, otherwise `LOAD httpfs` first). httpfs settings such as `http_proxy`,
`http_timeout` and `http_retries` apply.

## Installation

The extension is not in DuckDB's extension repositories; install it from a file attached to a
[GitHub release](https://github.com/zbyte64/duckdb-arcgis/releases). A binary only loads into the exact DuckDB
version and platform it was built for; releases are built for **DuckDB v1.5.6**.

1. Find your DuckDB version and platform:

   ```sql
   SELECT version() AS version, platform FROM pragma_platform();
   ```

2. Download `arcgis-<release>-duckdb-v1.5.6-<platform>.zip` from the release and unzip it. Keep the file name
   `arcgis.duckdb_extension`: DuckDB derives the extension's entry point from it.

3. The binaries are not signed by DuckDB, so unsigned extensions must be allowed when the database is opened (the
   setting cannot be changed afterwards). `INSTALL` copies the file to `~/.duckdb/extensions/v1.5.6/<platform>/`,
   after which `LOAD arcgis` works in every session that allows unsigned extensions.

   CLI:

   ```sh
   duckdb -unsigned
   ```

   ```sql
   INSTALL '/path/to/arcgis.duckdb_extension';
   LOAD arcgis;
   ```

   Python:

   ```python
   import duckdb

   con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
   con.install_extension("/path/to/arcgis.duckdb_extension")  # once
   con.load_extension("arcgis")
   ```

   Other clients: pass `allow_unsigned_extensions=true` in the database configuration, then run the same
   `INSTALL` / `LOAD` statements.

To upgrade, download the new release and run `FORCE INSTALL '/path/to/arcgis.duckdb_extension'`. Other DuckDB
versions need a [build from source](#building) with that version checked out in the `duckdb` and
`extension-ci-tools` submodules.

## Functions

### `arcgis_query(layer_url, ...)`

Returns the features of a layer or table (`.../FeatureServer/<id>` or `.../MapServer/<id>`). Columns follow the
layer's fields; layers with geometries get a `geometry` column of DuckDB's `GEOMETRY` type, tagged with the CRS of
the returned coordinates.

| Parameter | Type | Default | |
|---|---|---|---|
| `where_clause` | VARCHAR | `'1=1'` | ArcGIS `where` filter, evaluated by the server |
| `out_sr` | INTEGER | `4326` | Output spatial reference WKID; `NULL` returns the layer's native spatial reference |
| `token` | VARCHAR | | ArcGIS token (see [Authentication](#authentication)) |
| `page_size` | BIGINT | layer `maxRecordCount` | Features per request; capped at the layer's `maxRecordCount` |
| `pagination` | VARCHAR | `'auto'` | `'auto'`, `'offset'`, `'objectid'` or `'none'` |
| `query_params` | MAP(VARCHAR, VARCHAR) | | Extra parameters for the `query` endpoint, e.g. `MAP {'gdbVersion': 'v1', 'orderByFields': 'POP DESC'}` |

A pasted query URL works too: `where`, `outSR` and `token` in the URL become defaults, other parameters are passed
through to every query request.

Only the columns a query uses are requested (`outFields`), and geometries are only requested when the geometry column
is used, so `SELECT count(*)` or `SELECT name` transfer little data.

Field types map as follows: OID / BigInteger → `BIGINT`, Integer → `INTEGER`, SmallInteger → `SMALLINT`,
Double → `DOUBLE`, Single → `FLOAT`, Date / TimestampOffset → `TIMESTAMP WITH TIME ZONE`, DateOnly → `DATE`,
TimeOnly → `TIME`, everything else (String, GUID, GlobalID, ...) → `VARCHAR`.

Geometries are converted from Esri JSON: points, multipoints, polylines (`LINESTRING`, or `MULTILINESTRING` for
multiple paths), polygons (`POLYGON`, or `MULTIPOLYGON` when there are several exterior rings; clockwise rings are
exteriors and counter-clockwise rings are holes of the exterior that contains them) and envelopes. Z/M values are
dropped.

### `arcgis_layers(service_url, token := ...)`

Lists the layers and tables of a `MapServer` or `FeatureServer`: `id`, `name`, `type`, `geometry_type`,
`parent_layer_id`, `url`.

### `arcgis_services(directory_url, token := ...)`

Lists the services of a services directory (`.../rest/services`, including its folders) or of a single folder:
`folder`, `name`, `type`, `url`.

## Pagination

ArcGIS servers cap every response at the layer's `maxRecordCount` (and sometimes at a transfer size), flagging
truncated responses with `exceededTransferLimit`. `arcgis_query` plans all pages up front and fetches them in
parallel (at most `arcgis_max_concurrent_requests` at a time, default 4), keeping the server's order:

- `offset` (default when the layer advertises `supportsPagination`): a `returnCountOnly` request, then pages with
  `resultOffset`/`resultRecordCount`, ordered by the object id field unless `orderByFields` is given in
  `query_params`. A page the server truncates is continued from where the response stopped.
- `objectid` (default for layers with an object id field but no pagination support, e.g. older servers): a
  `returnIdsOnly` request, then object id ranges per page (`<where> AND OBJECTID >= a AND OBJECTID <= b`, which keeps
  URLs short). Features missing from a truncated response are re-requested by `objectIds`.
- `none`: a single request. With `pagination := 'auto'` this is only used for layers that support neither; a
  truncated response is then an error instead of silently returning a partial result. Ask for `'none'` explicitly to
  accept the first page.

## Error handling

ArcGIS servers often report failures with HTTP status 200 and a body like
`{"error": {"code": 400, "message": "...", "details": [...]}}`. Every response is parsed and checked; such bodies,
non-JSON bodies (e.g. HTML login pages) and responses missing the expected members raise an `IO Error` carrying the
server's code, message and details plus the request URL (with the token masked). HTTP-level failures from httpfs are
reported the same way.

## Authentication

Pass a token per query, put it in the URL (`...?token=...`), or store it in a secret scoped to a URL prefix:

```sql
CREATE SECRET agol (TYPE arcgis, TOKEN '<token>', SCOPE 'https://services.arcgis.com/<org id>/');
```

Precedence: `token :=` parameter, then a token in the URL, then the best matching `arcgis` secret.

## Caching with cache_httpfs

Requests are plain GETs through DuckDB's virtual file system, so the
[`cache_httpfs`](https://duckdb.org/community_extensions/extensions/cache_httpfs.html) community extension serves
them once it is loaded:

```sql
INSTALL cache_httpfs FROM community;
LOAD cache_httpfs;
SELECT count(*) FROM arcgis_query('...'); -- requests sent
SELECT count(*) FROM arcgis_query('...'); -- served from cache, no requests
```

Repeated requests for the same URL (same query, page and token) are answered from cache_httpfs' file handle cache
for its lifetime (`cache_httpfs_file_handle_cache_entry_timeout_millisec`, default 1 hour) within a DuckDB process.
Use `cache_httpfs_clear_cache()` to see fresh server data. ArcGIS responses are generated per request, so their size
is only known after downloading them; the extension therefore asks httpfs for a single full GET (no HEAD or range
requests, which ArcGIS servers do not answer reliably). As a consequence cache_httpfs' on-disk cache does not avoid
requests across processes.

## Limitations

- Requests use GET; very long `where_clause` values or `query_params` can exceed server URL limits.
- Filters in the SQL `WHERE` clause are applied by DuckDB after download; use `where_clause` to filter on the server.
- True curves (`returnTrueCurves`) are not supported.

## Building

```sh
git submodule update --init --recursive
GEN=ninja make release        # build/release/duckdb, build/release/extension/arcgis/arcgis.duckdb_extension
make test                     # SQL tests in test/sql that need no server
make test_mock                # test/sql/arcgis_mock.test against test/mock/arcgis_mock_server.py
```

The build derives the extension version from git, so the repository needs at least one commit.

## Releasing

Push a `v*` tag. CI builds and tests every platform, then creates the GitHub release for the tag with one
`arcgis-<tag>-duckdb-v1.5.6-<platform>.zip` per platform (if the release already exists, its archives are replaced):

```sh
git tag v0.2.0
git push origin v0.2.0
```
