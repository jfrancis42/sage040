#!/usr/bin/env bash
# compare.sh - fixed point vs the 68040 FPU, same program either way.
#
# Runs both variants under -icount so virtual time is deterministic and
# proportional to instruction count, then converts the measured frame
# rate into instructions per frame.
set -eu
cd "$(dirname "$0")"
SAGE_QEMU="$HOME/m68k/sage040-qemu/bin/qemu-system-m68k"
[ -x "$SAGE_QEMU" ] || SAGE_QEMU=qemu-system-m68k
QEMU="${QEMU:-$SAGE_QEMU}"
SHIFT="${SPEED:-6}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
RATE=$((1000000000 / (1 << SHIFT)))     # modelled instructions per second

run() {  # $1 = elf, $2 = tag
    local out="$TMP/cmp-$2.out"
    rm -f "$out"
    timeout 120 "$QEMU" -M sage040 -m 4 -icount "shift=$SHIFT,sleep=off" \
        -kernel "$1" -serial "file:$out" -display none >/dev/null 2>&1 &
    local p=$!
    for _ in $(seq 1 600); do grep -q "pacing to" "$out" 2>/dev/null && break; sleep 0.15; done
    kill $p 2>/dev/null || true; wait $p 2>/dev/null || true
}

run cube.elf       fixed
run cube-float.elf float

printf '\n%-22s %12s %12s\n' "" "fixed point" "68040 FPU"
printf '%-22s %12s %12s\n' "---------------------" "------------" "------------"

for m in "CPU stores" "2D engine"; do
    a=$(grep -o "$m *[0-9]*" "$TMP/cmp-fixed.out" | awk '{print $NF}')
    b=$(grep -o "$m *[0-9]*" "$TMP/cmp-float.out" | awk '{print $NF}')
    printf '%-22s %9s fps %9s fps\n' "frames/sec, $m" "$a" "$b"
    printf '%-22s %12s %12s\n' "  instructions/frame" "$((RATE / a))" "$((RATE / b))"
done

fa=$(stat -c%s cube.elf); fb=$(stat -c%s cube-float.elf)
printf '%-22s %12s %12s\n' "ELF size (bytes)" "$fa" "$fb"
na=$(~/m68k/install/bin/m68k-elf-objdump -d cube.elf | grep -cE '^\s+[0-9a-f]+:.*\sf(s|d)?(move|add|sub|mul|div|sqrt|neg|abs|cmp|tst|movem)' || true)
nb=$(~/m68k/install/bin/m68k-elf-objdump -d cube-float.elf | grep -cE '^\s+[0-9a-f]+:.*\sf(s|d)?(move|add|sub|mul|div|sqrt|neg|abs|cmp|tst|movem)' || true)
printf '%-22s %12s %12s\n' "FP instructions" "$na" "$nb"

echo
echo "projected geometry, same orientation:"
diff <(sed -n '/geometry at/,/^$/p' "$TMP/cmp-fixed.out") \
     <(sed -n '/geometry at/,/^$/p' "$TMP/cmp-float.out") \
  && echo "  identical" || echo "  ^ fixed point on the left, FPU on the right"
