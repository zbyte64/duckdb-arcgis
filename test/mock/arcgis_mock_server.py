#!/usr/bin/env python3
"""Minimal ArcGIS REST server used by test/sql/arcgis_mock.test.

Serves a handful of layers that exercise pagination strategies, truncated responses, token handling and
errors reported with HTTP 200. Query `where` clauses are evaluated with SQLite.

Usage: arcgis_mock_server.py [--port PORT]
Prints "READY <port>" once listening. GET /__stats returns {"requests": n} (query + metadata requests so far),
GET /__reset resets the counter, GET /__fail_queries?enabled=true|false makes every layer query fail with HTTP 200 and
an error body.
"""

import argparse
import json
import random
import re
import sqlite3
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

ROOT = "/arcgis/rest/services"
TOKEN = "secret-token"

FIELDS = [
    {"name": "OBJECTID", "type": "esriFieldTypeOID"},
    {"name": "NAME", "type": "esriFieldTypeString"},
    {"name": "CATEGORY", "type": "esriFieldTypeString"},
    {"name": "POP", "type": "esriFieldTypeInteger"},
    {"name": "AREA", "type": "esriFieldTypeDouble"},
    {"name": "SMALL", "type": "esriFieldTypeSmallInteger"},
    {"name": "CREATED", "type": "esriFieldTypeDate"},
    {"name": "DAY", "type": "esriFieldTypeDateOnly"},
    {"name": "GLOBALID", "type": "esriFieldTypeGlobalID"},
    {"name": "Shape", "type": "esriFieldTypeGeometry"},
]

ROWS = [
    # OBJECTID, NAME, CATEGORY, POP, AREA, SMALL, CREATED (epoch ms), DAY
    (1, "alpha", "a", 100, 1.5, 1, 1704067200000, "2024-01-01"),
    (2, "bravo", "b", 200, 2.5, 2, 1704153600000, "2024-01-02"),
    (3, "charlie", "a", None, None, None, None, None),
    (5, "delta", "b", 400, 4.5, 4, 1704326400000, "2024-01-04"),
    (8, "echo", "a", 500, 5.5, 5, 1704412800000, "2024-01-05"),
    (13, "foxtrot", "b", 600, 6.5, 6, 1704499200000, "2024-01-06"),
    (21, "golf", "a", 700, 7.5, 7, 1704585600000, "2024-01-07"),
]

# Clockwise exterior rings, counter-clockwise holes (Esri convention)
SQUARE = [[0, 0], [0, 10], [10, 10], [10, 0], [0, 0]]
HOLE = [[2, 2], [4, 2], [4, 4], [2, 4], [2, 2]]
SQUARE2 = [[20, 0], [20, 5], [25, 5], [25, 0], [20, 0]]

POLYGONS = {
    1: {"rings": [SQUARE]},
    2: {"rings": [SQUARE, HOLE]},
    3: None,
    5: {"rings": [SQUARE, SQUARE2]},
    8: {"rings": [SQUARE2, HOLE, SQUARE]},
    13: {"rings": []},
    21: {"rings": [[[0, 0], [0, 1], [1, 1], [1, 0]]]},  # unclosed ring
}


def point_geometry(oid):
    if oid == 3:
        return {"x": "NaN", "y": "NaN"}
    return {"x": float(oid), "y": float(oid) * 2}


def line_geometry(oid):
    if oid % 2:
        return {"paths": [[[0, 0], [oid, oid]]]}
    return {"paths": [[[0, 0], [1, 1]], [[2, 2], [oid, 0, 99]]]}


def make_layer(
    layer_id, name, geometry_type, geometry, max_record_count, pagination, response_cap=None, oid=True, rows=ROWS
):
    fields = [f for f in FIELDS if oid or f["type"] != "esriFieldTypeOID"]
    if geometry_type is None:
        fields = [f for f in fields if f["type"] != "esriFieldTypeGeometry"]
    metadata = {
        "id": layer_id,
        "name": name,
        "type": "Feature Layer" if geometry_type else "Table",
        "capabilities": "Query",
        "maxRecordCount": max_record_count,
        "advancedQueryCapabilities": {"supportsPagination": pagination},
        "fields": fields,
        "extent": {"spatialReference": {"wkid": 102100, "latestWkid": 3857}},
    }
    if geometry_type:
        metadata["geometryType"] = geometry_type
    if oid:
        metadata["objectIdField"] = "OBJECTID"
    return {
        "metadata": metadata,
        "geometry": geometry,
        "cap": response_cap or max_record_count,
        "oid": oid,
        "rows": rows,
    }


LAYERS = {
    # offset pagination
    0: make_layer(0, "parcels", "esriGeometryPolygon", POLYGONS.get, 3, True),
    # object id pagination
    1: make_layer(1, "points", "esriGeometryPoint", point_geometry, 2, False),
    # no object ids, no pagination
    2: make_layer(2, "plain_table", None, None, 2, False, oid=False),
    # every query fails with HTTP 200 and an error body
    3: make_layer(3, "broken", "esriGeometryPoint", point_geometry, 1000, True),
    # requires a token
    4: make_layer(4, "secured", "esriGeometryPoint", point_geometry, 1000, True),
    # object id pagination with a server that truncates responses below maxRecordCount
    5: make_layer(5, "lines", "esriGeometryPolyline", line_geometry, 10, False, response_cap=2),
    # offset pagination with a server that truncates responses below maxRecordCount
    6: make_layer(6, "capped", "esriGeometryPoint", point_geometry, 10, True, response_cap=2),
}

ERROR_LAYER = 3
TOKEN_LAYER = 4

lock = threading.Lock()
stats = {"requests": 0, "fail_queries": False}


def build_db():
    db = sqlite3.connect(":memory:", check_same_thread=False)
    db.execute(
        "CREATE TABLE t (OBJECTID INTEGER, NAME TEXT, CATEGORY TEXT, POP INTEGER, AREA REAL, SMALL INTEGER, "
        "CREATED INTEGER, DAY TEXT)"
    )
    db.executemany("INSERT INTO t VALUES (?, ?, ?, ?, ?, ?, ?, ?)", ROWS)
    return db


DB = build_db()


def error_body(code, message, details=()):
    return {"error": {"code": code, "message": message, "details": list(details)}}


def run_query(layer, params):
    def param(name, default=None):
        for key, values in params.items():
            if key.lower() == name.lower():
                return values[0]
        return default

    where = param("where", "1=1")
    sql = f"SELECT * FROM t WHERE ({where})"
    args = []
    object_ids = param("objectIds")
    if object_ids:
        ids = [int(v) for v in object_ids.split(",")]
        sql += f" AND OBJECTID IN ({','.join('?' * len(ids))})"
        args.extend(ids)
    order_by = param("orderByFields")
    if order_by:
        sql += f" ORDER BY {order_by}"
    try:
        with lock:
            rows = DB.execute(sql, args).fetchall()
    except sqlite3.Error as ex:
        return error_body(400, "Unable to complete operation.", [f"'where' parameter is invalid: {ex}"])

    if param("returnCountOnly", "false") == "true":
        return {"count": len(rows)}
    if param("returnIdsOnly", "false") == "true":
        if not layer["oid"]:
            return error_body(400, "Unable to complete operation.", ["Layer has no object id field"])
        ids = [r[0] for r in rows]
        random.shuffle(ids)  # ArcGIS does not guarantee order
        return {"objectIdFieldName": "OBJECTID", "objectIds": ids}

    offset = int(param("resultOffset", "0"))
    requested = param("resultRecordCount")
    window = rows[offset:]
    if requested is not None:
        window = window[: int(requested)]
    returned = window[: layer["cap"]]
    exceeded = len(returned) < len(window)

    names = [f["name"] for f in layer["metadata"]["fields"] if f["type"] != "esriFieldTypeGeometry"]
    out_fields = param("outFields", "*")
    if out_fields != "*":
        wanted = {f.lower() for f in out_fields.split(",")}
        unknown = wanted - {n.lower() for n in names}
        if unknown:
            return error_body(400, "Unable to complete operation.", [f"Invalid field(s) in outFields: {unknown}"])
        names = [n for n in names if n.lower() in wanted]
    return_geometry = param("returnGeometry", "true") == "true" and layer["geometry"] is not None

    columns = ["OBJECTID", "NAME", "CATEGORY", "POP", "AREA", "SMALL", "CREATED", "DAY"]
    features = []
    for row in returned:
        record = dict(zip(columns, row))
        attributes = {n: record.get(n) for n in names if n != "GLOBALID"}
        if "GLOBALID" in names:
            attributes["GLOBALID"] = "{%08d-0000-0000-0000-000000000000}" % row[0]
        feature = {"attributes": attributes}
        if return_geometry:
            feature["geometry"] = layer["geometry"](row[0])
        features.append(feature)
    result = {
        "objectIdFieldName": "OBJECTID",
        "spatialReference": {"wkid": int(param("outSR", "102100"))},
        "fields": layer["metadata"]["fields"],
        "features": features,
    }
    if exceeded:
        result["exceededTransferLimit"] = True
    return result


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format, *args):  # noqa: A002
        pass

    def send_body(self, body, content_type="application/json"):
        data = body.encode() if isinstance(body, str) else json.dumps(body).encode()
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def control_body(self):
        """Body of a test control endpoint (None for other paths). Control endpoints are idempotent."""
        url = urlparse(self.path)
        params = parse_qs(url.query, keep_blank_values=True)
        path = url.path.rstrip("/")
        if path == "/__reset":
            stats["requests"] = 0
        elif path == "/__fail_queries":
            stats["fail_queries"] = params.get("enabled", ["false"])[0] == "true"
            return json.dumps({"fail_queries": stats["fail_queries"]}).encode()
        elif path != "/__stats":
            return None
        return json.dumps({"requests": stats["requests"]}).encode()

    def do_HEAD(self):  # noqa: N802
        # Control endpoints behave like static files so that read_text (HEAD, then ranged GETs) can fetch them
        data = self.control_body()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        # Like ArcGIS Online: a HEAD response whose Content-Length has nothing to do with the GET body
        self.send_header("Content-Length", "7" if data is None else str(len(data)))
        self.end_headers()

    def send_control(self, data):
        ranged = re.fullmatch(r"bytes=(\d+)-(\d*)", self.headers.get("Range", ""))
        if not ranged:
            return self.send_body(data.decode())
        start = int(ranged.group(1))
        end = int(ranged.group(2)) if ranged.group(2) else len(data) - 1
        chunk = data[start : end + 1]
        self.send_response(206)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Range", f"bytes {start}-{start + len(chunk) - 1}/{len(data)}")
        self.send_header("Content-Length", str(len(chunk)))
        self.end_headers()
        self.wfile.write(chunk)

    def do_GET(self):  # noqa: N802
        url = urlparse(self.path)
        params = parse_qs(url.query, keep_blank_values=True)
        path = url.path.rstrip("/")

        control = self.control_body()
        if control is not None:
            return self.send_control(control)
        with lock:
            stats["requests"] += 1

        if path == ROOT:
            return self.send_body(
                {
                    "currentVersion": 11.1,
                    "folders": ["Folder"],
                    "services": [{"name": "Test", "type": "FeatureServer"}, {"name": "Html", "type": "FeatureServer"}],
                }
            )
        if path == ROOT + "/Folder":
            return self.send_body(
                {"currentVersion": 11.1, "folders": [], "services": [{"name": "Folder/Inner", "type": "MapServer"}]}
            )
        if path.startswith(ROOT + "/Html"):
            return self.send_body("<html><body>Please sign in</body></html>", "text/html")
        if path == ROOT + "/Test/FeatureServer":
            layers = [
                {
                    "id": i,
                    "name": l["metadata"]["name"],
                    "parentLayerId": -1,
                    "geometryType": l["metadata"]["geometryType"],
                    "type": "Feature Layer",
                }
                for i, l in LAYERS.items()
                if "geometryType" in l["metadata"]
            ]
            tables = [
                {"id": i, "name": l["metadata"]["name"]}
                for i, l in LAYERS.items()
                if "geometryType" not in l["metadata"]
            ]
            return self.send_body({"currentVersion": 11.1, "layers": layers, "tables": tables})

        prefix = ROOT + "/Test/FeatureServer/"
        if not path.startswith(prefix):
            return self.send_body(error_body(404, "Service not found"))
        parts = path[len(prefix) :].split("/")
        try:
            layer_id = int(parts[0])
            layer = LAYERS[layer_id]
        except (ValueError, KeyError):
            return self.send_body(error_body(400, "Invalid or missing input parameters."))

        if layer_id == TOKEN_LAYER:
            token = params.get("token", [None])[0]
            if token is None:
                return self.send_body(error_body(499, "Token Required"))
            if token != TOKEN:
                return self.send_body(error_body(498, "Invalid Token"))

        if len(parts) == 1:
            return self.send_body(layer["metadata"])
        if parts[1:] == ["query"]:
            if layer_id == ERROR_LAYER or stats["fail_queries"]:
                return self.send_body(error_body(400, "Unable to complete operation.", ["Invalid query parameters."]))
            return self.send_body(run_query(layer, params))
        return self.send_body(error_body(400, "Invalid URL"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=0)
    args = parser.parse_args()
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"READY {server.server_address[1]}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
