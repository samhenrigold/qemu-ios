#!/usr/bin/env python3
"""Compile the actual native Weather adapter; no network or guest required."""
import subprocess
import tempfile
from pathlib import Path
root = Path(__file__).resolve().parents[2]
source = r'''
#import "weather.m"
#include <assert.h>
int main(void) { @autoreleasepool {
    NSDictionary *place = @{@"latitude":@37.3, @"longitude":@(-122.0), @"name":@"A&B <City> ☀"};
    assert([decodeLocation(locationID(place)) isEqual:place]);
    assert(decodeLocation(@"USCA0273|12797509"));
    assert(!decodeLocation(@"USXX0000"));
    assert(!decodeLocation(@"ltm:bad"));
    assert(!string(@"bad\nname",200));
    assert(!location(@{@"latitude":@YES,@"longitude":@0,@"name":@"bad"}));
    assert(!location(@{@"latitude":@91,@"longitude":@0,@"name":@"bad"}));
    NSMutableDictionary *daily = [@{@"time":@[@"2026-09-06",@"2026-09-07",@"2026-09-08",@"2026-09-09",@"2026-09-10",@"2026-09-11"],
        @"weather_code":@[@0,@1,@2,@3,@61,@95], @"temperature_2m_max":@[@80,@81,@82,@83,@84,@85],
        @"temperature_2m_min":@[@60,@61,@62,@63,@64,@65],
        @"sunrise":@[@"2026-09-06T06:00",@"",@"",@"",@"",@""],
        @"sunset":@[@"2026-09-06T20:00",@"",@"",@"",@"",@""]} mutableCopy];
    NSMutableDictionary *current = [@{@"time":@"2026-09-06T14:30",@"temperature_2m":@73.2,@"weather_code":@0,@"is_day":@1} mutableCopy];
    NSDictionary *data = @{@"current":current,@"daily":daily,@"utc_offset_seconds":@0};
    assert([moon(947182500.0)[@"moonphase"] isEqual:@"0"]);
    assert([moon(947182500.0+29.53059*86400/2)[@"moonphase"] isEqual:@"4"]);
    NSXMLElement *item = forecast(data, locationID(place), place, NO);
    assert(item && [item elementsForName:@"forecast"].count == 6);
    assert([[[[forecast(data,@"id",place,YES) elementsForName:@"units"] firstObject] attributeForName:@"temperature"].stringValue isEqual:@"C"]);
    assert([[[[item elementsForName:@"forecast"] firstObject] attributeForName:@"dayofweek"].stringValue isEqual:@"1"]);
    NSXMLDocument *roundtrip = [[NSXMLDocument alloc] initWithXMLString:item.XMLString options:NSXMLNodeLoadExternalEntitiesNever error:NULL];
    assert([[[[roundtrip.rootElement elementsForName:@"location"] firstObject] attributeForName:@"city"].stringValue isEqual:place[@"name"]]);
    current[@"temperature_2m"] = NSNull.null; assert(!forecast(data,@"id",place,NO));
    current[@"temperature_2m"] = @73;
    daily[@"weather_code"] = @[@0]; assert(!forecast(data,@"id",place,NO));
    assert(!clockTime(@"2026-02-30T12:00"));
    assert(!weatherIcon(@100,YES)); assert([weatherIcon(@0,NO) isEqual:@"31"]);
    void *output = NULL; size_t length = 0;
    const char *url = "http://iphone-wu.apple.com/dgw?apptype=weather";
    assert(it_weather_response("http://example.com/dgw?apptype=weather","POST","",0,&output,&length)==0);
    assert(it_weather_response(url,"GET","",0,&output,&length)==400);
    const char *bad[] = {"<!DOCTYPE request [<!ENTITY x SYSTEM 'file:///etc/passwd'>]><request>&x;</request>",
        "<request><query type='other'/></request>","<request><query/><query/></request>",
        "<request><query type='getforecastbylocationid'><list><id>UNKNOWN</id></list><unit>f</unit></query></request>"};
    for (unsigned i=0;i<4;i++) {
        int status=it_weather_response(url,"POST",bad[i],strlen(bad[i]),&output,&length);
        assert(status == (i==3 ? 422 : 400)); assert(!output && !length);
    }
    const char *empty="<request><query type='getlocationid'><phrase>Q</phrase></query></request>";
    assert(it_weather_response(url,"POST",empty,strlen(empty),&output,&length)==200);
    assert(output && length); free(output);
    puts("PASS: city identity, XML escaping, six-day forecast, invalid upstream data and request boundaries");
} }
'''
with tempfile.TemporaryDirectory() as temp:
    path = Path(temp)
    (path/'check.m').write_text(source)
    subprocess.run(['clang','-fobjc-arc','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations',
                    '-fsanitize=address,undefined','-I'+str(root/'contrib/it-webproxy'),str(path/'check.m'),
                    '-framework','Foundation','-lcurl','-o',str(path/'check')],check=True)
    subprocess.run([str(path/'check')],check=True)

    # Exercise the actual proxy dispatch and HTTP framing, without an upstream.
    subprocess.run(['clang','-fobjc-arc','-Wno-deprecated-declarations','-c',str(root/'contrib/it-webproxy/weather.m'),
                    '-o',str(path/'weather.o')],check=True)
    subprocess.run(['clang','-DHAVE_WEATHER','-Wno-deprecated-declarations',str(root/'contrib/it-webproxy/itwebproxy.c'),
                    str(path/'weather.o'),'-framework','Foundation','-lcurl','-o',str(path/'proxy')],check=True)
    (path/'config').write_text('direct\n\n\n')
    body=b"<request><query type='getlocationid'><phrase>Q</phrase></query></request>"
    request=b'POST http://iphone-wu.apple.com/dgw?apptype=weather HTTP/1.0\r\nContent-Length: '+str(len(body)).encode()+b'\r\n\r\n'+body
    result=subprocess.run([str(path/'proxy'),str(path/'config')],input=request,stdout=subprocess.PIPE,check=True)
    headers,content=result.stdout.split(b'\r\n\r\n',1)
    assert headers.startswith(b'HTTP/1.0 200 OK'),headers
    assert b'Content-Length: '+str(len(content)).encode() in headers,headers
    import xml.etree.ElementTree as ET
    assert ET.fromstring(content).find('./result/list') is not None,content
    print('PASS: built-in Weather dispatch and HTTP response framing')
