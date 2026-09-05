#!/usr/bin/env python3
"""Offline TLS bridge checks: guest identity, encrypted errors, upstream trust."""
import concurrent.futures
import os
from pathlib import Path
import socket
import ssl
import subprocess
import tempfile
import threading
import warnings

ROOT = Path(__file__).resolve().parents[2]
PREFIX = Path(os.environ.get('OPENSSL_PREFIX', ROOT.parent / 'qemu-ios-deps12'))
with tempfile.TemporaryDirectory(prefix='it-proxy-tls-') as directory:
    work = Path(directory)
    exe = work / 'proxy'
    environment = dict(os.environ, ASAN_OPTIONS='log_path=' + str(work/'asan'), UBSAN_OPTIONS='log_path=' + str(work/'ubsan'))
    subprocess.run(['cc', '-g', '-fsanitize=address,undefined', '-DHAVE_OPENSSL',
        '-Wno-deprecated-declarations', '-I' + str(PREFIX / 'include'),
        str(ROOT / 'contrib/it-webproxy/itwebproxy.c'),
        str(PREFIX / 'lib/libssl.a'), str(PREFIX / 'lib/libcrypto.a'), '-lcurl', '-o', str(exe)], check=True)
    config = work / 'proxy.conf'
    config.write_text('direct\n')
    def initialize(_):
        subprocess.run([str(exe), '--init-ca', str(config)], check=True, env=environment)
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(initialize, range(4)))
    der = Path(str(config) + '.ca.der').read_bytes()
    initialize(0)
    assert Path(str(config) + '.ca.der').read_bytes() == der
    assert Path(str(config) + '.ca.pem').stat().st_mode & 0o777 == 0o600
    pem = ssl.DER_cert_to_PEM_cert(der)
    upstream = socket.socket()
    upstream.bind(('127.0.0.1', 0)); upstream.listen()
    upstream_port = upstream.getsockname()[1]
    server_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    server_context.set_ciphers('DEFAULT:@SECLEVEL=0')
    server_context.load_cert_chain(str(config) + '.ca.pem')
    def origin():
        conn, _ = upstream.accept()
        try:
            with server_context.wrap_socket(conn, server_side=True) as secure:
                secure.recv(4096)
                secure.sendall(b'HTTP/1.0 200 OK\r\n\r\nTHIS UNTRUSTED ORIGIN MUST NOT REACH THE GUEST')
        except (ssl.SSLError, OSError):
            conn.close()
    origin_thread = threading.Thread(target=origin, daemon=True)
    origin_thread.start()
    def request(data, trust=True, hostname='localhost'):
        client, child = socket.socketpair()
        client.settimeout(15)
        process = subprocess.Popen([str(exe), str(config)], stdin=child, stdout=child, stderr=subprocess.PIPE, env=environment)
        child.close()
        try:
            client.sendall(f'CONNECT localhost:{upstream_port} HTTP/1.0\r\n\r\n'.encode())
            header = b''
            while not header.endswith(b'\r\n\r\n'):
                part = client.recv(1)
                assert part, header
                header += part
            assert header.startswith(b'HTTP/1.0 200 '), header
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            context.set_ciphers('AES128-SHA:@SECLEVEL=0')
            with warnings.catch_warnings():
                warnings.simplefilter('ignore', DeprecationWarning)
                context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1
            if trust: context.load_verify_locations(cadata=pem)
            with context.wrap_socket(client, server_hostname=hostname) as secure:
                secure.sendall(data)
                response = bytearray()
                while part := secure.recv(16384): response.extend(part)
                return bytes(response)
        finally:
            client.close()
            process.wait(timeout=15)
    for trust, hostname in [(False, 'localhost'), (True, 'wrong.invalid')]:
        try:
            request(b'GET / HTTP/1.0\r\n\r\n', trust, hostname)
            raise AssertionError('Certificate verification must fail')
        except ssl.SSLCertVerificationError:
            pass
    result = request(b'POST / HTTP/1.0\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx')
    assert result.startswith(b'HTTP/1.0 400 '), result
    result = request(b'GET / HTTP/1.0\r\n\r\n')
    assert result.startswith(b'HTTP/1.0 502 ') and b'THIS UNTRUSTED' not in result, result
    origin_thread.join(timeout=5)
    assert not origin_thread.is_alive()
    upstream.close()
    config.write_text('off\n')
    echo_socket = socket.socket()
    echo_socket.bind(('127.0.0.1', 0)); echo_socket.listen()
    def echo():
        connection, _ = echo_socket.accept()
        with connection:
            body = bytearray()
            while part := connection.recv(1024): body.extend(part)
            connection.sendall(body)
    echo_thread = threading.Thread(target=echo)
    echo_thread.start()
    raw = subprocess.run([str(exe), str(config)],
        input=f'CONNECT 127.0.0.1:{echo_socket.getsockname()[1]} HTTP/1.0\r\n\r\nraw-tunnel'.encode(),
        capture_output=True, timeout=15, env=environment)
    assert raw.stdout.endswith(b'\r\n\r\nraw-tunnel'), raw.stdout
    echo_thread.join(timeout=5); echo_socket.close()
    private = Path(str(config) + '.ca.pem')
    private.chmod(0o644)
    assert subprocess.run([str(exe), '--init-ca', str(config)], env=environment).returncode != 0
    private.chmod(0o600)
    assert not list(work.glob('asan.*')) and not list(work.glob('ubsan.*')), 'Sanitizer reported an error'
print('PASS: concurrent per-device CA initialization, TLS 1.0, guest trust/name checks, encrypted framing errors, upstream certificate rejection')
