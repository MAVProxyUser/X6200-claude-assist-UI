/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - hubpower: a USB hub's port powered off and on again, as unplugging what is in it would (the device's
 *  own power cut, not only its link: a USB-3000 whose AMBE-3000 hung came back only so). Through the hub's own class
 *  requests (ClearPortFeature/SetPortFeature PORT_POWER), as uhubctl does, with the hub left to the kernel. Only a hub
 *  that switches each port's power (wHubCharacteristics bits 1-0 = 01) is asked; "info" says what the hub can.
 *  The Genesys hub in the X6200 (05e3:0610) says it does, but a hung USB-3000 stayed hung through off 3 s, so its
 *  VBUS does not really follow (2026-11-16): a hub that does is still worth this.
 *
 *      hubpower <hub USB path, "3-1"> info
 *      hubpower <hub USB path> <port> [off seconds, 3]
 *
 *  Built for the radio as the SDR daemon is, with its libusb: arm-linux-gnueabihf-gcc --sysroot=build/x6200/sysroot
 *  -I build/x6200/rtlsdr/inc/libusb-1.0 hubpower.c -L<the radio's libs> -Wl,--allow-shlib-undefined -lusb-1.0
 */
#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    libusb_context       *ctx;
    libusb_device       **list;
    libusb_device_handle *h = NULL;
    uint8_t               d[16];
    ssize_t               n;
    int                   r = 1;

    if (argc < 3 || libusb_init(&ctx)) return 2;
    n = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; i < n && !h; i++) {
        uint8_t ports[8];
        int     np = libusb_get_port_numbers(list[i], ports, 8);
        char    path[32];
        int     at = snprintf(path, sizeof(path), "%u", libusb_get_bus_number(list[i]));

        for (int p = 0; p < np; p++) at += snprintf(path + at, sizeof(path) - (size_t)at, "%c%u", p ? '.' : '-', ports[p]);
        if (np > 0 && !strcmp(path, argv[1]) && libusb_open(list[i], &h)) h = NULL;
    }
    if (!h) {
        fprintf(stderr, "hubpower: no hub at %s\n", argv[1]);
        goto out;
    }
    /* the hub descriptor: its ports, wHubCharacteristics */
    int k = libusb_control_transfer(h, 0xA0, 6, 0x2900, 0, d, sizeof(d), 1000);

    if (k < 5) {
        fprintf(stderr, "hubpower: %s gave no hub descriptor (%d)\n", argv[1], k);
        goto out;
    }
    int nports = d[2], chars = d[3] | d[4] << 8, sw = chars & 3;

    printf("hub %s: %d ports, power switching %s\n", argv[1], nports,
           sw == 0 ? "ganged (all ports together)" : sw == 1 ? "per port" : "none");
    if (!strcmp(argv[2], "info")) {
        r = 0;
        goto out;
    }
    int port = atoi(argv[2]), secs = argc > 3 ? atoi(argv[3]) : 3;

    if (port < 1 || port > nports || sw != 1) {
        fprintf(stderr, "hubpower: not cutting port %d of %s (%s)\n", port, argv[1],
                sw != 1 ? "this hub does not switch each port's power" : "no such port");
        goto out;
    }
    if (libusb_control_transfer(h, 0x23, 1, 8, (uint16_t)port, NULL, 0, 1000) < 0) {      /* ClearPortFeature POWER */
        fprintf(stderr, "hubpower: port %d's power not cut\n", port);
        goto out;
    }
    printf("port %d off for %d s\n", port, secs);
    sleep((unsigned)secs);
    r = libusb_control_transfer(h, 0x23, 3, 8, (uint16_t)port, NULL, 0, 1000) < 0;       /* SetPortFeature POWER */
    printf("port %d %s\n", port, r ? "NOT powered again" : "on again");
out:
    if (h) libusb_close(h);
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return r;
}
