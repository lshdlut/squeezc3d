#!/usr/bin/env python3

from __future__ import annotations

import argparse
import asyncio
import base64
import json
import os
import socket
import threading
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Iterable, List

from playwright.async_api import async_playwright


class _WasmRootHandler(SimpleHTTPRequestHandler):
    def guess_type(self, path: str) -> str:
        if path.endswith(".wasm"):
            return "application/wasm"
        return super().guess_type(path)


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


def _start_static_server(root: Path, port: int) -> tuple[ThreadingHTTPServer, threading.Thread]:
    if not root.exists():
        raise RuntimeError(f"Missing server root: {root}")
    handler = partial(_WasmRootHandler, directory=str(root))
    httpd = ThreadingHTTPServer(("127.0.0.1", port), handler)
    t = threading.Thread(target=httpd.serve_forever, daemon=True)
    t.start()
    return httpd, t


def _iter_c3d_files(c3d_files: Iterable[Path], c3d_dir: Path | None, limit: int) -> List[Path]:
    out: List[Path] = []
    for p in c3d_files:
        out.append(Path(p))
    if out:
        return out
    if c3d_dir is None:
        raise RuntimeError("Missing C3D input. Pass --c3d-file or --c3d-dir (or set C3D_FILE/C3D_DIR).")
    if not c3d_dir.exists():
        raise RuntimeError(f"Missing C3D dir: {c3d_dir}")
    files = sorted(c3d_dir.rglob("*.c3d"))
    if not files:
        raise RuntimeError(f"No .c3d files under: {c3d_dir}")
    return files[: max(1, int(limit))]


def _parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser()
    p.add_argument("--wasm-dir", type=Path, default=os.environ.get("SQZC3D_WASM_DIR"))
    p.add_argument("--c3d-file", type=Path, action="append", default=[])
    p.add_argument("--c3d-dir", type=Path, default=os.environ.get("C3D_DIR"))
    p.add_argument("--limit", type=int, default=int(os.environ.get("C3D_LIMIT", "5")))
    p.add_argument("--headless", action=argparse.BooleanOptionalAction, default=True)
    p.add_argument("--port", type=int, default=0)
    return p.parse_args()


def _write_html(root: Path, html_name: str) -> Path:
    html_path = root / html_name
    html_content = """<!doctype html>
<html>
  <head>
    <meta charset="utf-8" />
    <script src="/sqzc3d.js"></script>
    <script>
      function _cstr(Module, ptr) {
        if (!ptr) return "";
        const u8 = Module.HEAPU8;
        let end = ptr;
        while (u8[end] !== 0) end++;
        return new TextDecoder("utf-8").decode(u8.subarray(ptr, end));
      }

      function _utf8z(s) {
        const u8 = new TextEncoder().encode(s);
        const z = new Uint8Array(u8.length + 1);
        z.set(u8, 0);
        z[z.length - 1] = 0;
        return z;
      }

      function _decodeB64(b64) {
        const bin = atob(b64);
        const bytes = new Uint8Array(bin.length);
        for (let i = 0; i < bin.length; ++i) bytes[i] = bin.charCodeAt(i);
        return bytes;
      }

      window._run = async (payload) => {
        const Module = await SQZC3DModule();
        if (!Module) throw new Error("SQZC3DModule resolved to null");

        const needFns = [
          "_malloc", "_free",
          "_sqzc3d_open_file", "_sqzc3d_open_memory", "_sqzc3d_close_dec",
          "_sqzc3d_last_error",
          "_sqzc3d_default_build_opt", "_sqzc3d_build_chunks",
          "_sqzc3d_chunk_num_frames", "_sqzc3d_chunk_num_points", "_sqzc3d_chunk_num_scalar",
          "_sqzc3d_free_chunk",
        ];
        for (const k of needFns) {
          if (typeof Module[k] !== "function") {
            throw new Error(`Missing export: ${k}`);
          }
        }

        const bytes = _decodeB64(payload.b64);
        const fileName = String(payload.fileName || "input.c3d");
        const path = `/mem/${fileName}`;

        const hasFSWriteFile = !!(Module.FS && typeof Module.FS.writeFile === "function");
        const hasFSCreateDataFile = typeof Module.FS_createPath === "function"
          && typeof Module.FS_createDataFile === "function"
          && typeof Module.FS_unlink === "function";
        if (!hasFSWriteFile && !hasFSCreateDataFile) {
          throw new Error("No FS.writeFile / FS_createDataFile available");
        }

        // Best-effort ensure /mem exists.
        try {
          if (Module.FS && typeof Module.FS.mkdir === "function") {
            try { Module.FS.mkdir("/mem"); } catch (e) { /* already exists */ }
          } else {
            try { Module.FS_createPath("/", "mem", true, true); } catch (e) { /* already exists */ }
          }
        } catch (e) { /* best-effort */ }

        if (hasFSWriteFile) {
          Module.FS.writeFile(path, bytes);
        } else {
          try { Module.FS_unlink(path); } catch (e) { /* best-effort */ }
          Module.FS_createDataFile("/mem", fileName, bytes, true, true, true);
        }

        const p_dec = Module._malloc(4);
        const p_chunk = Module._malloc(4);
        const p_build_opt = Module._malloc(2048);
        const p_data = Module._malloc(bytes.length);
        const pathBytes = _utf8z(path);
        const p_path = Module._malloc(pathBytes.length);
        if (!p_dec || !p_chunk || !p_build_opt || !p_data || !p_path) {
          throw new Error("malloc failed");
        }

        Module.HEAPU8.set(bytes, p_data);
        Module.HEAPU8.set(pathBytes, p_path);

        const out = { file: fileName, open_file: null, open_memory: null };

        const runOpen = (mode, openFn) => {
          const r = {
            mode,
            status_open: null,
            last_error_open: null,
            status_build: null,
            last_error_build: null,
            frames: null,
            points: null,
            scalar: null,
          };
          Module.HEAP32[p_dec >> 2] = 0;
          Module.HEAP32[p_chunk >> 2] = 0;

          const stOpen = openFn() | 0;
          r.status_open = stOpen;
          if (stOpen !== 0) {
            r.last_error_open = _cstr(Module, Module._sqzc3d_last_error(0));
            return r;
          }

          const dec = Module.HEAP32[p_dec >> 2] | 0;
          if (!dec) {
            r.status_open = -1;
            r.last_error_open = "decoder handle is 0";
            return r;
          }

          try {
            Module._sqzc3d_default_build_opt(p_build_opt);
            // Only read a single frame by default to keep this smoke fast.
            // sqzc3d_build_opt_t: struct_size (i32), frame_range.start (i32), frame_range.count (i32)
            Module.HEAP32[(p_build_opt + 4) >> 2] = 0;
            Module.HEAP32[(p_build_opt + 8) >> 2] = 1;

            const stBuild = Module._sqzc3d_build_chunks(dec, p_build_opt, p_chunk) | 0;
            r.status_build = stBuild;
            if (stBuild !== 0) {
              r.last_error_build = _cstr(Module, Module._sqzc3d_last_error(dec));
              return r;
            }
            const chunk = Module.HEAP32[p_chunk >> 2] | 0;
            if (!chunk) {
              r.status_build = -2;
              r.last_error_build = "chunk handle is 0";
              return r;
            }
            r.frames = Module._sqzc3d_chunk_num_frames(chunk) | 0;
            r.points = Module._sqzc3d_chunk_num_points(chunk) | 0;
            r.scalar = Module._sqzc3d_chunk_num_scalar(chunk) | 0;
            Module._sqzc3d_free_chunk(chunk);
            return r;
          } finally {
            Module._sqzc3d_close_dec(dec);
          }
        };

        try {
          out.open_file = runOpen("open_file", () => Module._sqzc3d_open_file(p_dec, p_path, 0));
          out.open_memory = runOpen("open_memory", () => Module._sqzc3d_open_memory(p_dec, p_data, bytes.length, 0));
          return out;
        } finally {
          Module._free(p_path);
          Module._free(p_data);
          Module._free(p_build_opt);
          Module._free(p_chunk);
          Module._free(p_dec);
        }
      };
    </script>
  </head>
  <body>ready</body>
</html>
"""
    html_path.write_text(html_content, encoding="utf-8")
    return html_path


async def _run() -> int:
    args = _parse_args()
    if args.wasm_dir is None:
        raise RuntimeError("Missing --wasm-dir (or set SQZC3D_WASM_DIR).")
    wasm_dir = Path(args.wasm_dir)
    if not wasm_dir.exists():
        raise RuntimeError(f"Missing wasm dir: {wasm_dir}")
    if not (wasm_dir / "sqzc3d.js").exists():
        raise RuntimeError(f"Missing sqzc3d.js under: {wasm_dir}")

    c3d_file_env = os.environ.get("C3D_FILE")
    c3d_files = list(args.c3d_file)
    if not c3d_files and c3d_file_env:
        c3d_files = [Path(c3d_file_env)]

    c3d_paths = _iter_c3d_files(c3d_files, args.c3d_dir, int(args.limit))

    port = int(args.port) if int(args.port) > 0 else _free_port()
    httpd, t = _start_static_server(wasm_dir, port)

    html_name = "_playwright_sqzc3d_open_modes.html"
    html_path = _write_html(wasm_dir, html_name)
    url = f"http://127.0.0.1:{port}/{html_path.name}"

    failures = 0
    try:
        async with async_playwright() as p:
            browser = await p.chromium.launch(headless=bool(args.headless))
            page = await browser.new_page()
            page.on("pageerror", lambda exc: print("PAGEERROR", exc))
            await page.goto(url)

            for fp in c3d_paths:
                data = Path(fp).read_bytes()
                payload = {
                    "fileName": Path(fp).name,
                    "b64": base64.b64encode(data).decode("ascii"),
                }
                res = await page.evaluate("(payload) => window._run(payload)", payload)
                open_file_ok = int(res["open_file"]["status_open"]) == 0 and int(res["open_file"]["status_build"]) == 0
                open_mem_ok = int(res["open_memory"]["status_open"]) == 0 and int(res["open_memory"]["status_build"]) == 0
                if not (open_file_ok and open_mem_ok):
                    failures += 1

                summary = {
                    "file": res.get("file"),
                    "open_file": res.get("open_file"),
                    "open_memory": res.get("open_memory"),
                }
                print(json.dumps(summary, ensure_ascii=True))

            await browser.close()
    finally:
        httpd.shutdown()
        t.join(timeout=2.0)
        try:
            html_path.unlink(missing_ok=True)
        except Exception:
            pass

    return 1 if failures else 0


def main() -> None:
    raise SystemExit(asyncio.run(_run()))


if __name__ == "__main__":
    main()

