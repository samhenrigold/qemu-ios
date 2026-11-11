/* Temporary native preparation worker, never installed in System. */
#define IT_MG_FULL_SYSTEM 1
#include "mobilegestalt.c"
const char it_mobilegestalt_worker_version[] = "it_mobilegestalt_worker_v1";
const char it_mobilegestalt_container_version[] =
    "it_mobilegestalt_container_v1";
const char it_mobilegestalt_readable_cache_version[] =
    "it_mobilegestalt_readable_cache_v1";
int main(void)
{
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
    return it_prepare_mobilegestalt(stdout) ? 1 : 0;
}
