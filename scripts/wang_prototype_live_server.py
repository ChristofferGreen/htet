#!/usr/bin/env python3
"""Local-only rebuild endpoint for the interactive Wang prototype."""

from __future__ import annotations

import argparse
from collections import OrderedDict
import json
import subprocess
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse


ROOT = Path(__file__).resolve().parents[1]
ARTIFACTS = ROOT / "artifacts" / "wang-four-hexahedra-prototype"
EXPORTER = ROOT / "build-owned" / "wang_prototype_demo_export"
REBUILD_LOCK = threading.Lock()
GENERATED_ARTIFACTS = (
    "01-four-hexahedra.vtk",
    "02-dual-contour-surface.vtk",
    "03-implicit-tetrahedral-core.vtk",
    "04-wang-transition.vtk",
    "05-complete-prototype.vtk",
    "prototype-data.js",
    "summary.json",
)
# A parent tetrahedron is the first useful invalidation unit for the runtime:
# unchanged parents can reuse their complete validated Wang transaction while
# changed parents still take the authoritative recovery path.  Four entries
# are enough to compare nearby LOD states without allowing unbounded growth.
RESULT_CACHE: OrderedDict[tuple[object, ...], dict[str, bytes]] = OrderedDict()
MAXIMUM_CACHED_RESULTS = 4


def exporter_fingerprint() -> tuple[int, int]:
    status = EXPORTER.stat()
    return status.st_mtime_ns, status.st_size


def request_key(resolution: int, mode: str, minimum_level: int,
                surface_band: float) -> tuple[object, ...]:
    return (*exporter_fingerprint(), resolution, mode, minimum_level,
            float.hex(surface_band))


def snapshot_artifacts() -> dict[str, bytes] | None:
    if not all((ARTIFACTS / name).is_file() for name in GENERATED_ARTIFACTS):
        return None
    return {name: (ARTIFACTS / name).read_bytes() for name in GENERATED_ARTIFACTS}


def current_artifacts_match(resolution: int, mode: str, minimum_level: int,
                            surface_band: float) -> bool:
    try:
        summary = json.loads((ARTIFACTS / "summary.json").read_text())
    except (OSError, json.JSONDecodeError):
        return False
    invariants_hold = (
        summary.get("dc_closed_two_manifold") is True
        and summary.get("retained_core_preserved") is True
        and summary.get("no_strict_tetrahedron_overlap") is True
    )
    return (
        invariants_hold
        and summary.get("grid_resolution") == resolution
        and summary.get("core_mode") == mode
        and summary.get("core_min_red_depth") == minimum_level
        and abs(float(summary.get("core_surface_band", -1.0)) - surface_band) < 1e-12
        and snapshot_artifacts() is not None
    )


def remember_result(key: tuple[object, ...], artifacts: dict[str, bytes]) -> None:
    RESULT_CACHE[key] = artifacts
    RESULT_CACHE.move_to_end(key)
    while len(RESULT_CACHE) > MAXIMUM_CACHED_RESULTS:
        RESULT_CACHE.popitem(last=False)


def restore_result(artifacts: dict[str, bytes]) -> None:
    for name, body in artifacts.items():
        destination = ARTIFACTS / name
        temporary = destination.with_name(f".{destination.name}.cache-restore")
        temporary.write_bytes(body)
        temporary.replace(destination)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, _format: str, *_args: object) -> None:
        return

    def reply(self, status: HTTPStatus, payload: dict[str, object]) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802
        request = urlparse(self.path)
        if request.path == "/health":
            self.reply(HTTPStatus.OK, {"ready": EXPORTER.is_file()})
            return
        if request.path != "/rebuild":
            self.serve_artifact(request.path)
            return
        try:
            resolution = int(parse_qs(request.query).get("resolution", [""])[0])
        except ValueError:
            resolution = 0
        if resolution not in range(4, 13):
            self.reply(HTTPStatus.BAD_REQUEST, {"error": "resolution must be 4 through 12"})
            return
        mode = parse_qs(request.query).get("mode", ["uniform"])[0]
        if mode not in {"uniform", "adaptive"}:
            self.reply(HTTPStatus.BAD_REQUEST, {"error": "mode must be uniform or adaptive"})
            return
        try:
            minimum_level = int(parse_qs(request.query).get("minimum_level", ["2"])[0])
            surface_band = float(parse_qs(request.query).get("surface_band", ["0.5"])[0])
        except ValueError:
            self.reply(HTTPStatus.BAD_REQUEST, {"error": "invalid adaptive LOD setting"})
            return
        if minimum_level not in range(0, 7) or not 0.01 <= surface_band <= 4.0:
            self.reply(HTTPStatus.BAD_REQUEST, {"error": "adaptive LOD settings are out of range"})
            return
        if not EXPORTER.is_file():
            self.reply(HTTPStatus.SERVICE_UNAVAILABLE, {"error": "build the Wang exporter first"})
            return
        reuse = parse_qs(request.query).get("reuse", ["1"])[0] != "0"
        started = time.perf_counter()
        cache_hit = False
        completed: subprocess.CompletedProcess[str] | None = None
        with REBUILD_LOCK:
            key = request_key(resolution, mode, minimum_level, surface_band)
            cached = RESULT_CACHE.get(key) if reuse else None
            if cached is None and reuse and current_artifacts_match(
                    resolution, mode, minimum_level, surface_band):
                cached = snapshot_artifacts()
                if cached is not None:
                    remember_result(key, cached)
            if cached is not None:
                restore_result(cached)
                RESULT_CACHE.move_to_end(key)
                cache_hit = True
            else:
                completed = subprocess.run(
                    [str(EXPORTER), str(ARTIFACTS), str(resolution), mode,
                     str(minimum_level), str(surface_band)],
                    # A geometrically scaled depth-six core is intentionally much
                    # denser than the former fixed-cavity demo.  Keep the browser
                    # request alive while the bounded CPU oracle completes rather
                    # than reporting a false rebuild failure at two minutes.
                    cwd=ROOT, text=True, capture_output=True, timeout=300, check=False,
                )
                if completed.returncode == 0 and reuse:
                    generated = snapshot_artifacts()
                    if generated is not None:
                        remember_result(key, generated)
        if completed is not None and completed.returncode:
            self.reply(HTTPStatus.UNPROCESSABLE_ENTITY, {
                "error": completed.stderr.strip() or "Wang prototype rebuild failed",
            })
            return
        elapsed_milliseconds = (time.perf_counter() - started) * 1000.0
        self.reply(HTTPStatus.OK, {"resolution": resolution, "mode": mode,
                                   "minimum_level": minimum_level,
                                   "surface_band": surface_band,
                                   "cache_hit": cache_hit,
                                   "elapsed_milliseconds": elapsed_milliseconds,
                                   "reloaded": True})

    def serve_artifact(self, request_path: str) -> None:
        """Serve the inspector alongside its rebuild endpoint.

        Keeping both on one local origin means opening the inspector is enough
        for its resolution control to work; no second, easy-to-miss server is
        needed.
        """
        relative = "interactive-inspector.html" if request_path in {"", "/"} else request_path.lstrip("/")
        candidate = (ARTIFACTS / relative).resolve()
        if ARTIFACTS not in candidate.parents or not candidate.is_file():
            self.reply(HTTPStatus.NOT_FOUND, {"error": "not found"})
            return
        content_types = {
            ".html": "text/html; charset=utf-8",
            ".js": "application/javascript; charset=utf-8",
            ".json": "application/json; charset=utf-8",
            ".css": "text/css; charset=utf-8",
            ".svg": "image/svg+xml",
            ".png": "image/png",
        }
        body = candidate.read_bytes()
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", content_types.get(candidate.suffix, "application/octet-stream"))
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8766)
    args = parser.parse_args()
    ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
