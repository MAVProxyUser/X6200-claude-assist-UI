#!/bin/sh
# mkpatch.sh: x6x00_dmafix as an extra_patch for .github/workflows/rtw88.yml (it builds every obj-m of the rtw88 tree
# against the X6200's kernel kit and uploads the .ko files): the three files added, one obj-m line in rtw88's Makefile.
# Prints the base64 of its gzip:
#   gh workflow run rtw88.yml -f kit_run=<a kit run> -f extra_patch="$(sh tools/audio_dma/mkpatch.sh)"
D=$(cd "$(dirname "$0")/x6x00_dmafix" && pwd)
{
    for f in x6x00_dmafix.c x6x00_virt-dma.h x6x00_dmaengine.h; do
        n=$(wc -l < "$D/$f" | tr -d ' ')
        printf -- '--- /dev/null\n+++ b/%s\n@@ -0,0 +1,%s @@\n' "$f" "$n"
        sed 's/^/+/' "$D/$f"
    done
    printf -- '--- a/Makefile\n+++ b/Makefile\n@@ -45,0 +46 @@\n+obj-m\t\t+= x6x00_dmafix.o\n'
} | gzip -9c | base64 | tr -d '\n'
