/* it_boot (contrib/it-boot) on the host: it-boot-test.sh builds it with it-boot-test-host.h. */
#include "it_boot.c"
int main(int argc, char **argv) {
    assert(argc == 2);
    snprintf(fake_dir, sizeof(fake_dir), "%s", argv[1]);
    (void)fake_time;
    struct offer maximum = {0}; struct pull_clock clock;
    maximum.nent = MAX_ENT;
    for (int i = 0; i < MAX_ENT; i++) maximum.e[i].size = FILE_MAX;
    assert(pull_clock_start(&clock, &maximum) == 0);
    assert(clock.seconds == PULL_MAX_SECONDS);
    printf("%d\n", it_boot_run());
    return 0;
}
