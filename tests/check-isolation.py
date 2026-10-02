#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Optional real sandbox test: ABI load only, never expose a sensor or /sys."""
import argparse
import importlib.util
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("runner", ROOT / "scripts/run-baseline.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--bwrap", default="bwrap")
parser.add_argument("--dependency-dir", type=Path, help="Optional unpacked OS libraries for this ABI-only test")
args = parser.parse_args()
stage = ROOT / ".baseline/build"
cmd = runner.sandbox_command(stage, "--check-abi", sensor=False)
cmd[0] = args.bwrap
if args.dependency_dir:
    cmd[-3:-3] = ["--ro-bind", str(args.dependency_dir.resolve(strict=True)), "/test-dependencies",
                  "--setenv", "LD_LIBRARY_PATH", "/test-dependencies"]
result = subprocess.run(cmd, env=runner.SAFE_ENV, capture_output=True, text=True, timeout=20)
print(result.stdout, end="")
if result.returncode or result.stdout.strip() != "MEDION_BASELINE:ABI_OK":
    # bwrap's own setup errors contain no sensor data in this no-device test.
    print(result.stderr[:2000])
    raise SystemExit(1)
print("PASS: archived library ABI loads inside the actual no-device sandbox")
