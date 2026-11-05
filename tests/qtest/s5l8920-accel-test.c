/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The kboot boards' accelerometer reads as each board mounts it, from the board's DT orientation (ios_dt_mount):
 * the N18's 4 and the N88's 7 (AppleLIS302DL bits), the iPad's, the iPod touch 4G's and the iPhone 4's matrices
 * (AppleLIS331DLH). Each board's expected mount is the one found on hardware-checked rotation (the iPhone 4's z by
 * its compass; the iPod touch 4G's z by a CoreMotion probe on 6.1.6). Read as the N18's part, the 3GS took Home-left (4) for
 * Home-right (3) and drew landscape upside down (issues 23, 43).
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

static char *kboot;

static int64_t axis(QTestState *q, const char *name)
{
    QDict *r = qtest_qmp(q, "{ 'execute': 'qom-get', 'arguments': { 'path': '/machine', 'property': %s } }", name);
    int64_t v = qdict_get_int(r, "return");

    qobject_unref(r);
    return v;
}

static void orient(QTestState *q, int o)
{
    qobject_unref(qtest_qmp(q, "{ 'execute': 'qom-set', 'arguments': { 'path': '/machine', "
                               "'property': 'accel-orientation', 'value': %d } }", o));
}

typedef struct Board {
    const char *machine;
    int mount[3];           /* sensor axis i reads device axis |m| - 1, negated if m < 0 */
} Board;

/* The sensor's reading of device gravity g, in 1/64 g. */
static void expect(QTestState *q, const Board *b, const int g[3])
{
    static const char *const names[3] = { "accel-x", "accel-y", "accel-z" };

    for (int i = 0; i < 3; i++) {
        int a = abs(b->mount[i]) - 1;
        g_assert_cmpint(axis(q, names[i]), ==, (b->mount[i] < 0 ? -64 : 64) * g[a]);
    }
}

static void check(const void *data)
{
    const Board *b = data;
    QTestState *q = qtest_initf("-machine %s,kboot=%s,wifi=off -display none -audio driver=none -nic none",
                                b->machine, kboot);

    expect(q, b, (const int[3]){ 0, -1, 0 });         /* portrait at power-on, before any set */
    orient(q, 4);                                     /* Home left: the right edge down */
    expect(q, b, (const int[3]){ 1, 0, 0 });
    orient(q, 3);                                     /* Home right */
    expect(q, b, (const int[3]){ -1, 0, 0 });
    orient(q, 5);                                     /* face up */
    expect(q, b, (const int[3]){ 0, 0, -1 });
    qtest_quit(q);
}

/* The 3GS's AK8973 takes the app's Compass Heading under the iPad's property name. */
static void n88_compass(void)
{
    QTestState *q = qtest_initf("-machine n88,kboot=%s,wifi=off -display none -audio driver=none -nic none", kboot);

    qobject_unref(qtest_qmp(q, "{ 'execute': 'qom-set', 'arguments': { 'path': '/machine', "
                               "'property': 'compass-heading', 'value': 450 } }"));
    g_assert_cmpint(axis(q, "compass-heading"), ==, 90);
    qtest_quit(q);
}

static const Board boards[] = {
    { "n18", { 1, 2, 3 } },
    { "n88", { -1, -2, 3 } },
    { "ipad1", { 1, -2, -3 } },
    { "iPod-Touch-4G", { -2, -1, 3 } },
    { "iPhone-4", { -2, 1, 3 } },
};

int main(int argc, char **argv)
{
    /* A K48KBOOT bundle with an empty image: the trailer only. */
    static const uint8_t trailer[24] = {
        'K', '4', '8', 'K', 'B', 'O', 'O', 'T',
        0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40,
        0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00,
    };
    int fd, ret;

    g_test_init(&argc, &argv, NULL);
    fd = g_file_open_tmp("s5l8920-kboot-XXXXXX", &kboot, NULL);
    g_assert(fd >= 0);
    g_assert(write(fd, trailer, sizeof(trailer)) == sizeof(trailer));
    close(fd);
    for (size_t i = 0; i < ARRAY_SIZE(boards); i++) {
        g_autofree char *path = g_strdup_printf("/s5l8920/accel/%s-mount", boards[i].machine);
        qtest_add_data_func(path, &boards[i], check);
    }
    qtest_add_func("/s5l8920/compass/n88-heading", n88_compass);
    ret = g_test_run();
    unlink(kboot);
    g_free(kboot);
    return ret;
}
