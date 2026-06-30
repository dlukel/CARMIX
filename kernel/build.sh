#!/usr/bin/env bash
# CARMIX kernel core. Build, make a bootable ISO, boot headless in qemu-system-x86_64.
# The toolchain comes from the env vars below. Limine provides the boot path.
# See docs/BUILD.md for the toolchain and the env vars. The build fails loudly if any is unset.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
LIM="${LIMINE_DIR:?set LIMINE_DIR to a Limine 9.x directory (limine tool, headers, boot blobs)}"
X="${X86TOOLS:?set X86TOOLS to a prefix holding qemu-system-x86_64 and xorriso under usr/bin}"
CL="${CVSASX_CLANG:?set CVSASX_CLANG to a freestanding x86-64 clang}"
LD="${CVSASX_LLD:?set CVSASX_LLD to ld.lld}"
NM="${CVSASX_NM:?set CVSASX_NM to llvm-nm}"
OD="${CVSASX_OBJDUMP:?set CVSASX_OBJDUMP to llvm-objdump}"
ROOT="$(dirname "$HERE")"; GT="$ROOT/gate"; CX="$ROOT/carmix"; CAP="$ROOT/cap"; ST="$ROOT/store"; SL="$ROOT/sls"
OUT="$(mktemp -d)"
export LD_LIBRARY_PATH="$X/usr/lib/x86_64-linux-gnu:$X/lib/x86_64-linux-gnu${QEMU_LIBS:+:$QEMU_LIBS}"
KF="--target=x86_64-unknown-none-elf -ffreestanding -nostdlib -fno-stack-protector -fno-pic -fno-pie \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mgeneral-regs-only -mcmodel=kernel -O2 -Wall -Wextra \
    -Wno-interrupt-service-routine -Wno-sign-compare \
    -I $LIM -I $HERE -I $GT -I $CX -I $CAP -I $ST -I $SL -I $ST/blake3 -I $ST/freestanding"
B3="-DNDEBUG -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512 -DBLAKE3_USE_NEON=0 -fno-builtin"

echo "=== build kernel + capability backend (gate/+carmix/) + content-addressing spine (BLAKE3/store/sls) ==="
$CL $KF ${KAUDIT:-} -c "$HERE/kernel.c"  -o "$OUT/kernel.o"   # KAUDIT=-DAUDIT_PROBE=N for audit probes
$CL $KF -c "$GT/sfi_checker.c"     -o "$OUT/sfi_checker.o"
$CL $KF -c "$GT/executor.c"        -o "$OUT/executor.o"
$CL $KF -c "$GT/wasm_frontend.c"   -o "$OUT/wasm_frontend.o"
$CL $KF -c "$GT/optimize.c"        -o "$OUT/optimize.o"
$CL $KF -c "$CX/swcap.c"           -o "$OUT/swcap.o"
$CL $KF -c "$CX/backend_sw_gate.c" -o "$OUT/backend_sw_gate.o"
$CL $KF     -c "$ST/store_mem.c"               -o "$OUT/store_mem.o"   # recursion-safe mem*/strlen
$CL $KF $B3 -c "$ST/object_store.c"            -o "$OUT/object_store.o"
$CL $KF $B3 -c "$ST/blake3_wrap.c"             -o "$OUT/blake3_wrap.o"
$CL $KF $B3 -c "$ST/blake3/blake3.c"           -o "$OUT/blake3.o"
$CL $KF $B3 -c "$ST/blake3/blake3_dispatch.c"  -o "$OUT/blake3_dispatch.o"  # x86 portable path (NO SIMD)
$CL $KF $B3 -c "$ST/blake3/blake3_portable.c"  -o "$OUT/blake3_portable.o"
$CL $KF     -c "$SL/sls.c"                     -o "$OUT/sls.o"
"$LD" -m elf_x86_64 -static -T "$HERE/linker.ld" --build-id=none -z noexecstack -o "$OUT/kernel.elf" "$OUT"/*.o
echo "E0 link OK: kernel.elf = $(stat -c%s "$OUT/kernel.elf") bytes (backend linked in)"
echo "E0 undefined symbols : $("$NM" "$OUT/kernel.elf" 2>/dev/null | grep -c ' U ' || true)  (expect 0)"
echo "E0 libc/host symbols : $("$NM" "$OUT/kernel.elf" 2>/dev/null | grep -ciE ' (malloc|calloc|realloc|free|printf|fprintf|fopen|fwrite|fread|abort|exit|__stack_chk_fail)$' || true)  (expect 0; whole-name match)"
echo "E0 CHERI instructions: $("$OD" -d "$OUT/kernel.elf" 2>/dev/null | grep -ciE 'cgetbase|csetbounds|csetaddr|candperm|cspecialr|jr\.cap' || true)  (expect 0)"

echo "=== bootable ISO (Limine BIOS+UEFI El-Torito) ==="
ISO=$OUT/iso; mkdir -p "$ISO/boot/limine" "$ISO/EFI/BOOT"
cp "$OUT/kernel.elf" "$ISO/boot/"
cp "$LIM/limine-bios.sys" "$LIM/limine-bios-cd.bin" "$LIM/limine-uefi-cd.bin" "$ISO/boot/limine/"
cp "$LIM/BOOTX64.EFI" "$ISO/EFI/BOOT/"
cat > "$ISO/boot/limine/limine.conf" <<'EOF'
timeout: 0
serial: yes

/CARMIX
    protocol: limine
    path: boot():/boot/kernel.elf
EOF
( cd "$OUT" && "$X/usr/bin/xorriso" -as mkisofs -R -r -J \
    -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table \
    --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
    --protective-msdos-label iso -o carmix.iso ) 2>&1 | tail -1
"$LIM/limine" bios-install "$OUT/carmix.iso" 2>&1 | tail -1
cp "$OUT/carmix.iso" "$HERE/carmix.iso"   # real-hardware-bootable (USB) + qemu

echo "=== headless boots (serial). Durable media: file-backed virtio-blk (legacy I/O BAR). ==="
# A cold power-cycle = a SEPARATE qemu process on the SAME host disk image: RAM is genuinely
# empty on the next boot; only the disk file carries state. SINGLE_BOOT=1 runs just one boot
# (fast driver iteration). Otherwise: boot1 persists, boot2 (cold reboot) revives, boot3
# (host-tampered image) shows detection.
DISK="$OUT/carmix-disk.img"; truncate -s 64M "$DISK"
QEMU="$X/usr/bin/qemu-system-x86_64"
qrun(){ timeout "${BOOT_SECS:-40}" "$QEMU" -M q35 -m 512M -cdrom "$OUT/carmix.iso" \
    -drive file="$DISK",format=raw,if=none,id=d0,cache=writethrough \
    -device virtio-blk-pci,drive=d0,disable-modern=on,disable-legacy=off \
    -serial stdio -display none -no-reboot -L "$X/usr/share/seabios" -L "$X/usr/share/qemu" 2>&1 || true; }
echo "===== BOOT 1 (fresh disk: PERSIST phase) ====="; qrun
if [ -z "${SINGLE_BOOT:-}" ]; then
  echo "===== COLD REBOOT: qemu exited, RAM gone, disk image persists on host ====="
  echo "===== BOOT 2 (same disk: REVIVE phase) ====="; qrun
  echo "===== TAMPER: host flips one byte in an on-disk object payload (block 2 +5) ====="
  printf '\xA5' | dd of="$DISK" bs=1 seek=$((2*4096+5)) count=1 conv=notrunc 2>/dev/null
  echo "===== BOOT 3 (host-tampered disk: detection) ====="; qrun
fi
rm -rf "$OUT"
