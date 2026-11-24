/*
 * The GUI's own SSTV encoder (src/sstv/sstv_tx.c) on this computer, for the bench: a PNG made into the audio of one
 * SSTV picture, VIS header first, as 16-bit mono little-endian at the rate asked (tci_tone_tx.py plays 24 kHz files).
 *
 *   sstv_enc MODE IN.png OUT.raw RATE        MODE: robot8 robot36 robot72 martin1 martin2 scottie1 scottie2
 *
 * orch.py builds it as it needs it (cc, libpng from pkg-config) into ~/.config/x6x00-bench.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sstv_tx.h"

static const struct { const char *name; sstv_mode_t mode; } modes[] = {
    {"robot8", SSTV_ROBOT8},   {"robot36", SSTV_ROBOT36},   {"robot72", SSTV_ROBOT72}, {"martin1", SSTV_MARTIN1},
    {"martin2", SSTV_MARTIN2}, {"scottie1", SSTV_SCOTTIE1}, {"scottie2", SSTV_SCOTTIE2},
};

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: %s MODE IN.png OUT.raw RATE\n", argv[0]);
        return 2;
    }
    sstv_mode_t mode = SSTV_UNKNOWN;
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
        if (!strcmp(argv[1], modes[i].name)) mode = modes[i].mode;
    if (mode == SSTV_UNKNOWN) {
        fprintf(stderr, "%s: no mode %s\n", argv[0], argv[1]);
        return 2;
    }
    int16_t *pcm = NULL;
    size_t   n = 0;
    int      r = sstv_tx_pcm(mode, argv[2], (unsigned)atoi(argv[4]), &pcm, &n);
    if (r != 0) {
        fprintf(stderr, "%s: sstv_tx_pcm %d\n", argv[0], r);
        return 1;
    }
    FILE *f = fopen(argv[3], "wb");
    if (!f || fwrite(pcm, sizeof(int16_t), n, f) != n || fclose(f) != 0) {
        fprintf(stderr, "%s: cannot write %s\n", argv[0], argv[3]);
        return 1;
    }
    free(pcm);
    printf("%zu samples\n", n);
    return 0;
}
