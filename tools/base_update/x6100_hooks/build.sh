#!/bin/sh
# The hooks of x6100_patch.py assembled (arm-none-eabi binutils): prints each fix's bytes as x6100_patch.py holds them.
# The code goes at the end of Xiegu's V1.1.9 image (0x0807fb70, still in its last flash sector), padded with 0xFF to
# whole 16-byte AES blocks: the image grows by that much (the base's bootloader takes any length: RF.Guru's test base
# is 13 KB longer than Xiegu's).
# SPDX-License-Identifier: LGPL-2.1-or-later
set -e
cd "$(dirname "$0")"
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
arm-none-eabi-as -mthumb -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 alc_hold.s -o "$t/h.o"
arm-none-eabi-ld -o "$t/h.elf" --section-start=.hook=0x08027f82 --section-start=.text=0x0807fb70 \
    --defsym=XIEGU_STORE=0x08027f8a "$t/h.o"
arm-none-eabi-objcopy -O binary -j .hook "$t/h.elf" "$t/hook.bin"
arm-none-eabi-objcopy -O binary -j .text "$t/h.elf" "$t/code.bin"
echo "hook 0x08027f82 $(xxd -p "$t/hook.bin" | tr -d '\n')"
n=$(wc -c < "$t/code.bin" | tr -d ' ')
head -c $(( (16 - n % 16) % 16 )) /dev/zero | tr '\0' '\377' >> "$t/code.bin"
echo "code 0x0807fb70 $(xxd -p "$t/code.bin" | tr -d '\n') ($n bytes, $(wc -c < "$t/code.bin" | tr -d ' ') with the 0xFF padding)"
arm-none-eabi-objdump -d "$t/h.elf" | sed -n '/<alc_hold>:/,$p'
