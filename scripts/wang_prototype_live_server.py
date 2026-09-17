#!/usr/bin/env python3
"""Local-only rebuild endpoint for the interactive Wang prototype."""

from __future__ import annotations

import argparse
import json
import subprocess
import threading
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse


ROOT = Path(__file__).resolve().parents[1]
ARTIFACTS = ROOT / "artifacts" / "wang-four-hexahedra-prototype"
EXPORTER = ROOT / "build-owned" / "wang_prototype_demo_export"
REBUILD_LOCK = threading.Lock()


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
            self.reply(HTTPStatus.NOT_FOUND, {"error": "not found"})
            return
        try:
            resolution = int(parse_qs(request.query).get("resolution", [""])[0])
        except ValueError:
            resolution = 0
        if resolution not in range(4, 13):
            self.reply(HTTPStatus.BAD_REQUEST, {"error": "resolution must be 4 through 12"})
            return
        if not EXPORTER.is_file():
            self.reply(HTTPStatus.SERVICE_UNAVAILABLE, {"error": "build the Wang exporter first"})
            return
        with REBUILD_LOCK:
            completed = subprocess.run(
                [str(EXPORTER), str(ARTIFACTS), str(resolution)],
                # A geometrically scaled depth-six core is intentionally much
                # denser than the former fixed-cavity demo.  Keep the browser
                # request alive while the bounded CPU oracle completes rather
                # than reporting a false rebuild failure at two minutes.
                cwd=ROOT, text=True, capture_output=True, timeout=300, check=False,
            )
        if completed.returncode:
            self.reply(HTTPStatus.UNPROCESSABLE_ENTITY, {
                "error": completed.stderr.strip() or "Wang prototype rebuild failed",
            })
            return
        self.reply(HTTPStatus.OK, {"resolution": resolution, "reloaded": True})


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8767)
    args = parser.parse_args()
    ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
