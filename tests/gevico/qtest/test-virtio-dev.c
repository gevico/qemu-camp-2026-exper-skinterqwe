/*
 * QTest: G233 VirtIO device placeholder
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    return g_test_run();
}
