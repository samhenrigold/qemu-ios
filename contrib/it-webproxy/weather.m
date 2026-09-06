/* 7E18 Weather gateway translation. Foundation owns XML/JSON escaping/parsing. */
#import <Foundation/Foundation.h>
#import <curl/curl.h>
#import <math.h>

static BOOL number(id value, double low, double high)
{
    return [value isKindOfClass:NSNumber.class] &&
        CFGetTypeID((__bridge CFTypeRef)value) != CFBooleanGetTypeID() &&
        isfinite([value doubleValue]) && [value doubleValue] >= low && [value doubleValue] <= high;
}
static NSString *string(id value, NSUInteger maximum)
{
    return [value isKindOfClass:NSString.class] && [value length] <= maximum &&
        [value rangeOfCharacterFromSet:NSCharacterSet.controlCharacterSet].location == NSNotFound ? value : nil;
}
static NSXMLElement *child(NSXMLElement *parent, NSString *name, NSString *text)
{
    NSXMLElement *node = [NSXMLElement elementWithName:name stringValue:text];
    [parent addChild:node];
    return node;
}
static void attributes(NSXMLElement *node, NSDictionary *values)
{
    for (NSString *key in values) [node addAttribute:[NSXMLNode attributeWithName:key stringValue:values[key]]];
}
static NSDictionary *location(id value)
{
    if (![value isKindOfClass:NSDictionary.class] || !number(value[@"latitude"], -90, 90) ||
        !number(value[@"longitude"], -180, 180) || ![string(value[@"name"], 200) length]) return nil;
    return @{@"latitude":value[@"latitude"], @"longitude":value[@"longitude"], @"name":value[@"name"]};
}
static NSString *locationID(NSDictionary *place)
{
    NSData *data = [NSJSONSerialization dataWithJSONObject:place options:NSJSONWritingSortedKeys error:NULL];
    return [@"ltm:" stringByAppendingString:[data base64EncodedStringWithOptions:0]];
}
static NSDictionary *decodeLocation(NSString *identifier)
{
    if ([identifier isEqual:@"USCA0273|12797509"])
        return @{@"latitude":@37.323, @"longitude":@(-122.032), @"name":@"Cupertino"};
    if ([identifier isEqual:@"USNY0996|2459115"])
        return @{@"latitude":@40.7143, @"longitude":@(-74.006), @"name":@"New York"};
    if (identifier.length > 2048 || ![identifier hasPrefix:@"ltm:"]) return nil;
    NSData *data = [[NSData alloc] initWithBase64EncodedString:[identifier substringFromIndex:4] options:0];
    return data ? location([NSJSONSerialization JSONObjectWithData:data options:0 error:NULL]) : nil;
}
static size_t receiveJSON(char *bytes, size_t size, size_t count, void *context)
{
    NSMutableData *data = (__bridge NSMutableData *)context;
    if (size && count > SIZE_MAX / size) return 0;
    size_t length = size * count;
    if (length > 1024 * 1024 - data.length) return 0;
    [data appendBytes:bytes length:length];
    return length;
}
static id fetchJSON(NSString *host, NSString *path, NSDictionary *parameters)
{
    NSURLComponents *url = [NSURLComponents new];
    url.scheme = @"https"; url.host = host; url.path = path;
    NSMutableArray *query = [NSMutableArray new];
    for (NSString *key in parameters) [query addObject:[NSURLQueryItem queryItemWithName:key value:parameters[key]]];
    url.queryItems = query;
    CURL *curl = curl_easy_init();
    if (!curl) return nil;
    NSMutableData *body = [NSMutableData new];
    curl_easy_setopt(curl, CURLOPT_URL, url.string.UTF8String);
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "LightTouch/1.0 (Weather)");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receiveJSON);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (__bridge void *)body);
    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    return result == CURLE_OK && status == 200 ?
        [NSJSONSerialization JSONObjectWithData:body options:0 error:NULL] : nil;
}
static NSString *weatherIcon(id code, BOOL daylight)
{
    if (!number(code, 0, 99) || [code doubleValue] != [code intValue]) return nil;
    switch ([code intValue]) {
    case 0: return daylight ? @"32" : @"31";
    case 1: return daylight ? @"34" : @"33";
    case 2: return daylight ? @"30" : @"29";
    case 3: return @"26";
    case 45: case 48: return @"20";
    case 51: case 53: case 55: return @"9";
    case 56: case 57: return @"8";
    case 61: case 63: case 65: return @"12";
    case 66: case 67: return @"10";
    case 71: case 73: case 75: case 77: return @"16";
    case 80: case 81: case 82: return @"11";
    case 85: case 86: return @"14";
    case 95: return @"4";
    case 96: case 99: return @"3";
    default: return nil;
    }
}
static NSDate *date(NSString *text, NSString *format)
{
    if (!string(text, 32)) return nil;
    NSDateFormatter *formatter = [NSDateFormatter new];
    formatter.locale = [[NSLocale alloc] initWithLocaleIdentifier:@"en_US_POSIX"];
    formatter.calendar = [[NSCalendar alloc] initWithCalendarIdentifier:NSCalendarIdentifierGregorian];
    formatter.timeZone = [NSTimeZone timeZoneForSecondsFromGMT:0];
    formatter.dateFormat = format; formatter.lenient = NO;
    NSDate *value = [formatter dateFromString:text];
    return value && [[formatter stringFromDate:value] isEqual:text] ? value : nil;
}
static NSString *clockTime(id value)
{
    return date(value, @"yyyy-MM-dd'T'HH:mm") ? [value substringFromIndex:11] : nil;
}
static NSDictionary *moon(NSTimeInterval timestamp)
{
    // ponytail: mean lunar cycle for the stock icon; use an ephemeris if exact phase timing is needed.
    // NASA's 2000-01-06 18:15 UTC new moon; USNO mean synodic month 29.53059 days.
    double cycle = (timestamp - 947182500.0) / (29.53059 * 86400.0);
    cycle -= floor(cycle);
    return @{@"moonphase":[NSString stringWithFormat:@"%d", (int)floor(cycle * 8 + 0.5) % 8],
             @"moonfacevisible":[NSString stringWithFormat:@"%.3f", 50 * (1 - cos(2 * M_PI * cycle))]};
}
static NSXMLElement *forecast(id data, NSString *identifier, NSDictionary *place, BOOL celsius)
{
    if (![data isKindOfClass:NSDictionary.class] || !number(data[@"utc_offset_seconds"], -50400, 50400)) return nil;
    NSDictionary *current = data[@"current"], *daily = data[@"daily"];
    if (![current isKindOfClass:NSDictionary.class] || ![daily isKindOfClass:NSDictionary.class] ||
        !number(current[@"temperature_2m"], -200, 200) || !number(current[@"is_day"], 0, 1) ||
        [current[@"is_day"] doubleValue] != [current[@"is_day"] intValue]) return nil;
    NSString *time = clockTime(current[@"time"]), *icon = weatherIcon(current[@"weather_code"], [current[@"is_day"] boolValue]);
    if (!time || !icon) return nil;
    for (NSString *key in @[@"time", @"temperature_2m_max", @"temperature_2m_min", @"weather_code", @"sunrise", @"sunset"])
        if (![daily[key] isKindOfClass:NSArray.class] || [daily[key] count] != 6) return nil;
    NSString *sunrise = clockTime(daily[@"sunrise"][0]), *sunset = clockTime(daily[@"sunset"][0]);
    if (!sunrise || !sunset) return nil;
    NSXMLElement *item = [NSXMLElement elementWithName:@"item"];
    attributes(child(item, @"location", @""), @{@"id":identifier, @"city":place[@"name"]});
    attributes(child(item, @"units", @""), @{@"temperature":celsius ? @"C" : @"F"});
    NSXMLElement *astronomy = child(item, @"astronomy", @"");
    attributes(astronomy, @{@"sunrise":sunrise, @"sunset":sunset});
    attributes(astronomy, moon([date(current[@"time"], @"yyyy-MM-dd'T'HH:mm") timeIntervalSince1970] - [data[@"utc_offset_seconds"] doubleValue]));
    attributes(child(item, @"condition", @""), @{@"time":time, @"temp":[NSString stringWithFormat:@"%.0f", [current[@"temperature_2m"] doubleValue]], @"code":icon});
    NSCalendar *calendar = [[NSCalendar alloc] initWithCalendarIdentifier:NSCalendarIdentifierGregorian];
    calendar.timeZone = [NSTimeZone timeZoneForSecondsFromGMT:0];
    for (NSUInteger day = 0; day < 6; day++) {
        NSDate *when = date(daily[@"time"][day], @"yyyy-MM-dd");
        id high = daily[@"temperature_2m_max"][day], low = daily[@"temperature_2m_min"][day];
        NSString *condition = weatherIcon(daily[@"weather_code"][day], YES);
        if (!when || !condition || !number(high, -200, 200) || !number(low, -200, 200) || [high doubleValue] < [low doubleValue]) return nil;
        attributes(child(item, @"forecast", @""), @{@"high":[NSString stringWithFormat:@"%.0f", [high doubleValue]],
            @"low":[NSString stringWithFormat:@"%.0f", [low doubleValue]], @"code":condition,
            @"dayofweek":[NSString stringWithFormat:@"%ld", (long)[calendar component:NSCalendarUnitWeekday fromDate:when]]});
    }
    child(item, @"link", @"http://open-meteo.com/");
    return item;
}

/* Returns zero for other services; caller owns the returned malloc buffer. */
int it_weather_response(const char *target, const char *method, const void *bytes,
                        size_t length, void **output, size_t *outputLength)
{
    @autoreleasepool {
        *output = NULL; *outputLength = 0;
        NSURLComponents *url = [NSURLComponents componentsWithString:[NSString stringWithUTF8String:target]];
        if (![url.host.lowercaseString isEqual:@"iphone-wu.apple.com"] || ![url.path isEqual:@"/dgw"]) return 0;
        NSString *service = nil;
        for (NSURLQueryItem *part in url.queryItems) if ([part.name isEqual:@"apptype"]) {
            if (service) return 400;
            service = part.value;
        }
        if (![service isEqual:@"weather"]) return 0;
        if (strcmp(method, "POST") || url.user || url.password || length == 0 || length > 65536) return 400;
        NSString *xml = [[NSString alloc] initWithBytes:bytes length:length encoding:NSUTF8StringEncoding];
        if (!xml || [xml rangeOfString:@"<!DOCTYPE" options:NSCaseInsensitiveSearch].location != NSNotFound ||
            [xml rangeOfString:@"<!ENTITY" options:NSCaseInsensitiveSearch].location != NSNotFound ||
            memchr(bytes, 0, length)) return 400;
        NSXMLDocument *request = [[NSXMLDocument alloc] initWithXMLString:xml options:NSXMLNodeLoadExternalEntitiesNever error:NULL];
        NSXMLElement *root = request.rootElement;
        NSArray *queries = [root elementsForName:@"query"];
        if (![root.name isEqual:@"request"] || queries.count != 1 || request.DTD) return 400;
        NSXMLElement *query = queries[0];
        NSString *type = [query attributeForName:@"type"].stringValue;
        NSXMLElement *response = [NSXMLElement elementWithName:@"response"];
        NSXMLElement *list = child(child(response, @"result", @""), @"list", @"");
        if ([type isEqual:@"getlocationid"]) {
            NSArray *phrases = [query elementsForName:@"phrase"];
            if (phrases.count != 1) return 400;
            NSString *phrase = [phrases[0] stringValue];
            if (!string(phrase, 200)) return 400;
            if (phrase.length >= 2) {
                id result = fetchJSON(@"geocoding-api.open-meteo.com", @"/v1/search",
                    @{@"name":phrase, @"count":@"10", @"language":@"en", @"format":@"json"});
                if (![result isKindOfClass:NSDictionary.class]) return 502;
                id places = result[@"results"] ?: @[];
                if (![places isKindOfClass:NSArray.class] || [places count] > 10) return 502;
                for (id entry in places) {
                    NSDictionary *place = location(entry);
                    if (!place) return 502;
                    NSXMLElement *item = child(list, @"item", @"");
                    child(item, @"id", locationID(place));
                    child(item, @"city", place[@"name"]);
                    child(item, @"region", string(entry[@"admin1"], 200) ?: @"");
                    child(item, @"regionname", string(entry[@"admin1"], 200) ?: @"");
                    child(item, @"country", string(entry[@"country_code"], 8) ?: @"");
                    child(item, @"countryname", string(entry[@"country"], 200) ?: @"");
                }
            }
        } else if ([type isEqual:@"getforecastbylocationid"]) {
            NSArray *identifiers = [query nodesForXPath:@"./list/id" error:NULL];
            NSArray *units = [query elementsForName:@"unit"];
            if (!identifiers.count || identifiers.count > 20 || units.count != 1) return 400;
            NSString *unit = [units[0] stringValue];
            if (![unit isEqual:@"c"] && ![unit isEqual:@"f"]) return 400;
            NSMutableArray *places = [NSMutableArray new], *latitudes = [NSMutableArray new], *longitudes = [NSMutableArray new];
            for (NSXMLNode *identifier in identifiers) {
                NSDictionary *place = decodeLocation(identifier.stringValue);
                if (!place) return 422; // Unknown retired Yahoo ID: never guess its coordinates.
                [places addObject:place];
                [latitudes addObject:[place[@"latitude"] stringValue]];
                [longitudes addObject:[place[@"longitude"] stringValue]];
            }
            id result = fetchJSON(@"api.open-meteo.com", @"/v1/forecast", @{
                @"latitude":[latitudes componentsJoinedByString:@","], @"longitude":[longitudes componentsJoinedByString:@","],
                @"current":@"temperature_2m,weather_code,is_day", @"daily":@"weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset",
                @"temperature_unit":[unit isEqual:@"c"] ? @"celsius" : @"fahrenheit", @"timezone":@"auto", @"forecast_days":@"6"});
            NSArray *forecasts = [result isKindOfClass:NSDictionary.class] ? @[result] : result;
            if (![forecasts isKindOfClass:NSArray.class] || forecasts.count != places.count) return 502;
            for (NSUInteger index = 0; index < places.count; index++) {
                NSXMLElement *item = forecast(forecasts[index], [identifiers[index] stringValue], places[index], [unit isEqual:@"c"]);
                if (!item) return 502;
                [list addChild:item];
            }
        } else return 400;
        NSData *data = [[[NSXMLDocument alloc] initWithRootElement:response] XMLData];
        *output = malloc(data.length);
        if (!*output) return 503;
        memcpy(*output, data.bytes, data.length); *outputLength = data.length;
        return 200;
    }
}
