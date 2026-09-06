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
This search response and arbitrary replacement IDs still need native acceptance.

Run `python3 tests/ipod/test_weather_protocol_guest.py` with the built emulator,
proxy and sibling firmware/dependencies. It uses a fresh overlay and local HTTP
fixture, saves a screenshot for forecast review, checks persisted city names,
and requires guest-confirmed shutdown. It does not contact a weather provider.

## Stocks

Captured query types (response schemas remain unverified):

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

## Live provider work still required

Weather needs city search/identity verification, bounded upstream fetching,
validated forecast/icon conversion, attribution, and failure handling before
integration into direct proxy mode. Open-Meteo is a candidate, not an integrated
dependency: its [free endpoint terms](https://open-meteo.com/en/terms) restrict
use to noncommercial applications and require attribution. Distribution needs
an appropriate provider arrangement. Stocks still needs response-schema research
and an appropriate financial-data provider. Archive mode should retain archived
responses rather than quietly mix in present-day data.
