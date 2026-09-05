# Guest HTTP proxy

`itwebproxy CONFIG` serves one slirp guestfwd connection on stdin/stdout; it
never opens a host listening port. Build with `bash build.sh` (system libcurl).
The configuration file is read at connection start:

```
direct
```

or, to browse the nearest available Internet Archive capture to a date:

```
archive
20090909
```

Light Touch bundles this native helper. Device > Proxy offers No Proxy or HTTP
Proxy, with an optional archive date; there is no separate server or installation.
Dated browsing fetches `https://web.archive.org/web/DATEid_/ORIGINAL_URL` using
verified host TLS, follows canonical snapshot redirects on the host, and returns
the original page to the guest. Archived GET/HEAD requests do not forward guest
cookies or Authorization; archive Set-Cookie headers are not sent to the guest.
Responses are bounded to 32 MiB. Archive availability and rate limits still apply:
the first host acceptance request returned the original September 2009 example.com
page; a subsequent native guest request reached the archive but received HTTP 429.

`off` returns 503. Config files can be atomically replaced while QEMU runs.
Example slirp option:

```
-netdev 'user,id=wifi0,guestfwd=tcp:10.0.2.100:3128-cmd:/path/to/itwebproxy /path/to/config'
```

The app quotes both paths for the command shell and escapes QEMU option commas.
Direct mode resolves origin hosts on the Mac, accepts bounded HTTP requests,
closes each HTTP connection after its response, and supports end-to-end CONNECT.
CONNECT does not upgrade old guest TLS or install a certificate authority.
Connections have a 120-second lifetime bound. Direct HTTP request headers are
limited to 64 KiB and bodies to 8 MiB. Chunked request bodies and Expect are
explicitly rejected. Direct responses stream without a size cap.

A legacy `upstream HOST PORT` mode remains for protocol fixtures and CLI users;
it is not required by, or exposed in, the app.

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
