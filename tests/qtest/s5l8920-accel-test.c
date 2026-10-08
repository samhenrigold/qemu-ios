/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The S5L8920 boards' LIS302DL reads as each board mounts it. The N88's DT
 * accelerometer orientation is 7 where the N18's (and the N72's) is 4: two
 * bits more, which negate x and y in AppleLIS302DL. Without the N88's mount the
 * 3GS took Home-left (4) for Home-right (3) and drew landscape upside down.
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

/* sign: +1 when the sensor reads the device's axes as the N18's part, -1 when x and y are negated. */
static void check(const char *machine, int sign)
{
    QTestState *q = qtest_initf("-machine %s,kboot=%s,wifi=off -display none -audio driver=none -nic none",
                                machine, kboot);

    /* Portrait at power-on, before any set: gravity along -y on the N18's part. */
    g_assert_cmpint(axis(q, "accel-y"), ==, -64 * sign);
    orient(q, 4);                                     /* Home left: the right edge down */
    g_assert_cmpint(axis(q, "accel-x"), ==, 64 * sign);
    g_assert_cmpint(axis(q, "accel-y"), ==, 0);
    orient(q, 3);                                     /* Home right */
    g_assert_cmpint(axis(q, "accel-x"), ==, -64 * sign);
    orient(q, 5);                                     /* face up: z unchanged by either mount */
    g_assert_cmpint(axis(q, "accel-z"), ==, -64);
    qtest_quit(q);
}

static void n18(void) { check("n18", 1); }
static void n88(void) { check("n88", -1); }

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
    qtest_add_func("/s5l8920/accel/n18-mount", n18);
    qtest_add_func("/s5l8920/accel/n88-mount", n88);
    ret = g_test_run();
    unlink(kboot);
    g_free(kboot);
    return ret;
}
