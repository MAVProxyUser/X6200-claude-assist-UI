#!/bin/sh
# 30-wifi-channel (rootfs/etc/NetworkManager/dispatcher.d) with stand-ins for iw, gdbus and logger: runs anywhere
# with a POSIX sh and awk (a Mac too), nothing of this machine's network touched. The stand-in iw answers `link` and
# `scan dump` from files written per case, the stand-in gdbus answers the supplicant's calls as an X6200's does
# (the channel limit makes it rejoin elsewhere, as the X6200's did) and notes them, with logger's lines, in order.
#
#  1. An X6100 on channel 11: nothing asked.          2. An X6200's link going down, or not WiFi: nothing asked.
#  3. An X6200 on channel 6: nothing asked.           4. On channel 11, the network on channel 6 too (and another
#  5. Two others: the stronger is taken.                 network stronger there): limited, on channel 6 after.
#  6. Only a weak other (-85 dBm): one scan, stays.   7. The join's scan gone stale: one scan, then moved.
#  8. Not moved by the limit: asked to roam.          9. Not moved at all: said so.
# 10. Channel 10 is left too.                        11. A name with iw's \x escapes matches.
# 12. The supplicant without the link: says so, stays.
# 13. Another below the floor (-76 dBm, the X6200's case on 2026-11-21): one scan, stays.
HOOK=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
W=$(mktemp -d "${TMPDIR:-/tmp}/wifi-channel.XXXXXX")
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/bin" "$W/net/wlan0" "$W/net/eth0"
touch "$W/net/wlan0/phy80211"

cat > "$W/bin/iw" << EOF
#!/bin/sh
case "\$*" in
    *" link") cat "$W/link" ;;
    *" scan dump") cat "$W/dump" 2> /dev/null ;;
esac
EOF
cat > "$W/bin/logger" << EOF
#!/bin/sh
shift 2
echo "log \$*" >> "$W/asked"
EOF
cat > "$W/bin/gdbus" << EOF
#!/bin/sh
m=
while [ \$# -gt 0 ]; do
    [ "\$1" = --method ] && { m=\$2; shift 2; break; }
    shift
done
case "\$m" in
    *.GetInterface) [ -e "$W/no_iface" ] || echo "(objectpath '/fi/w1/wpa_supplicant1/Interfaces/0',)" ;;
    *.Properties.Get) echo "(<objectpath '/fi/w1/wpa_supplicant1/Interfaces/0/Networks/0'>,)" ;;
    *.Properties.Set) echo "Set \$3" >> "$W/asked"; [ -e "$W/after_set" ] && cp "$W/after_set" "$W/link"; echo "()" ;;
    *.Scan) echo "Scan" >> "$W/asked"; [ -e "$W/after_scan" ] && cp "$W/after_scan" "$W/dump"; echo "()" ;;
    *.Roam) echo "Roam \$1" >> "$W/asked"; [ -e "$W/refuse" ] && exit 1
        [ -e "$W/after_roam" ] && cp "$W/after_roam" "$W/link"; echo "()" ;;
esac
EOF
chmod +x "$W/bin/"*

# An access point as iw's scan dump shows it: bssid, MHz, dBm, name
ap() {
    printf 'BSS %s(on wlan0)\n\tlast seen: 1200.000s [boottime]\n\tTSF: 0 usec (0d, 00:00:00)\n\tfreq: %s.0\n' "$1" "$2"
    printf '\tbeacon interval: 100 TUs\n\tcapability: ESS Privacy ShortSlotTime (0x0411)\n\tsignal: %s.00 dBm\n' "$3"
    printf '\tlast seen: 40 ms ago\n\tInformation elements from Probe Response frame:\n\tSSID: %s\n' "$4"
    printf '\tDS Parameter set: channel %s\n\tVHT operation:\n\t\t * channel width: 0 (20 or 40 MHz)\n' \
        $(( ($2 - 2407) / 5 ))
    printf '\t\t * center freq segment 1: 0\n'
}
# A link as iw shows it: bssid, MHz, dBm, name
link() {
    printf 'Connected to %s (on wlan0)\n\tSSID: %s\n\tfreq: %s.0\n\tRX: 1 bytes (1 packets)\n' "$1" "$4" "$2"
    printf '\tsignal: %s dBm\n\ttx bitrate: 26.0 MBit/s MCS 3\n' "$3"
}
LIST="2412 2417 2422 2427 2432 2437 2442 2447 2467 2472 2484"
SET="Set <{'freq_list': <'$LIST'>}>"
C11=02:00:5e:00:00:0b C6=02:00:5e:00:00:06 C1=02:00:5e:00:00:01

fails=0
check() {   # what, wanted, got
    if [ "$2" = "$3" ]; then
        echo "ok   $1"
    else
        echo "FAIL $1"
        echo "     wanted [$2]"
        echo "     got    [$3]"
        fails=$((fails + 1))
    fi
}
run() {     # model, interface, action: what the hook asked, in order
    echo "$1" > "$W/compat"
    rm -f "$W/asked"
    PATH="$W/bin:$PATH" X6X00_DT="$W/compat" X6X00_NET="$W/net" sh "$HOOK" "$2" "$3"
    echo "exit $?" >> "$W/asked"
    tr '\n' '|' < "$W/asked"
}
fresh() {   # a new case on channel 11: the limit moves it to channel 6 unless the case says otherwise
    rm -f "$W/refuse" "$W/after_scan" "$W/after_roam" "$W/no_iface" "$W/dump"
    link $C11 2462 -58 HomeNet > "$W/link"
    link $C6 2437 -62 HomeNet > "$W/after_set"
}
X6200="xiegu,x6200 allwinner,sun8i-r16"
X6100="xiegu,x6100 allwinner,sun8i-r16"
ON6="log wlan0: off channel 11, the X6200's own: on channel 6 now ($C6, -62 dBm)|exit 0|"

fresh
{ ap $C11 2462 -58 HomeNet; ap $C6 2437 -62 HomeNet; ap $C1 2412 -83 HomeNet; ap 02:00:5e:00:00:99 2437 -40 Other; } \
    > "$W/dump"
check "1. an X6100 on channel 11" "exit 0|" "$(run "$X6100" wlan0 up)"
check "2. an X6200's link going down" "exit 0|" "$(run "$X6200" wlan0 down)"
check "2. a link that is not WiFi" "exit 0|" "$(run "$X6200" eth0 up)"
link $C6 2437 -62 HomeNet > "$W/link"
check "3. an X6200 on channel 6" "exit 0|" "$(run "$X6200" wlan0 up)"
link $C11 2462 -58 HomeNet > "$W/link"
check "4. an X6200 on channel 11, the network on channel 6 too" "$SET|$ON6" "$(run "$X6200" wlan0 up)"

fresh; { ap $C11 2462 -58 HomeNet; ap $C6 2437 -62 HomeNet; ap $C1 2412 -55 HomeNet; } > "$W/dump"
rm "$W/after_set"; link $C1 2412 -55 HomeNet > "$W/after_roam"
check "5. the stronger of two others (roamed to)" \
    "$SET|Roam $C1|log wlan0: off channel 11, the X6200's own: on channel 1 now ($C1, -55 dBm)|exit 0|" \
    "$(run "$X6200" wlan0 up)"

fresh; { ap $C11 2462 -58 HomeNet; ap $C6 2437 -85 HomeNet; } > "$W/dump"
check "6. only a weak other: one scan, stays" \
    "Scan|log wlan0: on channel 11, the X6200's own: no other access point of this network at -65 dBm or better, staying|exit 0|" \
    "$(run "$X6200" wlan0 up)"

fresh; ap $C11 2462 -58 HomeNet > "$W/dump"
{ ap $C11 2462 -58 HomeNet; ap $C6 2437 -62 HomeNet; } > "$W/after_scan"
check "7. the join's scan gone stale: one scan, then moved" "Scan|$SET|$ON6" "$(run "$X6200" wlan0 up)"

fresh; { ap $C11 2462 -58 HomeNet; ap $C6 2437 -62 HomeNet; } > "$W/dump"
rm "$W/after_set"; link $C6 2437 -62 HomeNet > "$W/after_roam"
check "8. not moved by the limit: asked to roam" "$SET|Roam $C6|$ON6" "$(run "$X6200" wlan0 up)"

fresh; { ap $C11 2462 -58 HomeNet; ap $C6 2437 -62 HomeNet; } > "$W/dump"
rm "$W/after_set"; touch "$W/refuse"
check "9. not moved at all" \
    "$SET|Roam $C6|log wlan0: off channel 11, the X6200's own: not moved to $C6 on channel 6 (-62 dBm), the supplicant did not|exit 0|" \
    "$(run "$X6200" wlan0 up)"

fresh; link $C11 2457 -58 HomeNet > "$W/link"; { ap $C11 2457 -58 HomeNet; ap $C6 2437 -62 HomeNet; } > "$W/dump"
check "10. channel 10 left too" \
    "$SET|log wlan0: off channel 10, the X6200's own: on channel 6 now ($C6, -62 dBm)|exit 0|" "$(run "$X6200" wlan0 up)"

fresh; link $C11 2462 -58 'Home\x41Net' > "$W/link"
{ ap $C11 2462 -58 'Home\x41Net'; ap $C6 2437 -62 'Home\x41Net'; ap $C1 2412 -50 HomeANet; } > "$W/dump"
check "11. a name with iw's escapes" "$SET|$ON6" "$(run "$X6200" wlan0 up)"

fresh; { ap $C11 2462 -58 HomeNet; ap $C6 2437 -62 HomeNet; } > "$W/dump"; touch "$W/no_iface"
check "12. the supplicant without the link" \
    "log wlan0: on channel 11, the X6200's own: the supplicant does not have it, staying|exit 0|" \
    "$(run "$X6200" wlan0 up)"

fresh; link $C11 2462 -50 HomeNet > "$W/link"; { ap $C11 2462 -50 HomeNet; ap $C6 2437 -76 HomeNet; } > "$W/dump"
check "13. another below the floor (the X6200's case, 2026-11-21): one scan, stays" \
    "Scan|log wlan0: on channel 11, the X6200's own: no other access point of this network at -65 dBm or better, staying|exit 0|" \
    "$(run "$X6200" wlan0 up)"

[ $fails = 0 ] && echo "all passed" || echo "$fails failed"
[ $fails = 0 ]
