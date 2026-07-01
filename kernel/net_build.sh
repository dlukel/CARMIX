#!/usr/bin/env bash
# CARMIX Track B - build nettest.elf, make a bootable ISO, launch TWO QEMU
# instances sharing one ivshmem file, capture each instance's serial separately.
# kernel.c and the proven modules are NOT touched (store/ + sls/ are LINKED only).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
LIM="${LIMINE_DIR:?set LIMINE_DIR to a Limine 9.x directory (limine tool, headers, boot blobs)}"
X="${X86TOOLS:?set X86TOOLS to a prefix holding qemu-system-x86_64 and xorriso under usr/bin}"
CL="${CVSASX_CLANG:?set CVSASX_CLANG to a freestanding x86-64 clang}"
LD="${CVSASX_LLD:?set CVSASX_LLD to ld.lld}"
NM="${CVSASX_NM:?set CVSASX_NM to llvm-nm}"
ROOT="$(dirname "$HERE")"; ST="$ROOT/store"; SL="$ROOT/sls"; CAP="$ROOT/cap"; CX="$ROOT/carmix"
OUT="$(mktemp -d)"
export LD_LIBRARY_PATH="$X/usr/lib/x86_64-linux-gnu:$X/lib/x86_64-linux-gnu${QEMU_LIBS:+:$QEMU_LIBS}"

KF="--target=x86_64-unknown-none-elf -ffreestanding -nostdlib -fno-stack-protector -fno-pic -fno-pie \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mgeneral-regs-only -mcmodel=kernel -O2 -Wall -Wextra \
    -Wno-sign-compare \
    -I $LIM -I $HERE -I $ST -I $SL -I $CAP -I $CX -I $ST/blake3 -I $ST/freestanding"
B3="-DNDEBUG -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512 -DBLAKE3_USE_NEON=0 -fno-builtin"

echo "=== build nettest + spine (store/sls, LINKED) + swcap anti-amp gate + Ed25519 (vendored) ==="
# NETTEST_DEFS=-DTAMPER_TEST / -DNODE_TAMPER_TEST run the by-construction tamper demos.
$CL $KF ${NETTEST_DEFS:-} -c "$HERE/nettest.c"  -o "$OUT/nettest.o"
$CL $KF     -c "$ST/store_mem.c"               -o "$OUT/store_mem.o"   # mem*/strlen (freestanding)
$CL $KF $B3 -c "$ST/object_store.c"            -o "$OUT/object_store.o"
$CL $KF $B3 -c "$ST/blake3_wrap.c"             -o "$OUT/blake3_wrap.o"
$CL $KF $B3 -c "$ST/blake3/blake3.c"           -o "$OUT/blake3.o"
$CL $KF $B3 -c "$ST/blake3/blake3_dispatch.c"  -o "$OUT/blake3_dispatch.o"
$CL $KF $B3 -c "$ST/blake3/blake3_portable.c"  -o "$OUT/blake3_portable.o"
$CL $KF     -c "$SL/sls.c"                      -o "$OUT/sls.o"
$CL $KF     -c "$CX/swcap.c"                    -o "$OUT/swcap.o"      # anti-amp re-mint gate (LINKED, unmodified)
$CL $KF -Wno-unused-parameter -c "$HERE/tweetnacl.c" -o "$OUT/tweetnacl.o"  # vendored ref Ed25519 (freestanding)
"$LD" -m elf_x86_64 -static -T "$HERE/linker.ld" --build-id=none -z noexecstack -o "$OUT/nettest.elf" "$OUT"/*.o
echo "link OK: nettest.elf = $(stat -c%s "$OUT/nettest.elf") bytes"
echo "undefined symbols : $("$NM" "$OUT/nettest.elf" 2>/dev/null | grep -c ' U ' || true)  (expect 0)"

echo "=== bootable ISO (Limine BIOS+UEFI) ==="
ISO=$OUT/iso; mkdir -p "$ISO/boot/limine" "$ISO/EFI/BOOT"
cp "$OUT/nettest.elf" "$ISO/boot/kernel.elf"
cp "$LIM/limine-bios.sys" "$LIM/limine-bios-cd.bin" "$LIM/limine-uefi-cd.bin" "$ISO/boot/limine/"
cp "$LIM/BOOTX64.EFI" "$ISO/EFI/BOOT/"
cat > "$ISO/boot/limine/limine.conf" <<'EOF'
timeout: 0
serial: yes

/CARMIX-NETTEST
    protocol: limine
    path: boot():/boot/kernel.elf
EOF
( cd "$OUT" && "$X/usr/bin/xorriso" -as mkisofs -R -r -J \
    -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table \
    --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
    --protective-msdos-label iso -o nettest.iso ) 2>&1 | tail -1
"$LIM/limine" bios-install "$OUT/nettest.iso" 2>&1 | tail -1
cp "$OUT/nettest.iso" "$HERE/nettest.iso"

# ---- the two-machine harness: one ivshmem file shared by BOTH instances -----
SHMFILE=/dev/shm/carmix_ivshmem
rm -f "$SHMFILE"
IV="-object memory-backend-file,id=hm,size=4M,mem-path=$SHMFILE,share=on -device ivshmem-plain,memdev=hm"
QEMU="$X/usr/bin/qemu-system-x86_64"
QFLAGS="-M q35 -m 512M -cdrom $OUT/nettest.iso -display none -no-reboot -L $X/usr/share/seabios -L $X/usr/share/qemu"

echo "=== B0: launching TWO QEMU instances sharing $SHMFILE ==="
echo "    instance A (source) first, so it claims the role-magic; B follows."
LOGA="$HERE/serial_A.log"; LOGB="$HERE/serial_B.log"
rm -f "$LOGA" "$LOGB"

# A first (claims magic), give it a moment to zero+claim, then B.
timeout 90 "$QEMU" $QFLAGS $IV -serial "file:$LOGA" >/dev/null 2>&1 &
PA=$!
sleep 2
timeout 90 "$QEMU" $QFLAGS $IV -serial "file:$LOGB" >/dev/null 2>&1 &
PB=$!

# host-level proof both attach the same file:
echo "    /dev/shm contents while both run:"
ls -la "$SHMFILE" 2>&1 | sed 's/^/      /'
echo "    open file descriptors pointing at the ivshmem file (host proof of sharing):"
for pid in $PA $PB; do
  for fd in /proc/$pid/fd/*; do
    tgt=$(readlink "$fd" 2>/dev/null || true)
    [ "$tgt" = "$SHMFILE" ] && echo "      pid $pid -> $tgt"
  done 2>/dev/null
done

wait $PA $PB 2>/dev/null || true
echo ""
echo "================= SERIAL A (source) ================="
cat "$LOGA" 2>/dev/null || echo "(no A log)"
echo "================= SERIAL B (destination) ============"
cat "$LOGB" 2>/dev/null || echo "(no B log)"
rm -rf "$OUT"
