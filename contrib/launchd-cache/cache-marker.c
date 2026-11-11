/* launchd locates this image with dlsym, then reads __TEXT,__xpcd_cache.
 * The metadata section is supplied by FirmwareKit; no launchd code is changed. */
int __xpcd_cache(void)
{
    return 1;
}
