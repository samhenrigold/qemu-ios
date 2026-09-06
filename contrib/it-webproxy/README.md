# Guest HTTP proxy

`itwebproxy CONFIG` serves one slirp guestfwd connection on stdin/stdout; it
never opens a host listening port. Build with `bash build.sh` (system libcurl plus statically bundled OpenSSL; set OPENSSL_PREFIX for a custom build).
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
Responses are bounded to 32 MiB. Archived HTTP-to-HTTPS redirects are resolved
on the host, and absolute HTTPS links in HTML/CSS/JavaScript are presented as
HTTP links to the guest. Binary resources are unchanged. Explicit HTTPS addresses
are supported through the local TLS bridge.

Archive requests share a process lock and a one-second request interval. A 429
or 503 pauses all new archive fetches, honoring Retry-After (60 seconds by default),
and displays a readable retry notice. A 64-slot disk cache stores successful
responses below 2 MiB for one day, keyed by the complete URL and requested date;
cache hits remain available during cooldown. Cache/gate files sit beside CONFIG. Archive availability and rate limits still apply:
the first host acceptance request returned the original September 2009 example.com
page; a subsequent native guest request reached the archive but received HTTP 429.

`off` passes stale proxy connections directly to the origin while the guest
restores its previous network settings. Invalid/missing configuration fails closed. Config files can be atomically replaced while QEMU runs.
Example slirp option:

```
-netdev 'user,id=wifi0,guestfwd=tcp:10.0.2.100:3128-cmd:/path/to/itwebproxy /path/to/config'
```

The app quotes both paths for the command shell and escapes QEMU option commas.
Direct mode resolves origin hosts on the Mac, accepts bounded HTTP requests,
closes each HTTP connection after its response, and bridges CONNECT to modern
HTTPS. `itwebproxy --init-ca CONFIG` creates a unique per-device RSA/SHA-1 CA in
CONFIG.ca.pem (private key, mode 0600) and CONFIG.ca.der (public certificate).
The app trusts only the public certificate inside its guest when Proxy is enabled,
and removes that trust when disabled. Nothing is trusted on the Mac. Guest TLS
1.0/AES-CBC terminates locally; system libcurl verifies the modern upstream TLS
connection. Date-based HTTPS follows the same archive privacy rules as HTTP.
CLI connections without an initialized CA retain the original raw CONNECT mode.
Connections have a 120-second lifetime bound. Direct HTTP request headers are
limited to 64 KiB and bodies to 8 MiB. Chunked request bodies and Expect are
explicitly rejected. Direct responses stream without a size cap.

A legacy `upstream HOST PORT` mode remains for protocol fixtures and CLI users;
it is not required by, or exposed in, the app.

Direct mode returns HTTP 410 immediately for the exact retired API hosts
`api.openfeint.com` and `gdata.youtube.com`, including CONNECT. It does not
contact those hosts or claim their sign-in, achievements or video feeds work.
Case and a DNS trailing dot are normalized; parent domains and unrelated hosts
remain accessible. Archive, off and upstream modes bypass this policy.
The endpoint and retirement references are the
[OpenFeint SDK README](https://www.openfeint.com/developers/readme-openfeint-ios-sdk-2-12-5/),
[OpenFeint shutdown announcement](https://www.openfeint.com/developers/openfeint-service-shutdown-and-gree-migration/),
and [Google's YouTube API retirement notice](https://developers-jp.googleblog.com/2015/05/youtube-data-api-v2.html).
Other advertising, analytics and game-service hosts are not presumed dead.

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

## TLS verification

`ARMV6_SDK=/path/to/iPhoneOS3.1.3.sdk ../it-proxy/build.sh` also builds
`ittrust add|remove CERT.der`. It calls the guest's native securityd trust-store
API in user domain 2 and carries iOS 3's `modify-anchor-certificates` entitlement
(signed with development tool `ldid`). It never edits the trust database directly.

`python3 tests/ipod/test_webproxy_tls_guest.py` from the repository root creates a
fresh disposable NAND overlay and a loopback OpenSSL TLS 1.0/AES128-SHA server.
The native guest client must reject the untrusted chain, load HTTP 200 after
adding its temporary CA, then reject it again after CA removal. This passed on
7E18. No CA is installed on the Mac or in the default NAND.

`python3 tests/ipod/test_webproxy_tls.py` exercises real TLS 1.0 socket streams,
concurrent/idempotent CA initialization, guest certificate/hostname checks,
encrypted HTTP error framing, and rejection of untrusted upstream certificates.
A native guest CONNECT run returned HTTP 200 from both https://example.com/ and
https://www.nytimes.com/; evidence: /tmp/it-proxy-tls-bridge-native.log.
Modern site JavaScript/CSS can still exceed iOS 3 WebKit's capabilities.

## Weather

Direct mode translates the stock 7E18 Weather gateway to Open-Meteo forecast
and geocoding APIs. City searches return opaque location IDs; the two default
Yahoo IDs (Cupertino/New York) also work. Other old IDs must be removed and
searched again rather than assigned guessed coordinates. Archive/upstream/off
modes retain their existing behavior. No separate helper installation is needed.

Data attribution: [Open-Meteo](https://open-meteo.com/),
[CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).
The bundled free API endpoint is for noncommercial use under the provider's
[terms](https://open-meteo.com/en/terms); commercial distribution needs an
appropriate provider arrangement. The stock Weather link opens Open-Meteo.
Search results currently use English names. Missing/invalid provider values,
unknown conditions or unavailable polar sunrise/sunset fail the update and
preserve the guest's previous forecast. No fabricated weather is substituted.

The moon icon uses a mean-cycle approximation, not a precise lunar ephemeris:
[NASA new-moon reference](https://ntrs.nasa.gov/api/citations/19950008253/downloads/19950008253.pdf)
and [USNO mean synodic month](https://aa.usno.navy.mil/downloads/c15_usb_online.pdf).

`python3 tests/ipod/test_weather_adapter.py` checks actual Foundation parsing,
coordinate identity, XML escaping, six-day forecast conversion, malformed
requests, unsafe entities, and the compiled proxy's HTTP framing without a
network. Live provider availability remains separate from these local checks.
