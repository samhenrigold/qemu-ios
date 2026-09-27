# Location on the iPad 1 (iOS 3.2.2): research, before building

The Wi-Fi iPad 1 has no GPS. Its only position source is a network location
service that turns nearby Wi-Fi BSSIDs (and cell towers, which we have none of)
into a latitude/longitude. Maps' "locate me" and its compass mode both wait on
that service, so today Maps ends with "Your location could not be determined"
(the compass hardware itself works — see the AK8973 model and the it-heading
proof). This documents what 3.2 queries and how a host-side responder can
answer, so Maps gets a fix and we get a host-settable location like the
Simulator's. Recommendation and open question at the end; nothing is built yet.

## Who asks, and how

`CLLocationManager -startUpdatingLocation` in an app talks to
`/usr/libexec/locationd`. The provider that matters for us lives entirely in
locationd (strings from the 7B500 binary, CoreLocation-197.32):

- **Apple Location Server (network / Wi-Fi), the modern path.** Class
  `CLNetworkLocationProvider` → `CLNetworkLocationRequesterALS`. It POSTs a
  protobuf to the URL in the `AppleLocationServer` preference, default
  **`https://iphone-services.apple.com/clls/wloc`**. Request/response types
  `ALSLocationRequest` / `ALSLocationResponse`, with `ALSWirelessAP`
  (field `macID`, the BSSID string) and `ALSLocation` (lat/lon/accuracy).
  Prefs seen alongside it: `AppleLocationServerRequiresCert` (bool),
  `AppleLocationServerRequestLog`, `AppleLocationServerResponseLog`. Headers
  include `User-Agent`, `Content-Type`, and `X-Apple-Lat` / `X-Apple-Lon`
  (the last-known position echoed on collection uploads).
- **Skyhook (WPS), the legacy path.** `CLSkyhookLocationProvider`, a bundled
  `WPS API 3.0.0` (`com.skyhookwireless.wps`), POSTs XML to the `WifiServer`
  preference, default **`https://iphone-maps.apple.com/shwps/v1/wps2`**
  (`http://skyhookwireless.com/wps/2005` XML namespace). It also has a
  hardcoded `https://api.skyhookwireless.com/wps2`.
- **Settings**, fetched over plain HTTP:
  **`http://configuration.apple.com/configurations/pep/cl/settings.plist`**
  (`CLDaemonSettings`). This is a live config file that can carry the provider
  URLs and toggles; because it's plain HTTP, a host responder can serve it and
  thereby point everything else wherever we want.
- Harvesting/collection endpoints (`.../clls/wloc`, `.../hcy/wcs`,
  `.../hcy/ccs`, `.../hcy/pbcwloc`, `.../pds/pd`) upload observations. We can
  ignore or 200-OK these; they don't affect getting a fix.

Which provider runs is chosen from the downloaded settings and per-region
prefs. We don't need to know which: whichever it is, we control its endpoint
through the plain-HTTP settings file, and both endpoints are hosts we can
intercept (below).

## How the guest's traffic reaches the host

The Wi-Fi netdev is slirp (`type=user,id=wifi0`, hw/arm/ipad1.c). Two existing
mechanisms already carry guest HTTP(S) to a host process:

- **The web proxy** (`contrib/it-webproxy`, slirp guestfwd 10.0.2.100:3128; the
  Wi-Fi service ships a PAC, `usr/local/share/ltm/proxy.pac`, that sends traffic
  there and falls back to DIRECT). It already terminates guest TLS with a local
  bridge: `CONNECT host:443` is met with a per-host certificate signed by a CA
  the guest trusts, when the app's Proxy feature is enabled
  (`CONFIG.ca.pem`/`.ca.der`, installed into the guest trust store by `ittrust`,
  which holds iOS 3's `modify-anchor-certificates` entitlement — README §CA).
  It already has a host-side canned-response pattern: `it_weather_response`
  matches a target URL and returns a synthesised body instead of proxying.
- **slirp DNS/guestfwd** could instead redirect one hostname to a host
  listener, but that is not needed given the proxy already MITMs by host.

So the delivery path for a location responder already exists: the same TLS
bridge and per-URL responder the weather service uses.

## Request/response format to implement

The ALS path is protobuf, which is stable and easy to parse/emit by hand:

- `ALSLocationRequest`: repeated `ALSWirelessAP { string macID; ... }`,
  plus counts (`numberOfSurroundingWifis`), optional `ALSCellTower`s (none for
  us), and a locale/version envelope.
- `ALSLocationResponse`: repeated `ALSWirelessAP` each carrying an
  `ALSLocation { double latitude; double longitude; ... accuracy ... }`.

We know the guest's only BSSID: the fake AP the SDIO Wi-Fi model advertises,
**02:00:5e:10:00:01** (`ssid "qemu-ios"`, docs/ipad1/wifi.md). So the responder
doesn't even need to parse the request precisely to start: for any request that
lists that BSSID, return one `ALSWirelessAP` with that MAC and an `ALSLocation`
at the host-set lat/long with a small accuracy (say 50 m). Parsing the request's
macID list and echoing each is the tidy version.

The Skyhook path is XML under the `skyhookwireless.com/wps/2005` namespace; if
the build turns out to prefer it, its response carries a `<location>` with
`latitude`/`longitude`. Either is small.

## The host-set value

A `location` app/machine setting (lat,long, like the Simulator's custom
location, and settable at runtime) feeds the responder. Natural homes: a QOM
property the proxy reads, or a field in the proxy's CONFIG file (which
"can be atomically replaced while QEMU runs", README), so `qom-set` or a
Device menu control changes it without a reboot.

## Recommendation (host-side, no new guest injection)

1. Add a location responder to `contrib/it-webproxy`, parallel to
   `it_weather_response`: on a POST whose target is the ALS `wloc` host/path
   (and/or the Skyhook `shwps` host), synthesise the response for the host-set
   lat/long and the guest's known BSSID. HTTPS is handled by the proxy's
   existing TLS bridge, so this needs the app's Proxy feature on (CA already
   trusted) — no new certificate work, no guest binary injection.
2. Serve `configuration.apple.com/.../settings.plist` from the proxy too, so we
   can pin the provider to the endpoint we answer and set
   `AppleLocationServerRequiresCert=false` if the cert path is ever awkward.
3. Host-set location via the proxy CONFIG file (atomic replace) or a QOM
   property; wire a Device-menu/`qom-set` control.
4. Prove it: it-heading already shows the compass; extend the same run to tap
   Maps' locate button and confirm the blue dot lands at the set location and
   compass mode then engages.

This is fully host-side **provided the app's Proxy feature (its trusted CA) is
enabled** — that's the one dependency. It is the existing, blessed mechanism
(same as weather and dated browsing), not new guest injection.

## Open question for the lead

The clean path assumes the **web-proxy Proxy feature is on** (its CA in the
guest trust store) so the TLS bridge can answer for `iphone-services.apple.com`.
If we want location to work with the proxy **off**, the alternatives are worse:
either serve only over the plain-HTTP `settings.plist` and hope the provider can
be pointed at a plain-HTTP `wloc` with `RequiresCert=false` (unverified that 3.2
honours an http:// AppleLocationServer), or install the CA unconditionally
(guest injection). I'd build option 1 (proxy-on) first and measure whether
`RequiresCert=false` + http:// endpoint also works, which would remove the
proxy-on dependency. Flagging before building, per your instruction.


## What was built (and what the capture showed)

A slirp capture settled the open question: **locationd ignores the Wi-Fi
service's PAC** and connects straight to `iphone-services.apple.com:443` (and
fetches `configuration.apple.com/.../WMM.dat` over plain HTTP, also direct).
With stock slirp there is no host-side way to intercept a direct connection by
name (guestfwd only covers addresses inside 10.0.2.0/24, and slirp's DNS can't be
overridden), so the TLS-bridge plan and a served settings.plist were both out.

What works is the plain-HTTP route through a preference: locationd reads
`AppleLocationServer` from its own domain and honours an `http://` URL with
`AppleLocationServerRequiresCert` false. The store builder seeds
`/var/mobile/Library/Preferences/com.apple.locationd.plist` (imgtools/ipad1_rootfs.py,
alongside the Wi-Fi PAC) with `http://10.0.2.100:3128/clls/wloc`, the address the
app's itwebproxy guestfwd already occupies. That is a baked guest preference,
like the PAC, not guest code. itwebproxy answers `/clls/wloc` itself
(`it_location_response`) **in every proxy mode, including off**, so location
doesn't depend on the Proxy feature or any CA.

Wire format, read off a captured request (7B500):

    request  u16 1 | u16 len "en_US" | u16 0 "" | u16 len "3.2.2.7B500"
             | u32 type 1 | u32 len | ALSLocationRequest
               { 1: cell tower {..}, 2: ALSWirelessAP { 1: macID "2:0:5e:10:0:1" },
                 3: 0, 4: 0 }
    response u16 1 | u32 type 1 | u32 len | ALSLocationResponse
               { 2: ALSWirelessAP { 1: macID, 2: ALSLocation
                   { 1: lat*1e8, 2: lon*1e8, 3: accuracy m } } }

(big-endian framing; the response preamble is PBRequester readResponsePreamble /
tryReadResponseData in ProtocolBuffer.framework.)

The position comes from `CONFIG.location` beside the proxy CONFIG
(`web-proxy.conf.location` in the app's state directory): `LAT LON [ACCURACY]`,
replaceable at any time; Apple Park when absent. tests/ipad1/test_location.py
checks the answer offline.

Measured (gl-drive, pristine-based scratch store with the preference and the PAC):

- proxy mode `direct`, 51.50073,-0.12463: Maps' locate dot on Big Ben; the second
  tap engages compass mode and the map turns with `qom-set compass-heading`.
- proxy mode `off`, 35.65858,139.74543: dot on Tokyo Tower, compass mode, map turns
  from heading 0 to 90 (screens/location-tokyo-proxy-off.png).

Known limits:

- **locationd caches a position per BSSID** (its wifis database) and only asks
  about unknown access points, so changing CONFIG.location while a guest runs does
  not move an already-placed dot; a fresh boot on a fresh overlay does. The fix is
  host-side too: give the fake AP a BSSID derived from the configured position (the
  SDIO Wi-Fi model), so a new position is a new, unknown AP. Not done yet.
- Compass mode shows the "Compass Interference" figure-8 prompt: the heading is
  right but locationd never considers the magnetometer calibrated (headingAccuracy
  -1), because a field that never changes with the pose looks uncalibrated. Not done.
- The app has to create `web-proxy.conf.location` (a Device > Location control);
  until then the default position is used.
