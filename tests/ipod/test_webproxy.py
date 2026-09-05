#!/usr/bin/env python3
"""Real sockets exercise guestfwd's stdio protocol without internet access."""
import concurrent.futures
import http.server
import pathlib
import socket
import subprocess
import tempfile
import threading

ROOT = pathlib.Path(__file__).resolve().parents[2]

class Origin(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith('/web/20090909id_/'):
            self.send_response(302)
            self.send_header('Location', self.path.replace('20090909id_', '20090910000000id_'))
            self.end_headers()
            return
        if self.path.startswith('/web/'):
            assert 'Cookie' not in self.headers and 'Authorization' not in self.headers
            self.send_response(200)
            self.send_header('Set-Cookie', 'archive=must-not-leak')
            self.end_headers()
            self.wfile.write(b'ARCHIVED ORIGINAL PAGE')
            return
        self.send_response(200)
        self.send_header('Transfer-Encoding', 'chunked')
        self.end_headers()
        self.wfile.write(b'5\r\nhello\r\n0\r\n\r\n')
    def do_POST(self):
        body = self.rfile.read(int(self.headers['Content-Length']))
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *args):
        pass

with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    exe = tmp / 'proxy'
    subprocess.run(['cc', '-g', '-fsanitize=address,undefined', '-Wno-deprecated-declarations',
                    str(ROOT / 'contrib/it-webproxy/itwebproxy.c'), '-lcurl', '-o', str(exe)], check=True)
    config = tmp / 'configuration with spaces'
    config.write_text('direct\n')
    origin = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Origin)
    threading.Thread(target=origin.serve_forever, daemon=True).start()
    url = f'http://127.0.0.1:{origin.server_port}/test'
    def request(data):
        p = subprocess.run([str(exe), str(config)], input=data, capture_output=True, timeout=10)
        assert b'AddressSanitizer' not in p.stderr and b'runtime error:' not in p.stderr, p.stderr
        return p.stdout
    def get(_):
        result = request(f'GET {url} HTTP/1.1\r\nHost: ignored\r\n\r\n'.encode())
        assert result.endswith(b'\r\n\r\nhello'), result
        assert b'Transfer-Encoding' not in result and b'Connection: close' in result
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        list(pool.map(get, range(24)))
    result = request(f'POST {url} HTTP/1.0\r\nContent-Length: 4\r\n\r\n'.encode() + b'a\0bc')
    assert result.endswith(b'a\0bc'), result
    for header, status in [('Content-Length: 1\r\nContent-Length: 1', b'400'),
                           ('Content-Length: -1', b'400'), ('Content-Length: 9000000', b'413'),
                           ('Transfer-Encoding: chunked', b'501'), ('Expect: 100-continue', b'417')]:
        assert status in request(f'POST {url} HTTP/1.1\r\n{header}\r\n\r\n'.encode()).split(b'\r\n')[0]
    assert b'431' in request(b'GET / HTTP/1.0\r\nX: ' + b'a' * 65536).split(b'\r\n')[0]
    # CONNECT is a real bidirectional tunnel, including a client half-close.
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0)); listener.listen()
    def echo():
        conn, _ = listener.accept()
        with conn:
            data = bytearray()
            while part := conn.recv(1024): data.extend(part)
            conn.sendall(bytes(data))
    thread = threading.Thread(target=echo); thread.start()
    port = listener.getsockname()[1]
    result = request(f'CONNECT 127.0.0.1:{port} HTTP/1.0\r\n\r\n'.encode() + b'tunnel\0bytes')
    assert result.endswith(b'\r\n\r\ntunnel\0bytes'), result
    thread.join(); listener.close()
    # External mode preserves absolute URI, response bytes and arbitrary methods.
    config.write_text(f'upstream\n127.0.0.1\n{origin.server_port}\n')
    assert request(f'GET {url} HTTP/1.0\r\n\r\n'.encode()).endswith(b'5\r\nhello\r\n0\r\n\r\n')
    # A built-in archive replay follows canonical snapshot redirects on the
    # host and sends neither guest cookies nor credentials to the archive.
    subprocess.run(['cc', '-g', '-fsanitize=address,undefined', '-Wno-deprecated-declarations',
                    f'-DARCHIVE_ORIGIN="http://127.0.0.1:{origin.server_port}"',
                    str(ROOT / 'contrib/it-webproxy/itwebproxy.c'), '-lcurl', '-o', str(exe)], check=True)
    config.write_text('archive\n20090909\n')
    archived = request(b'GET http://example.invalid/page HTTP/1.0\r\nCookie: secret=value\r\nAuthorization: Basic secret\r\n\r\n')
    assert archived.count(b'HTTP/1.0') == 1 and b'200 Archive response' in archived, archived
    assert archived.endswith(b'ARCHIVED ORIGINAL PAGE') and b'Set-Cookie' not in archived, archived
    assert b'405' in request(b'POST http://example.invalid/page HTTP/1.0\r\n\r\n')
    assert b'400' in request(b'GET http://name:password@example.invalid/page HTTP/1.0\r\n\r\n')
    config.write_text('archive\ninvalid\n')
    assert b'503' in request(b'GET http://example.invalid/ HTTP/1.0\r\n\r\n')
    config.write_text('off\n')
    assert b'503' in request(b'GET / HTTP/1.0\r\n\r\n')
    origin.shutdown(); origin.server_close()
print('PASS: concurrent HTTP, dechunking, binary POST, malformed framing, CONNECT half-close, upstream, built-in archive redirects/privacy and off')
