#!/usr/bin/env python3
"""Protocol checks; --hardware additionally reads standard USB descriptors."""
import argparse
import os
from pathlib import Path
import secrets
import select
import socket
import struct
import subprocess

HEADER = struct.Struct('<IIIiII')
MAGIC = 0x42555041


def receive(sock, count):
    data = bytearray()
    while len(data) < count:
        block = sock.recv(count - len(data))
        if not block:
            raise EOFError('early server disconnect')
        data.extend(block)
    return bytes(data)


def rpc(sock, op, arg=0, payload=b''):
    sock.sendall(HEADER.pack(MAGIC, 1, op, 0, arg, len(payload)) + payload)
    magic, version, opcode, status, arg, length = HEADER.unpack(receive(sock, 24))
    assert (magic, version, opcode) == (MAGIC, 1, op)
    assert length <= 4 * 1024 * 1024
    return status, arg, receive(sock, length)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--hardware', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    token = secrets.token_hex(16)
    env = dict(os.environ, APUSB_TOKEN=token)
    log = root / 'research/reports/native-bridge-test.log'
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('w+') as output:
        process = subprocess.Popen([str(root/'build/apusb-bridge'), str(port)], env=env,
                                   stdout=output, stderr=subprocess.PIPE, text=True)
        try:
            ready, _, _ = select.select([process.stderr], [], [], 5)
            assert ready, 'server readiness timeout'
            first = process.stderr.readline()
            output.write(first)
            assert first.startswith('listening'), first

            def connect():
                return socket.create_connection(('127.0.0.1', port), timeout=5)

            with connect() as sock:
                assert rpc(sock, 1, payload=b'0'*32)[0] == -3
                assert sock.recv(1) == b''
            with connect() as sock:
                sock.sendall(HEADER.pack(MAGIC, 1, 1, 0, 0, 4*1024*1024+1))
                assert sock.recv(1) == b''
            with connect() as sock:
                assert rpc(sock, 1, payload=token.encode())[0] == 0
                setup = struct.pack('<BBHHHI', 0x80, 6, 0x0100, 0, 18, 2000)
                assert rpc(sock, 4, payload=setup)[0] == -4  # not opened
                assert rpc(sock, 3)[0] == 0
            print('PASS: authentication rejection, payload bounds, unopened-device error, close')
            if args.hardware:
                with connect() as sock:
                    assert rpc(sock, 1, payload=token.encode())[0] == 0
                    status, _, descriptors = rpc(sock, 2)
                    assert status == 0, f'open failed: {status}'
                    assert len(descriptors) >= 27
                    assert descriptors[:2] == bytes([18, 1])
                    assert struct.unpack_from('<HH', descriptors, 8) == (0x13e4, 3)
                    config_length = struct.unpack_from('<H', descriptors, 20)[0]
                    assert len(descriptors) == 18 + config_length
                    status, _, device = rpc(sock, 4, payload=setup)
                    assert status == 18 and device == descriptors[:18]
                    assert rpc(sock, 3)[0] == 0
                print('PASS: real adapter open, device/config descriptors, EP0 GET_DESCRIPTOR, close')
        finally:
            process.terminate()
            try:
                _, remaining = process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                _, remaining = process.communicate()
            output.write(remaining)


if __name__ == '__main__':
    main()
