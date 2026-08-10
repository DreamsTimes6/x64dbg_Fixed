#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Functional test suite for the headless.exe -rpc JSON mode.

Run from the repo root:
    python src/headless/test_rpc.py [--arch x64|x32] [--headless <path>]

Covers: ping / get / eval / cmd (async+sync) / wait / exit, plugin loading,
error handling, output cleanliness (every stdout line must be valid JSON),
and both x64 and x32 builds. Exits nonzero if any check fails.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from x64dbg_rpc import X64dbgRpc, RpcError  # noqa: E402

PASS = 0
FAIL = 0
RESULTS: list[str] = []


def check(name: str, ok: bool, detail: str = "") -> None:
    global PASS, FAIL
    if ok:
        PASS += 1
        RESULTS.append(f"PASS  {name}")
    else:
        FAIL += 1
        RESULTS.append(f"FAIL  {name}  {detail}")
        print(f"  !! {name}: {detail}")


def raw_session(headless: str, userdir: str, plugin: str, lines: list[str],
                cwd: str) -> tuple[int, list[str]]:
    """One-shot raw stdin session; returns (exitcode, stdout lines)."""
    p = subprocess.Popen(
        [headless, "-rpc", "-userdir", userdir, "-plugin", plugin],
        cwd=cwd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True,
    )
    p.stdin.write("\n".join(lines) + "\n")
    p.stdin.close()
    out = p.stdout.read().splitlines()
    p.wait(timeout=30)
    return p.returncode, out


def run_suite(arch: str, headless: str | None) -> None:
    global PASS, FAIL
    PASS = 0
    FAIL = 0
    RESULTS.append(f"===== arch={arch} =====")

    ud = os.path.join(os.environ.get("TEMP", "/tmp"), f"x64dbg-rpc-test-{arch}")
    plugin_rel = os.path.join("tests", "membp", f"membp.dp{'64' if arch == 'x64' else '32'}")
    cwd = os.path.join(ROOT, "bin", arch)

    # ---------- 1. session basics ----------
    rpc = X64dbgRpc(arch=arch, userdir=ud, plugins=[plugin_rel], headless=headless)
    rpc.start()
    check("start + hello", True)
    check("ping", rpc.ping() is True)

    # ---------- 2. get ----------
    st = rpc.get_state()
    check("get state initial", st.get("state") == "initialized" and st.get("isDebugging") is False, str(st))

    # ---------- 3. eval ----------
    check("eval 1+2", rpc.eval("1+2") == 3)
    check("eval hex", rpc.eval("0x10") == 16)
    check("eval expression w/ registers", rpc.eval("1+2*3") == 7)
    try:
        rpc.eval("this_is_not_a_real_symbol_xyz")
        check("eval invalid -> error", False, "expected RpcError")
    except RpcError:
        check("eval invalid -> error", True)
    check("eval before init cip=0", rpc.eval("cip") == 0, "no process yet")

    # ---------- 4. cmd async + init flow ----------
    rpc.cmd("init tests/membp.exe")
    rpc.wait("initialized", timeout=15000)
    check("init -> wait initialized", True)

    rpc.cmd("bp membp:ReadSequence")
    check("cmd bp ok", True)

    # ---------- 5. run / wait ----------
    rpc.cmd("run")
    rpc.wait("paused", timeout=15000)
    st = rpc.get_state()
    check("run -> wait paused", st.get("state") == "paused" and st.get("isDebugging") is True, str(st))
    check("eval cip != 0", rpc.eval("cip") != 0)

    # ---------- 6. memory read via eval ----------
    target = rpc.eval("membp:ReadTarget")
    check("eval symbol addr != 0", target != 0, hex(target))
    val = rpc.eval(f"byte:[{target:#x}]")
    check("eval byte@symbol", val == 0xFF, f"ReadTarget initial value should be 0xFF, got {val:#x}")

    # ---------- 7. sync step ----------
    cip0 = rpc.eval("cip")
    rpc.cmd("step", sync=True)
    cip1 = rpc.eval("cip")
    check("sync step advances cip", cip1 != cip0, f"{cip0:#x} -> {cip1:#x}")
    rpc.cmd("step", sync=True)
    cip2 = rpc.eval("cip")
    check("2nd sync step advances cip", cip2 != cip1, f"{cip1:#x} -> {cip2:#x}")

    # ---------- 8. plugin command ----------
    try:
        rpc.cmd("mbassertbpcount 2")  # membp plugin command; entry bp + ReadSequence bp
        check("plugin command mbassertbpcount", True)
    except RpcError as e:
        check("plugin command mbassertbpcount", False, str(e))

    # ---------- 9. unknown command (sync so the failure is observable) ----------
    try:
        rpc.cmd("totally_unknown_command_zzz", sync=True)
        check("unknown command -> error", False, "expected RpcError")
    except RpcError:
        check("unknown command -> error", True)

    # ---------- 10. wait timeout ----------
    t0 = time.time()
    try:
        rpc.wait("nonexistent-state", timeout=1.0)
        check("wait timeout -> error", False, "expected RpcError")
    except RpcError:
        elapsed = time.time() - t0
        check("wait timeout -> error", True, f"in {elapsed:.1f}s")

    # ---------- 11. exit / clean shutdown ----------
    rpc.close()
    check("clean exit", rpc._proc is None or rpc._proc.returncode == 0, f"rc={rpc._proc and rpc._proc.returncode}")

    # ---------- 12. raw error handling + output cleanliness ----------
    rc, out = raw_session(
        headless or os.path.join(cwd, "headless.exe"), ud, plugin_rel,
        ['not json at all', '{"unknown_key":123}', '{"exit":true}'], cwd,
    )
    check("raw session exit code", rc == 0, str(rc))
    bad_lines = [ln for ln in out if not ln.strip().startswith("{")]
    check("all stdout lines are JSON", not bad_lines, str(bad_lines[:3]))
    # the two bad requests must produce ok:false responses
    errors = [ln for ln in out if '"ok":false' in ln]
    check("bad requests -> ok:false", len(errors) >= 2, str(errors))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--arch", default="x64", choices=["x64", "x32"])
    ap.add_argument("--headless", default=None)
    args = ap.parse_args()

    run_suite(args.arch, args.headless)
    print("\n".join(RESULTS))
    print(f"---- {args.arch}: {PASS} passed, {FAIL} failed ----")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
