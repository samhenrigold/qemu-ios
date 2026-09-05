# Guest HTTP proxy

`itwebproxy CONFIG` serves one slirp guestfwd connection on stdin/stdout; it
never opens a host listening port. Build with `bash build.sh` (system libcurl).
The configuration file is read at connection start:

```
direct
```

or, for an existing HTTP proxy such as WaybackProxy:

```
upstream
127.0.0.1
8888
```

`off` returns 503. Files can be atomically replaced while QEMU runs. Example:

```
-netdev 'user,id=wifi0,guestfwd=tcp:10.0.2.100:3128-cmd:/path/to/itwebproxy /path/to/config'
```

The app quotes both paths for the command shell and escapes QEMU option commas.
Direct mode resolves origin hosts on the Mac, accepts bounded HTTP requests,
closes each HTTP connection after its response, and supports end-to-end CONNECT.
CONNECT does not upgrade old guest TLS or install a certificate authority.
Connections have a 120-second lifetime bound and a 60-second idle/read timeout.
Request headers are limited to 64 KiB and bodies to 8 MiB. Chunked request
bodies and Expect are explicitly rejected. Responses stream without a size cap.

External mode forwards the byte stream unchanged. To use
[WaybackProxy](https://github.com/richardg867/WaybackProxy), run that project's
server, set `HOST` to `127.0.0.1`, `LISTEN_PORT` to `8888`, `QUICK_IMAGES` to
`false`, and choose `DATE` there. In Light Touch choose Device > HTTP Proxy >
External Proxy / WaybackProxy and enter that host and port. WaybackProxy owns
archive selection and rewriting; it is not bundled or automatically launched.
Its archive availability and supported methods remain server-dependent.

`../it-proxy/itproxy on|off` applies the guest settings through SCPreferences.
It backs up the six HTTP/HTTPS proxy keys per en0 service, preserves unrelated
settings, commits and applies through configd, and restores the saved keys on
off. It does not rebake the NAND or restart SpringBoard.

Host regression: `python3 tests/ipod/test_webproxy.py` from the repository root.
It uses loopback fixtures and ASan/UBSan, including simultaneous requests,
chunked response decoding, binary POST, framing rejection, CONNECT half-close,
and unmodified external-proxy streams. Native guest preference on/on/off/off
transactions were verified on 7E18. Native NSURLConnection returned HTTP 200
and the fixture body for `http://example.invalid/fixture` through an external
host proxy fixture; no guest origin DNS is needed. Repeat with
`tests/ipod/regress.py --checks boot,webproxy` after building both helpers.
Evidence from the initial native run is `/tmp/it-proxy-http-v2.log`.
