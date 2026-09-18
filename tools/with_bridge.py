#!/usr/bin/env python3
"""Run a command with a private native USB helper and per-run credentials.

Example: python3 tools/with_bridge.py /path/to/wine build/shim_smoke.exe
WINEPREFIX / CrossOver bottle settings are inherited from the caller.
"""
import argparse
from datetime import datetime, timezone
import os
from pathlib import Path
import secrets
import socket
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not args.command:
        parser.error('a Wine command is required')
    root = Path(__file__).resolve().parent.parent
    helper = root / 'build/apusb-bridge'
    if not helper.is_file():
        parser.error('build/apusb-bridge is missing; build it first')
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    env = dict(os.environ, APUSB_PORT=str(port), APUSB_TOKEN=secrets.token_hex(16))
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    log = root / 'research/reports' / f'bridge-{stamp}.log'
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('w') as output:
        server = subprocess.Popen([str(helper), str(port)], env=env,
                                  stdout=output, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 5
            while True:
                if server.poll() is not None:
                    raise RuntimeError(f'USB helper exited; see {log}')
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=0.2):
                        break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise RuntimeError(f'USB helper readiness timeout; see {log}')
                    time.sleep(0.05)
            print(f'USB helper ready; log: {log}', flush=True)
            return subprocess.call(args.command, env=env)
        finally:
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()


if __name__ == '__main__':
    raise SystemExit(main())
