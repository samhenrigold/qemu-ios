/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A wake from hibernate on the kboot boards (hw/arm/s5l8920.c, hw/arm/ipad1.c),
 * which run neither the ROM nor LLB: the kernel's "pmu go hib" (the suspend
 * marker in PMU scratch, then the power command's bit 1) turns the AP off, Home
 * turns it on, and the machine resumes as LLB does. DRAM keeps the kernel's
 * trampoline (kboot is not staged again), 'XSOM' 'PSUS' at DRAM + 0x80 is
 * cleared, the scratch marker reads "resumed" and I2C0 is set up as LLB leaves
 * it (CTRL 0x30), which the kernel's I2C driver resumes on, and the PMGR
 * timebase keeps counting (xnu's absolute time must not go back). Without the DRAM marker a
 * wake boots: kboot's image is staged over DRAM's base again.
 */
#include "qemu/osdep.h"
#include "libqtest.h"

#define DRAM        0x40000000
#define I2C0        0x83200000
#define PMU         0x74
#define TRAMPOLINE  0xe28fff7eu     /* what 6.1.6 leaves at DRAM's base */
#define IMAGE_WORD  0x11111111u     /* kboot's image */

static char *kboot;

typedef struct {
    const char *machine;
    uint8_t scratch, power, hib;    /* the suspend marker, the power command and its hibernate value */
    uint32_t ticks;                 /* the PMGR timebase's low word */
} Board;

static void pmu_write(QTestState *q, uint8_t reg, uint8_t val)
{
    qtest_writel(q, I2C0 + 0x00, PMU);
    qtest_writel(q, I2C0 + 0x10, reg);
    qtest_writel(q, I2C0 + 0x18, 1);
    qtest_writel(q, I2C0 + 0x20, val);
    qtest_writel(q, I2C0 + 0x24, 5);    /* start, write */
}

static uint8_t pmu_read(QTestState *q, uint8_t reg)
{
    qtest_writel(q, I2C0 + 0x00, PMU);
    qtest_writel(q, I2C0 + 0x10, reg);
    qtest_writel(q, I2C0 + 0x18, 1);
    qtest_writel(q, I2C0 + 0x24, 4);    /* start, read */
    return qtest_readl(q, I2C0 + 0x20);
}

/* The reset a wake requests runs from the main loop: wait for its effect. */
static uint32_t wait_word(QTestState *q, uint64_t addr, uint32_t want)
{
    uint32_t v = 0;

    for (int i = 0; i < 200 && (v = qtest_readl(q, addr)) != want; i++) {
        g_usleep(10 * 1000);
    }
    return v;
}

/* Hibernate, then Home. */
static void hibernate_and_wake(QTestState *q, const Board *b)
{
    QDict *r;

    pmu_write(q, b->scratch, 0x80);
    pmu_write(q, b->power, b->hib);
    qtest_clock_step(q, 30 * 1000 * 1000);     /* the rail drops 20 ms after the command */
    r = qtest_qmp(q, "{'execute': 'qom-set', 'arguments': {'path': '/machine', 'property': 'button-home', "
                     "'value': true}}");
    qobject_unref(r);
    r = qtest_qmp(q, "{'execute': 'qom-set', 'arguments': {'path': '/machine', 'property': 'button-home', "
                     "'value': false}}");
    qobject_unref(r);
}

static void check(const void *data)
{
    const Board *b = data;
    QTestState *q = qtest_initf("-machine %s,kboot=%s,wifi=off -display none -audio driver=none -nic none",
                                b->machine, kboot);

    /* Asleep with the kernel's marker: the wake resumes. */
    g_assert_cmphex(qtest_readl(q, DRAM), ==, IMAGE_WORD);
    qtest_writel(q, DRAM, TRAMPOLINE);
    qtest_writel(q, DRAM + 0x80, 0x4d4f5358);   /* 'XSOM' */
    qtest_writel(q, DRAM + 0x84, 0x53555350);   /* 'PSUS' */
    qtest_clock_step(q, 1000 * 1000 * 1000);
    uint32_t t0 = qtest_readl(q, b->ticks);
    g_assert_cmpuint(t0, >=, 24000000);                       /* 1 s of 24 MHz */
    hibernate_and_wake(q, b);
    g_assert_cmphex(wait_word(q, DRAM + 0x80, 0), ==, 0);
    g_assert_cmpuint(qtest_readl(q, b->ticks), >=, t0);       /* the timebase did not restart */
    g_assert_cmphex(qtest_readl(q, DRAM + 0x84), ==, 0);
    g_assert_cmphex(qtest_readl(q, DRAM), ==, TRAMPOLINE);
    g_assert_cmphex(qtest_readl(q, 0), ==, TRAMPOLINE);         /* what the CPU starts at */
    g_assert_cmphex(qtest_readl(q, I2C0 + 0x08), ==, 0x30);    /* LLB's I2C setup */
    g_assert_cmphex(pmu_read(q, b->scratch), ==, 0x40);
    g_assert_cmphex(pmu_read(q, 0x01) & 3, ==, 1);             /* Home's wake event */

    /* No DRAM marker: LLB boots, and kboot is staged again. */
    hibernate_and_wake(q, b);
    g_assert_cmphex(wait_word(q, DRAM, IMAGE_WORD), ==, IMAGE_WORD);
    qtest_quit(q);
}

static const Board boards[] = {
    { "n88", 0x6f, 0x0d, 0x26, 0xbf100200 },   /* D1755: 8C148 "pmu go hib" sets 0x26 in 0x0d */
    { "n18", 0x6f, 0x0d, 0x26, 0xbf100200 },
    { "iPhone-4", 0x8f, 0x12, 0x02, 0xbf102000 },  /* D1815: 0x8F, then 0x12 bit 1 */
};

int main(int argc, char **argv)
{
    /* A K48KBOOT bundle: one word of image at DRAM's base, entered there. */
    static const uint8_t bundle[4 + 24] = {
        0x11, 0x11, 0x11, 0x11,
        'K', '4', '8', 'K', 'B', 'O', 'O', 'T',
        0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40,
        0x00, 0x00, 0x00, 0x40, 0x04, 0x00, 0x00, 0x00,
    };
    int fd, ret;

    g_test_init(&argc, &argv, NULL);
    fd = g_file_open_tmp("s5l8920-hib-XXXXXX", &kboot, NULL);
    g_assert(fd >= 0);
    g_assert(write(fd, bundle, sizeof(bundle)) == sizeof(bundle));
    close(fd);
    for (int i = 0; i < ARRAY_SIZE(boards); i++) {
        g_autofree char *path = g_strdup_printf("/s5l8920/hibernate/%s", boards[i].machine);
        qtest_add_data_func(path, &boards[i], check);
    }
    ret = g_test_run();
    unlink(kboot);
    g_free(kboot);
    return ret;
}
