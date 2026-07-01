#!/usr/bin/env bash
# CARMIX hardware bring-up Cycle 1: build hwtest.elf, boot ONE QEMU (q35 provides real
# ACPI/IOAPIC/LAPIC), capture serial. kernel.c and the proven modules NOT touched
# (swcap + store_mem LINKED only).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
LIM="${LIMINE_DIR:?}"; X="${X86TOOLS:?}"; CL="${CVSASX_CLANG:?}"; LD="${CVSASX_LLD:?}"; NM="${CVSASX_NM:?}"
ROOT="$(dirname "$HERE")"; ST="$ROOT/store"; CX="$ROOT/carmix"; CAP="$ROOT/cap"
OUT="$(mktemp -d)"
export LD_LIBRARY_PATH="$X/usr/lib/x86_64-linux-gnu:$X/lib/x86_64-linux-gnu${QEMU_LIBS:+:$QEMU_LIBS}"
KF="--target=x86_64-unknown-none-elf -ffreestanding -nostdlib -fno-stack-protector -fno-pic -fno-pie \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mgeneral-regs-only -mcmodel=kernel -O2 -Wall -Wextra \
    -Wno-interrupt-service-routine -Wno-sign-compare -I $LIM -I $HERE -I $ST -I $CX -I $CAP -I $ST/freestanding"

echo "=== build hwtest + swcap gate (LINKED, proven core untouched) ==="
$CL $KF -c "$HERE/hwtest.c"        -o "$OUT/hwtest.o"
$CL $KF -c "$ST/store_mem.c"       -o "$OUT/store_mem.o"
$CL $KF -c "$CX/swcap.c"           -o "$OUT/swcap.o"
"$LD" -m elf_x86_64 -static -T "$HERE/linker.ld" --build-id=none -z noexecstack -o "$OUT/hwtest.elf" "$OUT"/*.o
echo "link OK: hwtest.elf = $(stat -c%s "$OUT/hwtest.elf") bytes"
echo "undefined symbols : $("$NM" "$OUT/hwtest.elf" 2>/dev/null | grep -c ' U ' || true)  (expect 0)"

ISO=$OUT/iso; mkdir -p "$ISO/boot/limine" "$ISO/EFI/BOOT"
cp "$OUT/hwtest.elf" "$ISO/boot/kernel.elf"
cp "$LIM/limine-bios.sys" "$LIM/limine-bios-cd.bin" "$LIM/limine-uefi-cd.bin" "$ISO/boot/limine/"
cp "$LIM/BOOTX64.EFI" "$ISO/EFI/BOOT/"
cat > "$ISO/boot/limine/limine.conf" <<'EOF'
timeout: 0
serial: yes

/CARMIX-HWTEST
    protocol: limine
    path: boot():/boot/kernel.elf
EOF
( cd "$OUT" && "$X/usr/bin/xorriso" -as mkisofs -R -r -J \
    -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table \
    --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
    --protective-msdos-label iso -o hwtest.iso ) 2>&1 | tail -1
"$LIM/limine" bios-install "$OUT/hwtest.iso" 2>&1 | tail -1
cp "$OUT/hwtest.iso" "$HERE/hwtest.iso"

echo "=== boot ONE QEMU (q35: real ACPI + IOAPIC + LAPIC) ==="
QEMU="$X/usr/bin/qemu-system-x86_64"; LOG="$HERE/serial_hw.log"; rm -f "$LOG"
timeout 90 "$QEMU" -M q35 -m 512M -cdrom "$OUT/hwtest.iso" -serial "file:$LOG" -display none -no-reboot \
    -L "$X/usr/share/seabios" -L "$X/usr/share/qemu" >/dev/null 2>&1 || true
echo "================= SERIAL (hwtest) ================="
cat "$LOG" 2>/dev/null || echo "(no log)"
rm -rf "$OUT"
