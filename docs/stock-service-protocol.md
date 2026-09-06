# Stock service gateway (7E18)

Weather and Stocks POST XML to `http://iphone-wu.apple.com/dgw` with
`apptype=weather` or `finance`. The query includes a per-install UUID; captures
must omit that identifier and headers. These findings come from disposable
native guests and the stock ARMv6 applications, not a live replacement service.

## Weather

Forecast request:

```xml
<request api="weather" apiver="1.0.0">
  <query id="30" timestamp="0" type="getforecastbylocationid">
    <list><id>USCA0273|12797509</id><id>USNY0996|2459115</id></list>
    <language>en_US</language><unit>f</unit>
  </query>
</request>
```

The default IDs are Cupertino and New York respectively. A successful fixture:

```xml
<response><result><list><item>
  <location id="USCA0273|12797509" city="Fixture City"/>
  <units temperature="F"/>
  <astronomy sunrise="06:00" sunset="20:00" moonfacevisible="0.5" moonphase="0"/>
  <condition time="14:30" temp="73" code="32"/>
  <forecast high="80" low="60" code="32" dayofweek="1"/>
  <forecast high="81" low="61" code="32" dayofweek="2"/>
  <forecast high="82" low="62" code="32" dayofweek="3"/>
  <forecast high="83" low="63" code="32" dayofweek="4"/>
  <forecast high="84" low="64" code="32" dayofweek="5"/>
  <forecast high="85" low="65" code="32" dayofweek="6"/>
</item></list></result></response>
```

The native parser requires **exactly six forecasts**. Sunday is day 1; code 32
renders sunny. The location ID matches an existing city's `Zip` preference.
Temperature unit `C` selects Celsius. The example's values are synthetic.
`WeatherUpdater processDocument:` at `0xf10c` parses the response; the six-entry
check is at `0xf67c`. XML attributes are read through `0xeb80`.

City search uses query type `getlocationid`, child `phrase` and `language`.
The response parser at `0x122ec` reads `response/result/list/item`, with **child
text elements** `id`, `city`, `region`, `regionname`, `country`, `countryname`.
A native Add City test searched for `Q`, displayed `Fixture City, CA`, selected
it and persisted the exact opaque ID `ltm:37.323,-122.032:Fixture`. This confirms
that new search results need not use Yahoo IDs.

Run `python3 tests/ipod/test_stock_services_guest.py` with the built emulator,
proxy and sibling firmware/dependencies. It uses a fresh overlay and local HTTP
fixture, saves a screenshot for forecast review, checks persisted city names,
and requires guest-confirmed shutdown. Add `--stocks` to verify ten persisted
synthetic quotes; screenshots retain chart rendering for review. It does not contact a weather provider.

## Stocks

Captured query types:

| Type | Children |
| --- | --- |
| `getnews` | `list/symbol` |
| `getsymbol` | `phrase`, `count`, `offset` |
| `getquotes` | `list/symbol`, `parts` |
| `getchart` | `symbol`, `range` (observed `6m`) |

Observed quote parts are `symbol,price,change,marketcap,status` and
`symbol,open,high,low,volume,averagedailyvolume,peratio,yearrange,dividendyield`.
Some symbol list entries percent-encode the caret (e.g. `%5EDJI`), while other
query types send `^DJI`. Do not substitute fabricated quotes for missing data.

The quote response has now rendered synthetic prices and changes in a native
guest (`/tmp/it-stocks-response-probe.log`):

```xml
<response><result><list count="1"><quote>
  <symbol>AAPL</symbol><name>SYNTHETIC FIXTURE</name><sname>FIXTURE</sname>
  <price>123.45</price><change>1.25</change><status>1</status>
  <marketcap>1.2B</marketcap>
</quote></list></result></response>
```

`StockUpdater parseData:` at `0x167e8` uses SAX parsing. Start/end callbacks
at `0x1782c`/`0x17150` consume `response/result/list/quote`, and quote fields
are child text. `list.count` is an attribute. Additional recognized fields are
`name`, `sname`, `exchange`, `open`, `high`, `low`, `volume`, `marketcap`,
`peratio`, `dividendyield`, `yearrange`, and `averagedailyvolume`.
The consolidated native check also rendered a synthetic 60-point chart:

```xml
<response><result><list count="2">
  <point timestamp="1788566400" close="100" volume="1000"/>
  <point timestamp="1788652800" close="111" volume="1100"/>
</list></result></response>
```

`list.count` must match point count. Point values are **attributes**: timestamp
is Unix seconds, close is numeric price, volume is an unsigned quantity. The
attribute parser is at `0x190d0`; the chart SAX start callback is at `0x191a0`.
The fixture shows a repeating 100–111 sawtooth. News and symbol validation
responses remain unverified, as do other chart intervals and metadata.

## Live integration status

Direct proxy mode now translates Weather requests to Open-Meteo using native
Foundation XML/JSON handling and verified host TLS. It supports the two default
Yahoo IDs and newly searched cities. Other historical IDs return an error that
asks the user to remove/re-add the city. Search names are currently English.
Unknown/missing forecast values fail the update; previous guest data is retained.
Polar sunrise/sunset values without a valid time remain unsupported.

`python3 tests/ipod/test_stock_services_guest.py --live-weather` uses a fresh
guest, captures the initial Fahrenheit screen, searches Cupertino, adds the new
city, switches to Celsius and checks the display preference and persisted six-day
forecasts for all cities. Stock Weather retains Fahrenheit data and converts
it locally for Celsius display.
Unlike the default fixture run, this explicitly contacts the provider and can
fail when it is unavailable. Screenshots retain visual evidence separately.

The bundled [Open-Meteo free endpoint](https://open-meteo.com/en/terms) is for
noncommercial use with attribution. The Weather link and Light Touch Help credit
the provider. Commercial distribution needs an appropriate provider arrangement.
The moon icon uses a documented mean-cycle approximation. Forecast data and
geocoding are live; archive mode retains archived responses.

Stocks quote/chart protocols are verified with synthetic fixtures only. News,
symbol validation and an appropriate live financial-data provider remain pending.
