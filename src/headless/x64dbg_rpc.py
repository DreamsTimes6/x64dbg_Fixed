#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
x64dbg headless JSON-RPC client.

A minimal client for the `headless.exe -rpc` mode added in this tree.

Protocol (JSON Lines):
  request  ->  {"ping":true}
               {"get":"state"}
               {"eval":"<x64dbg expression>"}
               {"cmd":"<x64dbg command>"}            # async; ok means enqueued
               {"wait":"<state>","timeout":ms}       # block until state matches
               {"exit":true}
  response ->  {"ok":true,...} / {"ok":false,"err":"..."}
  events   ->  {"log":"..."}                         # debugger log line
               {"event":"state","state":"..."}       # debugger state change

Usage:
    rpc = X64dbgRpc(arch="x64", userdir=r"C:\\tmp\\ud")
    with rpc:
        rpc.cmd("init C:\\targets\\app.exe")
        rpc.wait("initialized")
        rpc.cmd("bp app:func")
        rpc.cmd("run")
        rpc.wait("paused")
        eip = rpc.eval("cip")
        print(hex(eip))
"""

from __future__ import annotations

import json
import os
import subprocess
import time

# Default location relative to this file: <repo>/src/bin/<arch>/headless.exe
_DEFAULT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


class RpcError(RuntimeError):
    pass


class X64dbgRpc:
    """Spawns headless.exe -rpc and speaks JSON Lines over stdio."""

    def __init__(self, arch: str = "x64", userdir: str | None = None,
                 plugins: list[str] | None = None, headless: str | None = None,
                 cwd: str | None = None):
        self.arch = arch
        if headless is None:
            headless = os.path.join(_DEFAULT_ROOT, "bin", arch, "headless.exe")
        self.headless = os.path.abspath(headless)
        self.userdir = userdir or os.path.join(_DEFAULT_ROOT, "bin", arch, ".rpc-userdir")
        self.plugins = plugins or []
        self.cwd = cwd or os.path.dirname(self.headless)
        self._proc: subprocess.Popen | None = None

    # -- lifecycle -------------------------------------------------------
    def __enter__(self) -> "X64dbgRpc":
        self.start()
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def start(self, timeout: float = 30.0) -> None:
        os.makedirs(self.userdir, exist_ok=True)
        args = [self.headless, "-rpc", "-userdir", self.userdir]
        for plug in self.plugins:
            args += ["-plugin", plug]
        self._proc = subprocess.Popen(
            args, cwd=self.cwd,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True,
            bufsize=1,  # line buffered
        )
        # Wait for the hello line.
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self._readline()
            obj = self._parse(line)
            if obj is not None and "hello" in obj:
                return
        raise RpcError("headless did not announce readiness")

    def close(self) -> None:
        if self._proc is None:
            return
        try:
            if self._proc.poll() is None:
                self._write({"exit": True})
                time.sleep(0.2)
        finally:
            try:
                self._proc.stdin.close()
            except Exception:
                pass
            try:
                self._proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self._proc.kill()

    # -- low-level -------------------------------------------------------
    def _readline(self) -> str:
        assert self._proc and self._proc.stdout
        line = self._proc.stdout.readline()
        return line

    @staticmethod
    def _parse(line: str):
        line = line.strip()
        if not line:
            return None
        try:
            return json.loads(line)
        except json.JSONDecodeError:
            return {"log": line}  # tolerate stray non-JSON noise

    def _write(self, obj: dict) -> None:
        assert self._proc and self._proc.stdin
        self._proc.stdin.write(json.dumps(obj) + "\n")
        self._proc.stdin.flush()

    def _response(self, timeout: float = 30.0) -> dict:
        """Read lines until an ok/fail response (log/event lines are drained
        and returned in `events`)."""
        deadline = time.time() + timeout
        events: list[dict] = []
        while time.time() < deadline:
            obj = self._parse(self._readline())
            if obj is None:
                continue
            if "ok" in obj:
                obj["_events"] = events
                return obj
            events.append(obj)
        raise RpcError("timed out waiting for a response")

    # -- high-level API ---------------------------------------------------
    def ping(self, timeout: float = 10.0) -> bool:
        self._write({"ping": True})
        return bool(self._response(timeout).get("pong"))

    def get_state(self, timeout: float = 10.0) -> dict:
        self._write({"get": "state"})
        return self._response(timeout)

    def wait(self, state: str, timeout: float = 30000.0) -> dict:
        """Block until the debugger reaches `state` (e.g. 'paused' after run)."""
        self._write({"wait": state, "timeout": int(timeout * 1000)})
        resp = self._response(timeout / 1000 + 5)
        if not resp.get("ok"):
            raise RpcError(resp.get("err", "wait failed"))
        return resp

    def eval(self, expr: str, timeout: float = 15.0):
        """Evaluate an x64dbg expression (cip, [mem], registers, math, ...)."""
        self._write({"eval": expr})
        resp = self._response(timeout)
        if not resp.get("ok"):
            raise RpcError(resp.get("err", f"eval failed: {expr}"))
        return resp["value"]

    def cmd(self, command: str, timeout: float = 15.0, sync: bool = False) -> dict:
        """Execute an x64dbg command.

        sync=False (default): async, `ok` means enqueued; results follow as
            {"log":...}/{"event":...} lines.
        sync=True: blocks until the command finished (DbgCmdExecDirect). Use
            for step/stepinto/stepover/run where you need the result now.
        """
        req = {"cmd": command}
        if sync:
            req["sync"] = True
            timeout = max(timeout, 120.0)  # sync run/step can take a while
        self._write(req)
        resp = self._response(timeout)
        if not resp.get("ok"):
            raise RpcError(resp.get("err", f"command failed: {command}"))
        return resp


def _demo() -> None:
    """End-to-end demo: init -> breakpoint -> run -> read state/memory -> step."""
    import tempfile

    ud = os.path.join(tempfile.gettempdir(), "x64dbg-rpc-demo")
    with X64dbgRpc(arch="x64", userdir=ud,
                   plugins=[r"tests\membp\membp.dp64"]) as rpc:
        print("ping:", rpc.ping())
        print("state:", rpc.get_state())
        rpc.cmd("init tests/membp.exe")
        rpc.wait("initialized")
        rpc.cmd("bp membp:ReadSequence")
        rpc.cmd("run")
        rpc.wait("paused")
        print("state:", rpc.get_state())
        print("cip =", hex(rpc.eval("cip")))
        target = rpc.eval("membp:ReadTarget")
        print("ReadTarget @", hex(target), "= 0x%X" % rpc.eval(f"byte:[{target:#x}]"))
        rpc.cmd("step", sync=True)  # synchronous: returns after the step finished
        print("after sync step, cip =", hex(rpc.eval("cip")))
        rpc.cmd("step", sync=True)
        print("after 2nd sync step, cip =", hex(rpc.eval("cip")))
    print("demo done")


if __name__ == "__main__":
    _demo()
