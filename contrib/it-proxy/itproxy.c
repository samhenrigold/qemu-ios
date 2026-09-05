/* Configure the guest through configd's supported preferences transaction.
 * Keep a per-service backup of only the keys we own, restoring them on off.
 */
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int strcmp(const char *, const char *);
extern long write(int, const void *, unsigned long);
extern void _exit(int);
typedef const void *Ref;
typedef unsigned char Bool;
#define LOAD(lib, name, result, args) result (*name) args = dlsym(lib, #name); if (!name) _exit(2)
#define UTF8 0x08000100
__attribute__((naked)) void _start(void) {
    __asm__ volatile("ldr r0, [sp]\n\tadd r1, sp, #4\n\tb _main");
}
int main(int argc, char **argv) {
    if (argc != 2 || (strcmp(argv[1], "on") && strcmp(argv[1], "off"))) _exit(2);
    int enable = !strcmp(argv[1], "on");
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", 2);
    void *sc = dlopen("/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration", 2);
    LOAD(cf, CFStringCreateWithCString, Ref, (Ref, const char *, unsigned));
    LOAD(cf, CFPropertyListCreateDeepCopy, Ref, (Ref, Ref, unsigned long));
    LOAD(cf, CFDictionaryCreateMutable, Ref, (Ref, long, Ref, Ref));
    LOAD(cf, CFDictionaryGetValue, Ref, (Ref, Ref));
    LOAD(cf, CFDictionarySetValue, void, (Ref, Ref, Ref));
    LOAD(cf, CFDictionaryRemoveValue, void, (Ref, Ref));
    LOAD(cf, CFDictionaryGetCount, long, (Ref));
    LOAD(cf, CFDictionaryGetKeysAndValues, void, (Ref, Ref *, Ref *));
    LOAD(cf, CFGetTypeID, unsigned long, (Ref));
    LOAD(cf, CFDictionaryGetTypeID, unsigned long, (void));
    LOAD(cf, CFNumberCreate, Ref, (Ref, int, const void *));
    LOAD(cf, CFEqual, Bool, (Ref, Ref));
    LOAD(cf, CFRelease, void, (Ref));
    LOAD(sc, SCPreferencesCreate, Ref, (Ref, Ref, Ref));
    LOAD(sc, SCPreferencesLock, Bool, (Ref, Bool));
    LOAD(sc, SCPreferencesUnlock, Bool, (Ref));
    LOAD(sc, SCPreferencesGetValue, Ref, (Ref, Ref));
    LOAD(sc, SCPreferencesSetValue, Bool, (Ref, Ref, Ref));
    LOAD(sc, SCPreferencesCommitChanges, Bool, (Ref));
    LOAD(sc, SCPreferencesApplyChanges, Bool, (Ref));
    Ref keyCallbacks = dlsym(cf, "kCFTypeDictionaryKeyCallBacks");
    Ref valueCallbacks = dlsym(cf, "kCFTypeDictionaryValueCallBacks");
    if (!keyCallbacks || !valueCallbacks) _exit(2);
#define STR(s) CFStringCreateWithCString(0, s, UTF8)
#define DICT() CFDictionaryCreateMutable(0, 0, keyCallbacks, valueCallbacks)
#define ISDICT(v) ((v) && CFGetTypeID(v) == CFDictionaryGetTypeID())
    Ref name = STR("LightTouchWebProxy"), networkKey = STR("NetworkServices");
    Ref backupKey = STR("LightTouchWebProxyBackup"), proxyKey = STR("Proxies");
    Ref interfaceKey = STR("Interface"), deviceKey = STR("DeviceName"), en0 = STR("en0");
    Ref keys[] = {STR("HTTPEnable"), STR("HTTPProxy"), STR("HTTPPort"),
                  STR("HTTPSEnable"), STR("HTTPSProxy"), STR("HTTPSPort")};
    int one = 1, port = 3128;
    Ref enabled = CFNumberCreate(0, 9, &one), portValue = CFNumberCreate(0, 9, &port);
    Ref host = STR("10.0.2.100");
    Ref values[] = {enabled, host, portValue, enabled, host, portValue};
    Ref prefs = SCPreferencesCreate(0, name, 0);
    if (!prefs || !SCPreferencesLock(prefs, 1)) _exit(3);
    Ref oldNetwork = SCPreferencesGetValue(prefs, networkKey);
    if (!ISDICT(oldNetwork)) _exit(4);
    Ref network = CFPropertyListCreateDeepCopy(0, oldNetwork, 1);
    Ref oldBackup = SCPreferencesGetValue(prefs, backupKey);
    if (oldBackup && !ISDICT(oldBackup)) _exit(4);
    Ref backup = oldBackup ? CFPropertyListCreateDeepCopy(0, oldBackup, 1) : DICT();
    if (!network || !backup) _exit(4);
    long count = CFDictionaryGetCount(network);
    if (count < 0 || count > 64) _exit(4);
    Ref identifiers[64], services[64];
    CFDictionaryGetKeysAndValues(network, identifiers, services);
    int matched = 0;
    for (long i = 0; i < count; i++) {
        Ref service = services[i];
        if (!ISDICT(service)) _exit(4);
        Ref interface = CFDictionaryGetValue(service, interfaceKey);
        if (!ISDICT(interface)) continue;
        Ref device = CFDictionaryGetValue(interface, deviceKey);
        if (!device || !CFEqual(device, en0)) continue;
        matched++;
        Ref proxies = CFDictionaryGetValue(service, proxyKey);
        if (proxies && !ISDICT(proxies)) _exit(4);
        if (!proxies) { proxies = DICT(); CFDictionarySetValue(service, proxyKey, proxies); CFRelease(proxies); }
        Ref saved = CFDictionaryGetValue(backup, identifiers[i]);
        if (saved && !ISDICT(saved)) _exit(4);
        if (enable && !saved) {
            saved = DICT();
            for (int k = 0; k < 6; k++) {
                Ref value = CFDictionaryGetValue(proxies, keys[k]);
                if (value) CFDictionarySetValue(saved, keys[k], value);
            }
            CFDictionarySetValue(backup, identifiers[i], saved); CFRelease(saved);
        }
        if (enable) {
            for (int k = 0; k < 6; k++) CFDictionarySetValue(proxies, keys[k], values[k]);
        } else if (saved) {
            for (int k = 0; k < 6; k++) {
                Ref value = CFDictionaryGetValue(saved, keys[k]);
                if (value) CFDictionarySetValue(proxies, keys[k], value);
                else CFDictionaryRemoveValue(proxies, keys[k]);
            }
            CFDictionaryRemoveValue(backup, identifiers[i]);
        }
    }
    if (!matched) _exit(5);
    if (!SCPreferencesSetValue(prefs, networkKey, network) ||
        !SCPreferencesSetValue(prefs, backupKey, backup) ||
        !SCPreferencesCommitChanges(prefs) || !SCPreferencesApplyChanges(prefs)) _exit(6);
    SCPreferencesUnlock(prefs);
    write(1, enable ? "Proxy enabled\n" : "Proxy restored\n", enable ? 14 : 15);
    _exit(0);
    return 0;
}
