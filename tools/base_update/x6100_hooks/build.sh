#!/bin/sh
# The hooks of x6100_patch.py assembled (arm-none-eabi binutils): prints each fix's bytes as x6100_patch.py holds them.
# Their code goes at the end of Xiegu's V1.1.9 image, one after the other, each padded with 0xFF to whole 16-byte AES
# blocks: alc_hold ("hold") at 0x0807fb70, tx_iq ("txiq") at 0x0807fc50, all still in the last flash sector and
# before 0x08080000 (RF.Guru's payload; the base's bootloader takes any length: their test base is 13 KB longer).
# SPDX-License-Identifier: LGPL-2.1-or-later
set -e
cd "$(dirname "$0")"
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT

# name source hook-address code-address symbol=address
build() {
    arm-none-eabi-as -mthumb -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 "$2" -o "$t/$1.o"
    arm-none-eabi-ld -o "$t/$1.elf" -e "$1" --section-start=.hook="$3" --section-start=.text="$4" --defsym="$5" \
        "$t/$1.o"
    arm-none-eabi-objcopy -O binary -j .hook "$t/$1.elf" "$t/$1.hook"
    arm-none-eabi-objcopy -O binary -j .text "$t/$1.elf" "$t/$1.code"
    echo "$1 hook $3 $(xxd -p "$t/$1.hook" | tr -d '\n')"
    n=$(wc -c < "$t/$1.code" | tr -d ' ')
    head -c $(( (16 - n % 16) % 16 )) /dev/zero | tr '\0' '\377' >> "$t/$1.code"
    echo "$1 code $4 $(xxd -p "$t/$1.code" | tr -d '\n') ($n bytes, $(wc -c < "$t/$1.code" | tr -d ' ') with the 0xFF padding)"
    arm-none-eabi-objdump -d "$t/$1.elf" | sed -n "/<$1>:/,\$p"
}
build alc_hold alc_hold.s 0x08027f82 0x0807fb70 XIEGU_STORE=0x08027f8a
build tx_iq tx_iq.s 0x08026560 0x0807fc50 XIEGU_NEXT=0x08026564
