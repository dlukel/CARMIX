# CARMIX hardware bring-up: what it takes to run on real metal

This is Part A, an honest research map. Every gap below is tied to a concrete
assumption in the current code, not to generalities. Line numbers refer to the
tree as it stands. Files are named by repo-relative path.

CARMIX today boots under QEMU with the flags in kernel/build.sh:

```
-M q35 -m 512M -smp 2 -accel tcg,thread=multi -cdrom carmix.iso
-drive file=DISK,format=raw,if=none,id=d0,cache=writethrough
-device virtio-blk-pci,drive=d0,disable-modern=on,disable-legacy=off
-serial stdio -display none -no-reboot
```

Limine hands the kernel long mode, page tables, a GDT, a framebuffer, a memory
map, and the HHDM offset. Everything after that is CARMIX code. The rest of this
document walks the four places where "works in QEMU" and "works on a physical
box" diverge.

---

## H-R1 The virtio-to-real-hardware storage gap

The durable content-addressed store (S1 through S7, kernel/kernel.c from about
line 5333) sits on exactly one storage driver: a legacy virtio-blk driver at
kernel/kernel.c lines 110 to 236. That driver assumes a virtual machine in
several concrete ways.

**It assumes a virtio-blk device exists and has a specific PCI identity.** The
probe at lines 152 to 157 walks bus 0, device 0 to 31, function 0 only, and
matches vendor 0x1AF4 device 0x1001:

```
if((vd&0xFFFF)==0x1AF4 && ((vd>>16)&0xFFFF)==0x1001){ dev=d; break; }
```

0x1AF4 is the Red Hat / virtio vendor id. 0x1001 is the transitional (legacy)
virtio-blk device id. If the device is not found the store simply does not run
(line 158, then line 225 prints "virtio-blk NOT AVAILABLE"). No real physical
disk controller has this id, so on bare metal the probe fails and persistence is
skipped.

**It assumes the legacy virtio interface over a PCI I/O BAR.** Lines 161 to 163
require BAR0 to be an I/O-space BAR (`bar0 & 1`) and take the port base from
`bar0 & ~0x3`. All device access is then port I/O (outb/outw/outl/inb/inw/inl)
against the legacy register block defined at lines 128 to 134, including
`VIO_Q_PFN` at offset 0x08. The queue is published by writing a page frame
number, `outl(vio_io+VIO_Q_PFN, vq_phys>>12)` at line 184. This is the virtio
0.9.5 legacy layout. Modern virtio 1.0 devices expose capability-described MMIO
BARs and a different register set, and the modern-only virtio-blk id is 0x1042,
not 0x1001. The build forces the emulator to present the legacy face with
`disable-modern=on,disable-legacy=off` in kernel/build.sh. Nothing in the driver
speaks the modern interface.

**It assumes queue memory is physically contiguous from the boot bump
allocator.** Lines 179 to 180 call falloc repeatedly and bail if the frames are
not consecutive ("queue not contiguous"). This only holds because the driver
runs first, right after the memory map is parsed, while falloc is still a simple
upward bump (see H-R4).

**It assumes guest-physical equals the falloc address, with no IOMMU.** The
header comment at lines 115 to 116 states it plainly:

```
No MSI-X, no IOMMU (q35 default), so a descriptor address IS a guest-physical
(falloc) address.
```

Descriptor addresses handed to the device (lines 197 to 202) are raw falloc
physical addresses. On a machine with an active IOMMU (Intel VT-d or AMD-Vi),
the device would see I/O virtual addresses that the IOMMU translates, so those
raw addresses would point somewhere else unless the IOMMU is in passthrough or
is programmed with matching mappings.

**It assumes a polling model, no interrupts.** vblk_op (lines 192 to 217) kicks
the queue with `outw(vio_io+VIO_Q_NOTIFY,0)` and then spins on the used-ring
index (lines 209 to 213) up to two billion iterations. There is no virtio IRQ,
no MSI-X, and no completion interrupt handler. This is actually a portability
help, not a hindrance (see H-R4 on interrupts): the storage path needs no device
IRQ wiring at all.

### What real x86-64 hardware presents instead

A physical machine does not offer virtio. It offers one of:

- AHCI over SATA (older and mid-range boxes, and many desktop boards),
- NVMe over PCIe (essentially every modern laptop, workstation, and server),
- USB mass storage (xHCI plus USB BOT or UASP), which is the hardest of the
  three because it means bringing up xHCI first.

virtio only appears when the "real" box is itself a hypervisor host, that is, a
KVM or cloud instance whose host exposes virtio-blk to the guest.

### The shortest real paths, and the tradeoff

**Path (a): run on a box that itself exposes virtio.** A bare-metal KVM host or a
cloud instance configured with virtio-blk gives real CPU, real RAM, real SMP,
real firmware, and a real boot disk, while the storage controller stays virtio.
This exercises everything in H-R2, H-R3, and H-R4 (real memory map, real timing,
real APIC, real PCI) without writing a new disk driver. The one caveat: many
such hosts default to modern virtio 1.0 and present device id 0x1042, so the
current probe (which matches only 0x1001) and the legacy register path would
still need a transitional-device configuration on the host, or a small modern
front end. This is the fastest first rung.

**Path (b): write a driver for a real controller.** NVMe is the most tractable
modern target. It is a clean queue-based MMIO interface (admin and I/O
submission and completion queues, doorbell registers, a PRP or SGL data path)
that maps closely onto the queue discipline the virtio driver already uses.
AHCI is the alternative for older SATA machines and is messier (port registers,
command lists, FIS structures). USB storage is a distant third.

**Recommended sequence: (a) then (b).** Prove real boot and real content-addressed
storage on a virtio-exposing box first, then write NVMe. NVMe can be developed
against QEMU's own NVMe emulation with `-device nvme`, which implements the real
NVMe register and queue interface. That is an intermediate rung: closer to metal
than virtio because it is the actual NVMe programming model, but still
emulator-tested rather than physical-tested. The honest ordering is virtio on
real hardware, then NVMe against emulated NVMe, then NVMe on a physical drive.

---

## H-R2 The real console

CARMIX has two console outputs today.

**COM1 serial via port I/O.** kernel/kernel.c line 66 hardcodes `COM1 0x3F8`.
serial_init (lines 67 to 70) programs a 16550 UART (line control, divisor
latch, FIFO). Every diagnostic goes through sputc (lines 71 to 72), which polls
the line-status register bit 0x20 and writes the data port. This is the primary
console. The build boots headless with `-serial stdio -display none` in
kernel/build.sh, so in QEMU all output lands on stdout.

**A Limine framebuffer plus desktop code.** fb_req at line 44 requests a
framebuffer. At boot (lines 6468 to 6480) FB is taken from Limine and the kernel
draws directly into `FB->address` via put_px and get_px (lines 308 to 310),
including the literal "CARMIX ON HW" string at line 6475. The stages S2 through
E4 build a scrolling console, windows, a cursor, and a desktop on top of that
same framebuffer.

### Real options

- **A real 16550 UART.** If the box actually has a physical serial port at the
  standard I/O base 0x3F8, the existing serial code works unchanged. The catch
  is that many modern laptops and small-form-factor boxes have no physical
  serial port at all. Servers and industrial boards usually still do, sometimes
  only as an internal header.
- **USB-serial or a debug header.** Where there is no real UART, output needs a
  USB-serial adapter driven by the host on the other end plus a serial source on
  the box, or a vendor debug header. Either way the machine side still needs
  something that looks like a UART, otherwise serial output is a no-op and the
  0x3F8 writes go nowhere.
- **The Limine framebuffer as the graphical console.** Because CARMIX already
  renders text and a desktop into the Limine framebuffer, a box with a display
  gives a real on-screen console with no new driver. Limine fills in the
  framebuffer response only when the firmware provides a GOP or equivalent, so
  this depends on real UEFI graphics being present.

A headless real box needs a working serial path (real UART or a debug transport)
because there is no screen to read. A box with a display can rely on the Limine
framebuffer console, and serial becomes optional but is still the most reliable
early-boot log.

---

## H-R3 The target machine class and boot medium

"A real x86-64 machine" for the first target concretely means either a physical
box (a desktop, a laptop, a server, or a single-board x86 machine) or a
bare-metal cloud instance. The boot path is the same one the ISO already uses.

kernel/build.sh builds a hybrid Limine ISO with both a BIOS El Torito boot
record and a UEFI boot image (BOOTX64.EFI under EFI/BOOT, plus limine-bios-cd.bin
and limine-uefi-cd.bin), runs `limine bios-install`, and copies the result to
kernel/carmix.iso. The comment at that step says the ISO is
"real-hardware-bootable (USB) + qemu". So the boot medium is one of:

- a USB stick written from kernel/carmix.iso (the hybrid image boots on both
  BIOS and UEFI firmware that Limine supports), or
- the boot disk of a cloud or bare-metal instance, written with the same image.

The minimum the machine must present:

- **x86-64 long mode.** The kernel confirms this at boot (lines 6405 to 6408,
  reads EFER.LMA). Limine will not hand control in long mode otherwise.
- **Limine-supported firmware.** UEFI or legacy BIOS that Limine can boot from.
- **Real RAM described by the firmware memory map.** The kernel treats a missing
  HHDM as fatal (line 6410) and needs at least one usable region (see H-R4).
- **A console path.** A real UART at 0x3F8 or a framebuffer, per H-R2.
- **A storage path** if persistence is wanted, per H-R1. Without it the box
  still boots and runs everything in RAM, and only the durable stages are
  skipped.

---

## H-R4 What breaks off-emulator

These are the assumptions that only hold under QEMU, each tied to code.

### Memory map

CARMIX does honor the Limine memmap, but only barely. At lines 6414 to 6418 kmain
walks the memmap and keeps the single largest `LIMINE_MEMMAP_USABLE` entry:

```
if(e->type==LIMINE_MEMMAP_USABLE && e->length>bl){ bb=e->base; bl=e->length; }
free_base=(bb+0xfff)&~0xfffULL; free_top=bb+bl; ram_lo=free_base; ram_hi=free_top;
```

The good news: because it filters on USABLE, it never allocates into reserved,
ACPI, or MMIO holes, so it will not stomp firmware or device regions on real
hardware. The frame allocator (falloc, lines 83 to 87) and the frame database
(scoped to `[ram_lo, ram_hi)`, see lines 1317 and 4492) both live inside that one
region. The limitation: it uses only one contiguous region and ignores all the
others. On a real machine memory is fragmented by the sub-4GB PCI hole, ACPI
reclaimable ranges, EFI runtime regions, and multiple usable spans above and
below 4GB. QEMU with 512M presents a big clean low region, so one entry is
plenty there. On a real box with, say, several usable spans, CARMIX would run on
whichever single span is largest and leave the rest unused. That is capacity
loss, not corruption, but it is a QEMU-shaped choice.

### Timing

Two timing assumptions lean on the emulator.

- **rdtsc as the only clock.** All the measurement stages read rdtsc directly
  (definition at line 1062, used throughout). There is no calibration against a
  wall clock. The code itself acknowledges emulator timing noise: line 1814
  prints "(HW not cheaper this run - rdtsc/TCG noise)". On real hardware rdtsc is
  a real invariant-TSC counter, so the measured cycle numbers become physically
  meaningful, but any comparison tuned to TCG behavior may read differently.
- **Busy-wait loops sized for emulator speed.** The virtio poll spins up to two
  billion iterations as a stand-in timeout (line 212). The preemption test busy-
  loops a fixed 4,000,000 iterations and assumes that is "tens of ms" of work so
  the 100Hz timer preempts it (lines 989 and 994). Both are iteration counts,
  not real time, so on faster or slower real silicon they represent a different
  wall-clock duration. Nothing breaks correctness, but the timeout and the
  preemption window are not calibrated to real time.

### Interrupts

The interrupt setup is deliberately minimal and QEMU-friendly.

- **Legacy 8259 PIC plus PIT for the timer.** pic_remap (lines 287 to 291)
  remaps the two 8259 PICs and unmasks only IRQ0. pit_init (line 292) programs
  the 8253/8254 PIT to 100Hz. The timer ISR EOIs the master PIC with
  `outb(0x20,0x20)` (line 285). This assumes a working legacy PIC and PIT. QEMU
  q35 emulates both. Real modern hardware may present them only in a legacy
  compatibility mode through the chipset LPC, and on some machines the PIT is
  gone or unreliable, with the expectation that the OS uses the LAPIC timer or
  HPET instead. There is no LAPIC-timer or HPET path in the code.
- **LAPIC used only for IPI.** The SMP code (lines 5835 to 5856) maps the LAPIC
  at the architectural default physical base 0xFEE00000 (line 5836, hardcoded,
  not read from the IA32_APIC_BASE field), enables xAPIC, and sends inter-
  processor interrupts. That is the only LAPIC use. It works on real hardware as
  long as the LAPIC base is at the default, which it almost always is.
- **No IOAPIC at all.** There is no IOAPIC code anywhere in kernel/kernel.c. On a
  real APIC-based system, device IRQs are routed through one or more IOAPICs, and
  the routing is described by the ACPI MADT, which CARMIX does not parse. So real
  device interrupts are simply not wired.
- **What is missing for real device interrupts, and why it mostly does not
  matter yet.** Because the storage path is polled (H-R1) and the keyboard is
  polled (line 290 says "keyboard is POLLED"), the only interrupt CARMIX actually
  depends on is the periodic timer. So the missing IOAPIC and MSI/MSI-X support
  do not block bring-up of the current feature set. The one real dependency is
  getting a periodic tick on real hardware, which today assumes the legacy PIC
  plus PIT deliver IRQ0. If a target box lacks a usable PIT or does not route
  IRQ0 through the 8259 in the way QEMU does, the tick, and therefore preemption,
  would need an LAPIC-timer path that does not exist yet.

### Device enumeration

The PCI access is the legacy configuration mechanism, not ECAM. pci_rd and pci_wr
(lines 120 to 127) use the 0xCF8 address port and 0xCFC data port defined at
lines 118 and 119. The only scan (lines 154 to 157) is limited to bus 0, devices
0 to 31, function 0 only. There is no recursion across PCI-to-PCI bridges, no
multifunction probing, and no use of the PCIe extended configuration space (ECAM,
the memory-mapped config region whose base comes from the ACPI MCFG table).

For the current single virtio-blk device on QEMU q35 that is enough, because the
device sits on bus 0 at function 0. Real hardware routinely places devices behind
bridges on non-zero buses, uses multiple functions, and exposes extended
capability registers that only ECAM can reach. A real controller driver (the
NVMe or AHCI work in H-R1) would in practice need a fuller enumeration: at least
a multi-bus, multifunction scan through 0xCF8/0xCFC, and ideally ECAM via the
MCFG table for extended capabilities and MSI/MSI-X setup.

---

## Part B status: NVMe rung built (QEMU-NVMe, real interface), physical boot still blocked

Part B is the first concrete rung: CARMIX booting via Limine on a real x86-64 machine
with real content-addressed storage. The honest observed result this cycle is that the
concrete metal step was NOT executed, for one real reason: this build environment has no
physical x86-64 machine and no bare-metal cloud instance attached to it. The kernel is
built and booted in QEMU only. Claiming a real-hardware boot or real-disk storage that
did not happen would be the exact dodge D1 and D3 forbid, so it is not claimed.

What is genuinely true and carries toward metal:

- The boot artifact is real-media-ready. build.sh produces a hybrid BIOS plus UEFI Limine
  ISO (the same one QEMU boots) that can be written to a USB stick. Booting it on a real
  box is a matter of running it on hardware, which this environment cannot do.
- CARMIX already honors the Limine memory map for usable RAM and checks long mode and the
  HHDM before proceeding (H-R4), so the first-boot memory assumptions are not QEMU-clean-map
  assumptions. The known weakness (using only the single largest usable region) is named in
  H-R4 and is the first hardening a real box with fragmented RAM would require.
- The only hard real-IRQ dependency is a periodic tick (the storage and keyboard paths are
  polled), which narrows what a first real boot must get right on interrupts (H-R4).

The code rung per H-R1, an NVMe driver, is now BUILT (kernel/nvmetest.c, kernel/nvme_build.sh).
NVMe is the most tractable real storage controller (a clean queue-based MMIO interface). The
driver does the real controller bring-up: it finds the NVMe controller by PCI class (01h 08h),
maps BAR0 uncached, resets the controller (CC.EN=0, waits CSTS.RDY=0), sets up the admin
submission and completion queues (AQA, ASQ, ACQ), enables the controller (CC.EN=1, waits
CSTS.RDY=1), creates an I/O queue pair with the admin Create-I/O-CQ and Create-I/O-SQ commands,
and does I/O reads and writes through the I/O submission queue with PRP1 data pointers and
completion-queue phase-bit polling and doorbell rings. This is the real NVMe register and queue
interface, not virtio.

It is tested against QEMU's own NVMe device emulation (-device nvme), which implements the NVMe
spec, so it exercises the real interface. It is EMULATOR-tested, NOT physical-NVMe-tested.
Running it on a physical NVMe SSD is the next step and needs physical hardware this environment
does not have. That distinction is stated everywhere the rung is reported.

Observed this run (QEMU-NVMe):

```
HW-B NVMe found bus=0 dev=3 fn=0 BAR0=0xfebd4000   CAP DSTRD=0 MQES=2048
HW-B controller ENABLED (CSTS.RDY=1); admin queues live; I/O queue pair created (qid 1)
HW-B1 NVMe init OK: 199473658 cyc
HW-B2 object 512B content address BLAKE3=45e04ceedb6460dc..
HW-B2 wrote payload@LBA1 status=0 header@LBA0 status=0; write 975345 cyc
HW-B2 read header status=0 payload status=0; read 621766 cyc
HW-B2 re-hash of the read-back payload == the stored content address = y
HW-B2 tamper: flip 1 payload byte -> re-hash MISMATCH detected = y
-> RUNG (b) DONE: a real NVMe controller driver does content-addressed I/O, re-verified from the device (QEMU-NVMe; physical-NVMe untested)
```

A content-addressed object (512 bytes, named by its BLAKE3 hash) is written to the NVMe device
(payload LBA plus a header LBA that binds the length and the hash, the torn-tail record shape),
read back, and re-verified by re-hashing the read-back bytes to the stored content address. A
single flipped byte in the read-back payload fails the re-hash, so the device is treated as
untrusted media and every load is re-verified. This is the architecture's content store proving
itself over a real storage-controller interface. All numbers are rdtsc-measured this run and
labeled QEMU-NVMe, not physical-hardware.

What remains blocked is the PHYSICAL step: an actual metal boot and an actual physical NVMe SSD.
This environment has no physical x86-64 machine and no bare-metal instance, so no real-hardware
boot or real-disk write is claimed. The boot artifact is real-media-ready (a hybrid BIOS plus
UEFI Limine ISO), and the storage driver now speaks the real NVMe interface, so the two pieces
that need physical hardware are the boot itself and a physical NVMe device.

This is the beginning of a long path, stated as the beginning. It does not claim a hardware OS,
a broad driver suite, or production readiness. kernel.c and the proven core are untouched, and
the QEMU regression remains green.

## Cycle 1: ACPI topology, IOAPIC routing, LAPIC timer, device authority (QEMU real ACPI/APIC)

Built as kernel/hwtest.c (kernel.c and the proven core untouched; swcap linked only), booted on
one QEMU q35 which provides real ACPI, a real IOAPIC, and a real LAPIC. Every rung ran against
those real interfaces, which is genuine progress toward metal. Physical boot on a real machine
is a separate gap (no physical hardware here), stated honestly below.

- HW-1 ACPI MADT parse: the RSDP (revision 0, so the 32-bit RSDT) was walked to the MADT and the
  real interrupt topology read from the machine's tables, not hardcoded: 1 Local APIC, LAPIC base
  0xfee00000, IOAPIC at 0xfec00000, 5 interrupt source overrides.
- HW-2 IOAPIC/LAPIC routing: an IOAPIC redirection entry (GSI 2 to LAPIC vector 0x41) was
  programmed, the legacy PIC masked, and the PIT driven so its IRQ routes through the IOAPIC. 5
  real device interrupts were delivered IOAPIC to LAPIC, handled, and acknowledged with EOI.
- HW-3 LAPIC timer: calibrated against the PIT (a real 10 ms one-shot), measured about 63,145,600
  LAPIC-timer ticks per second, set periodic at 100 Hz, and observed 20 real periodic ticks. This
  is the real LAPIC timer, not a PIT relabeled.
- HW-4 physical boot: NOT run on metal, no physical x86-64 machine in this environment. The
  ACPI/IOAPIC/timer rungs ran against QEMU's real ACPI/APIC. Physical boot and a physical NVMe SSD
  await real hardware. No physical boot is claimed. The boot artifact is real-media-ready.
- HW-5 device authority under the ceiling (the CARMIX-specific rung): a driver holds a bounded
  device capability over exactly its MMIO region (base 0xfec00000, length 4096) via the proven
  swcap gate. In-region access is allowed, an out-of-region access is refused, and an over-length
  access is refused. Device access is capability-bounded, anti-amplification at the driver level,
  no ambient MMIO. This is what makes CARMIX on metal structurally different from a conventional
  kernel.
- HW-6 measurement (rdtsc, QEMU this run): ACPI MADT parse about 292M cyc (includes the on-demand
  ACPI page mappings), LAPIC-timer bring-up plus calibration about 892M cyc (includes the 10 ms
  calibration wall time), calibrated rate 63,145,600 ticks/sec.

The QEMU regression is unaffected (kernel.c and the proven core are byte-identical). The remaining
gap to metal is the physical boot itself and a physical device, which need real hardware.
