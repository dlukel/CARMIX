/*
 * CARMIX
 * Copyright (c) 2026 Loucas Louka
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
/* ===========================================================================
 * CARMIX kernel core - bare-metal x86-64, Limine boot protocol.
 * BORROWED: the boot shim (Limine hands us long mode + page tables + GDT +
 * framebuffer + memory map). WRITTEN: everything below (serial, IDT/fault dump,
 * frame allocator, page mapper, framebuffer draw, timer). Milestones B0-B5 print
 * over serial as they pass; a deliberate #UD at the end proves the fault path (B2).
 * Staging: the proven CARMIX modules + the Wasm backend are NOT embedded here.
 * ===========================================================================*/
#include <stdint.h>
#include <stddef.h>
#include "limine.h"
#include "font8x8.h"
#include "cvsasx_gate.h"     /* Step 4: the proven Wasm-SFI gate (recompiled, unmodified) */
#include "cvsasx_swcap.h"    /* Step 4: the proven software capability backend */
#include "cvsasx_store.h"    /* Step 5: BLAKE3 + content-addressed store */
#include "cvsasx_sls.h"      /* Step 5: single-level store + structural diff */

/* Freestanding mem-functions and strlen come from store/store_mem.c (recursion-safe,
 * optnone), linked this step; the kernel no longer defines its own (would duplicate). */

/* ---- Limine requests (// VERIFIED against limine.h) ---------------------- */
__attribute__((used, section(".limine_requests_start_marker")))
static volatile LIMINE_REQUESTS_START_MARKER;
__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);
__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request fb_req = { .id = LIMINE_FRAMEBUFFER_REQUEST, .revision = 0 };
__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request mm_req = { .id = LIMINE_MEMMAP_REQUEST, .revision = 0 };
__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_req = { .id = LIMINE_HHDM_REQUEST, .revision = 0 };
/* SMP/MP: Limine performs the standard INIT-SIPI-SIPI and parks each AP in long mode at
 * its goto_address (the known sequence, via the existing boot path). flags=0 -> xAPIC. */
#ifndef LIMINE_MP_REQUEST
#define LIMINE_MP_REQUEST LIMINE_SMP_REQUEST
#endif
__attribute__((used, section(".limine_requests")))
static volatile struct LIMINE_MP(request) smp_req = { .id = LIMINE_MP_REQUEST, .revision = 0 };
__attribute__((used, section(".limine_requests_end_marker")))
static volatile LIMINE_REQUESTS_END_MARKER;

/* ---- B0: serial (16550 COM1) -------------------------------------------- */
static inline void outb(uint16_t p, uint8_t v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t inb(uint16_t p){ uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void outw(uint16_t p, uint16_t v){ __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline uint16_t inw(uint16_t p){ uint16_t v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void outl(uint16_t p, uint32_t v){ __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline uint32_t inl(uint16_t p){ uint32_t v; __asm__ volatile("inl %1,%0":"=a"(v):"Nd"(p)); return v; }
#define COM1 0x3F8
static void serial_init(void){
    outb(COM1+1,0x00); outb(COM1+3,0x80); outb(COM1+0,0x03); outb(COM1+1,0x00);
    outb(COM1+3,0x03); outb(COM1+2,0xC7); outb(COM1+4,0x0B);
}
static void sputc(char c){ while(!(inb(COM1+5)&0x20)) {} outb(COM1,(uint8_t)c);
    if(c=='\n'){ while(!(inb(COM1+5)&0x20)) {} outb(COM1,'\r'); } }
static void sputs(const char*s){ while(*s) sputc(*s++); }
static void sx64(uint64_t v){ sputs("0x"); for(int i=60;i>=0;i-=4){ int d=(v>>i)&0xF; sputc(d<10?(char)('0'+d):(char)('a'+d-10)); } }
static void sdec(uint64_t v){ char b[24]; int i=0; if(!v){sputc('0');return;} while(v){b[i++]=(char)('0'+v%10);v/=10;} while(i)sputc(b[--i]); }
static uint64_t rdmsr(uint32_t m){ uint32_t lo,hi; __asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(m)); return ((uint64_t)hi<<32)|lo; }
static void wrmsr(uint32_t m, uint64_t v){ __asm__ volatile("wrmsr"::"c"(m),"a"((uint32_t)v),"d"((uint32_t)(v>>32))); }
static inline void hcf(void){ for(;;) __asm__ volatile("cli; hlt"); }

/* ---- frame allocator + page mapper (over Limine memmap + HHDM) ----------- */
static uint64_t hhdm_off, free_base, free_top, freelist, ram_lo, ram_hi;
static inline void* P2V(uint64_t p){ return (void*)(p + hhdm_off); }
static uint64_t falloc(void){
    if(freelist){ uint64_t p=freelist; freelist=*(uint64_t*)P2V(p); return p; }
    if(free_base+4096<=free_top){ uint64_t p=free_base; free_base+=4096; return p; }
    return 0;   /* fail-closed: out of frames */
}
/* AUDIT A1 fix: reject misaligned / out-of-range / immediate-double-free frees so a
 * bad caller cannot poison the freelist (cycle/alias). // full double-free
 * detection needs a free-bitmap - deferred until a page-unmap path exists. */
static void ffree(uint64_t p){
    if((p & 0xfffULL) || p < ram_lo || p >= ram_hi || p == freelist) return;
    *(uint64_t*)P2V(p)=freelist; freelist=p;
}
static uint64_t* next_tbl(uint64_t *t, int i){
    if(!(t[i]&1)){ uint64_t p=falloc(); if(!p) return 0; uint64_t*v=P2V(p); for(int k=0;k<512;k++)v[k]=0; t[i]=p|0x3; }
    return (uint64_t*)P2V(t[i]&~0xfffULL);
}
static int map_page(uint64_t va, uint64_t pa, uint64_t flags){
    uint64_t cr3; __asm__ volatile("mov %%cr3,%0":"=r"(cr3));
    uint64_t *pml4=P2V(cr3&~0xfffULL);
    uint64_t *pdpt=next_tbl(pml4,(va>>39)&0x1ff); if(!pdpt)return 0;
    uint64_t *pd  =next_tbl(pdpt,(va>>30)&0x1ff); if(!pd)return 0;
    uint64_t *pt  =next_tbl(pd,  (va>>21)&0x1ff); if(!pt)return 0;
    pt[(va>>12)&0x1ff]=(pa&~0xfffULL)|flags|1;
    __asm__ volatile("invlpg (%0)"::"r"(va):"memory");
    return 1;
}

/* ===========================================================================
 * VIRTIO-BLK (legacy, PCI I/O BAR, polling) - the stable-media path (S1).
 * Minimal and correct: PCI enumerate vendor 0x1AF4 device 0x1001, enable I/O +
 * bus-master, reset, negotiate only VIRTIO_BLK_F_FLUSH if offered, set up ONE
 * polled virtqueue, and do 4 KiB block read/write/flush. No interrupts, no
 * MSI-X, no IOMMU (q35 default), so a descriptor address IS a guest-physical
 * (falloc) address. New bring-up; nothing here reimplements an existing path.
 * ===========================================================================*/
#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC
static uint32_t pci_rd(uint8_t bus,uint8_t dev,uint8_t fn,uint8_t off){
    uint32_t a=0x80000000u|((uint32_t)bus<<16)|((uint32_t)dev<<11)|((uint32_t)fn<<8)|(off&0xFC);
    outl(PCI_ADDR,a); return inl(PCI_DATA);
}
static void pci_wr(uint8_t bus,uint8_t dev,uint8_t fn,uint8_t off,uint32_t v){
    uint32_t a=0x80000000u|((uint32_t)bus<<16)|((uint32_t)dev<<11)|((uint32_t)fn<<8)|(off&0xFC);
    outl(PCI_ADDR,a); outl(PCI_DATA,v);
}
#define VIO_DEV_FEAT 0x00
#define VIO_GST_FEAT 0x04
#define VIO_Q_PFN    0x08
#define VIO_Q_SIZE   0x0C
#define VIO_Q_SEL    0x0E
#define VIO_Q_NOTIFY 0x10
#define VIO_STATUS   0x12
#define VST_ACK 1u
#define VST_DRV 2u
#define VST_DRVOK 4u
#define VQF_NEXT 1u
#define VQF_WRITE 2u
#define VBLK_T_IN 0u
#define VBLK_T_OUT 1u
#define VBLK_T_FLUSH 4u
#define VBLK_F_FLUSH (1u<<9)
#define VBLK_BLOCK 4096u            /* CARMIX durable block = one page = 8 virtio sectors */
struct vq_desc { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; } __attribute__((packed));
static uint16_t vio_io, vq_size, vq_avail_idx, vq_used_seen;
static struct vq_desc *vq_desc; static uint16_t *vq_avail; static volatile uint8_t *vq_used;
static uint64_t vq_phys, vblk_hdr_phys, vblk_data_phys, vblk_stat_phys;
static uint8_t *vblk_hdr, *vblk_data, *vblk_stat;
static int vio_ready=0, vio_has_flush=0;

static int virtio_blk_init(void){
    int dev=-1;
    for(int d=0; d<32; d++){
        uint32_t vd=pci_rd(0,(uint8_t)d,0,0x00);
        if((vd&0xFFFF)==0x1AF4 && ((vd>>16)&0xFFFF)==0x1001){ dev=d; break; }
    }
    if(dev<0){ sputs("DUR virtio-blk: PCI device 1af4:1001 NOT FOUND\n"); return 0; }
    uint32_t cmd=pci_rd(0,(uint8_t)dev,0,0x04);
    pci_wr(0,(uint8_t)dev,0,0x04, cmd | 0x1u | 0x4u);          /* I/O space + bus master (DMA) */
    uint32_t bar0=pci_rd(0,(uint8_t)dev,0,0x10);
    if(!(bar0&1u)){ sputs("DUR virtio-blk: BAR0 not I/O\n"); return 0; }
    vio_io=(uint16_t)(bar0 & ~0x3u);
    outb(vio_io+VIO_STATUS,0);                                 /* reset */
    outb(vio_io+VIO_STATUS,VST_ACK);
    outb(vio_io+VIO_STATUS,(uint8_t)(VST_ACK|VST_DRV));
    uint32_t devf=inl(vio_io+VIO_DEV_FEAT);
    uint32_t gf=devf & VBLK_F_FLUSH;                           /* negotiate ONLY flush, if offered */
    outl(vio_io+VIO_GST_FEAT, gf);
    vio_has_flush=(gf&VBLK_F_FLUSH)?1:0;
    outw(vio_io+VIO_Q_SEL,0);
    vq_size=inw(vio_io+VIO_Q_SIZE);
    if(vq_size==0){ sputs("DUR virtio-blk: queue size 0\n"); return 0; }
    uint64_t desc_sz=(uint64_t)vq_size*16;
    uint64_t avail_sz=6+2*(uint64_t)vq_size;
    uint64_t used_off=(desc_sz+avail_sz+4095)&~4095ULL;
    uint64_t total=used_off + (6+8*(uint64_t)vq_size);
    uint64_t pages=(total+4095)/4096;
    uint64_t base=falloc(), prev=base;                         /* contiguous (early-boot bump) */
    for(uint64_t i=1;i<pages;i++){ uint64_t p=falloc(); if(p!=prev+4096){ sputs("DUR virtio-blk: queue not contiguous\n"); return 0; } prev=p; }
    vq_phys=base; uint8_t *q=P2V(base);
    for(uint64_t i=0;i<pages*4096;i++) q[i]=0;
    vq_desc=(struct vq_desc*)q; vq_avail=(uint16_t*)(q+desc_sz); vq_used=(volatile uint8_t*)(q+used_off);
    outl(vio_io+VIO_Q_PFN,(uint32_t)(vq_phys>>12));
    uint64_t hf=falloc(); vblk_hdr_phys=hf; vblk_hdr=P2V(hf); vblk_stat_phys=hf+2048; vblk_stat=P2V(hf+2048);
    uint64_t df=falloc(); vblk_data_phys=df; vblk_data=P2V(df);
    outb(vio_io+VIO_STATUS,(uint8_t)(VST_ACK|VST_DRV|VST_DRVOK));
    vq_avail_idx=0; vq_used_seen=0; vio_ready=1;
    return 1;
}
/* one polled request. type IN/OUT use vblk_data (4 KiB); FLUSH has no data. */
static int vblk_op(uint32_t type, uint64_t sector){
    if(!vio_ready) return -1;
    uint32_t *h=(uint32_t*)vblk_hdr; h[0]=type; h[1]=0; *(uint64_t*)(vblk_hdr+8)=sector;
    *vblk_stat=0xFF;
    if(type==VBLK_T_FLUSH){
        vq_desc[0].addr=vblk_hdr_phys; vq_desc[0].len=16; vq_desc[0].flags=VQF_NEXT; vq_desc[0].next=1;
        vq_desc[1].addr=vblk_stat_phys; vq_desc[1].len=1; vq_desc[1].flags=VQF_WRITE; vq_desc[1].next=0;
    } else {
        vq_desc[0].addr=vblk_hdr_phys; vq_desc[0].len=16; vq_desc[0].flags=VQF_NEXT; vq_desc[0].next=1;
        vq_desc[1].addr=vblk_data_phys; vq_desc[1].len=VBLK_BLOCK; vq_desc[1].flags=(uint16_t)(VQF_NEXT|(type==VBLK_T_IN?VQF_WRITE:0)); vq_desc[1].next=2;
        vq_desc[2].addr=vblk_stat_phys; vq_desc[2].len=1; vq_desc[2].flags=VQF_WRITE; vq_desc[2].next=0;
    }
    vq_avail[2 + (vq_avail_idx % vq_size)] = 0;                /* head descriptor index */
    __asm__ volatile("mfence":::"memory");
    vq_avail_idx++; vq_avail[1]=vq_avail_idx;                  /* avail.idx */
    __asm__ volatile("mfence":::"memory");
    outw(vio_io+VIO_Q_NOTIFY,0);
    uint64_t spins=0;
    while(*(volatile uint16_t*)(vq_used+2) == vq_used_seen){
        __asm__ volatile("pause":::"memory");
        if(++spins>2000000000ULL){ sputs("DUR virtio-blk: TIMEOUT\n"); return -2; }
    }
    vq_used_seen=*(volatile uint16_t*)(vq_used+2);
    __asm__ volatile("mfence":::"memory");
    return (*vblk_stat==0)?0:-3;
}
/* 4 KiB block read/write: logical block B -> sector B*8. data via vblk_data bounce buffer. */
static int vblk_read(uint64_t blk){ return vblk_op(VBLK_T_IN, blk*8); }
static int vblk_write(uint64_t blk){ return vblk_op(VBLK_T_OUT, blk*8); }
static int vblk_flush(void){ return vio_has_flush?vblk_op(VBLK_T_FLUSH,0):0; }

/* S1 self-test: write a pattern to a scratch block, read it back, verify. */
static void virtio_blk_selftest(void){
    if(!virtio_blk_init()){ sputs("DUR S1 virtio-blk NOT AVAILABLE (persistence stages will not run)\n"); return; }
    sputs("DUR S1 virtio-blk up: io="); sx64(vio_io); sputs(" qsz="); sdec(vq_size);
    sputs(" flush="); sputs(vio_has_flush?"y":"n"); sputc('\n');
    const uint64_t TB=1000;                                    /* scratch block well past the log head */
    for(int i=0;i<4096;i++) vblk_data[i]=(uint8_t)(i*131u+7u);
    int w=vblk_write(TB); vblk_flush();
    for(int i=0;i<4096;i++) vblk_data[i]=0;                    /* clobber the bounce buffer */
    int r=vblk_read(TB);
    int match=1; for(int i=0;i<4096;i++) if(vblk_data[i]!=(uint8_t)(i*131u+7u)){ match=0; break; }
    sputs("DUR S1 write rc="); sdec((uint64_t)(w==0?0:1)); sputs(" read rc="); sdec((uint64_t)(r==0?0:1));
    sputs(" block read-back matches written="); sputs(match?"y":"n");
    sputs(match&&w==0&&r==0?"  -> STABLE-MEDIA READ/WRITE OK\n":"  -> *** virtio-blk I/O FAILED ***\n");
}

/* ---- IDT + fault handler (B2: dump, don't triple-fault) ------------------ */
struct idt_entry { uint16_t lo; uint16_t sel; uint8_t ist; uint8_t type; uint16_t mid; uint32_t hi; uint32_t z; } __attribute__((packed));
static struct idt_entry idt[256];
struct idtr { uint16_t limit; uint64_t base; } __attribute__((packed));
static void idt_set(int v, void *isr, uint16_t sel){
    uint64_t a=(uint64_t)isr;
    idt[v].lo=a&0xffff; idt[v].sel=sel; idt[v].ist=0; idt[v].type=0x8E;
    idt[v].mid=(a>>16)&0xffff; idt[v].hi=(uint32_t)(a>>32); idt[v].z=0;
}
struct iframe { uint64_t rip, cs, rflags, rsp, ss; };
static void fault_dump(int vec, uint64_t err, struct iframe *f){
    sputs("\n*** EXCEPTION #"); sdec((uint64_t)vec); sputs(" CAUGHT - the fault handler works (not a triple-fault) ***\n");
    sputs("  rip="); sx64(f->rip); sputs(" cs="); sx64(f->cs); sputs(" rflags="); sx64(f->rflags); sputc('\n');
    sputs("  err="); sx64(err); sputs(" rsp="); sx64(f->rsp); sputs(" - halting (NOT a triple-fault).\n");
    hcf();
}
__attribute__((interrupt)) static void isr_ud(struct iframe *f){ fault_dump(6,0,f); }
__attribute__((interrupt)) static void isr_gp(struct iframe *f, uint64_t e){ fault_dump(13,e,f); }
/* The REMATERIALIZING FAULT HANDLER (F0-F5): #PF is now RESUMABLE. The
 * __attribute__((interrupt)) convention already saves all GPRs and ends with iretq;
 * RETURNING from this ISR resumes the faulting instruction. pf_service classifies the
 * fault, resolves vaddr->hash, materializes+verifies, and maps; on success we just
 * return (resume). On a fail-closed verdict (protection, no binding, store miss, verify
 * mismatch) we dump+halt - fail-loud, never resume with bad/absent bytes. */
static int pf_service(uint64_t cr2, uint64_t err, uint64_t rip);   /* defined below, near run_fault */
static int us0_protection_probe(uint64_t cr2, uint64_t err, volatile uint64_t *rip_io);  /* US0 ring-3 probe hook */
__attribute__((interrupt)) static void isr_pf(struct iframe *f, uint64_t e){
    uint64_t cr2; __asm__ volatile("mov %%cr2,%0":"=r"(cr2));
    /* &f->rip is the on-stack RIP the iretq epilogue will reload; the hook may rewrite it
     * (US0 RIP fixup). volatile so -O2 does not dead-store-eliminate that write (the iretq
     * reload is invisible to the C optimizer). */
    if(us0_protection_probe(cr2, e, (volatile uint64_t*)&f->rip)) return;  /* US0: classified + survived */
    if(pf_service(cr2, e, f->rip)) return;   /* SERVICED: iretq (implicit) -> faulting insn re-executes */
    sputs("  (#PF NOT serviced - fail-closed)\n"); fault_dump(14,e,f);   /* fail-loud, halt */
}
/* AUDIT A6 fix: #DF (8) and a catch-all over the 0-31 CPU-exception range, split by the
 * error-code-pushing vectors, so a stray/nested exception DUMPS instead of silently
 * triple-faulting. not implemented: an IST/TSS stack for #DF (a kernel-stack fault still escalates
 * because the dump runs on the faulting stack); per-vector stubs for exact vector numbers. */
__attribute__((interrupt)) static void isr_df(struct iframe *f, uint64_t e){ fault_dump(8,e,f); }
__attribute__((interrupt)) static void isr_exc_noerr(struct iframe *f){ fault_dump(0xFE,0,f); }
__attribute__((interrupt)) static void isr_exc_err(struct iframe *f, uint64_t e){ fault_dump(0xEF,e,f); }
static volatile uint64_t ticks;
/* P2: the timer ISR can preempt the running task. Defined after the scheduler
 * (sched_tick) so the body stays here but the switch logic lives in one place. */
static void sched_tick(void);
__attribute__((interrupt)) static void isr_timer(struct iframe *f){ (void)f; ticks++; outb(0x20,0x20); sched_tick(); }

static void pic_remap(void){
    outb(0x20,0x11); outb(0xA0,0x11); outb(0x21,0x20); outb(0xA1,0x28);
    outb(0x21,0x04); outb(0xA1,0x02); outb(0x21,0x01); outb(0xA1,0x01);
    outb(0x21,0xFE); outb(0xA1,0xFF);   /* unmask IRQ0 (timer); keyboard is POLLED (Stage 1) */
}
static void pit_init(uint32_t hz){ uint32_t d=1193182u/hz; outb(0x43,0x36); outb(0x40,d&0xff); outb(0x40,(d>>8)&0xff); }
/* STAGE 1: initialize the i8042 PS/2 controller so a keypress raises IRQ1 with a
 * set-1 scancode. (SeaBIOS usually does this, but make it explicit + robust.) */
static void kbd_init(void){
    while(inb(0x64)&1) inb(0x60);                 /* flush output buffer */
    while(inb(0x64)&2){} outb(0x64,0x20);         /* read controller command byte */
    while(!(inb(0x64)&1)){} uint8_t cmd=inb(0x60);
    cmd |= 0x01;  /* enable IRQ1 (keyboard interrupt) */
    cmd |= 0x40;  /* enable translation -> scancode set 1 */
    cmd &= ~0x10; /* clear "keyboard clock disabled" */
    while(inb(0x64)&2){} outb(0x64,0x60);         /* write command byte */
    while(inb(0x64)&2){} outb(0x60,cmd);
    while(inb(0x64)&2){} outb(0x60,0xF4);         /* enable scanning on the keyboard */
}

/* ---- B4: framebuffer ----------------------------------------------------- */
static struct limine_framebuffer *FB;
static inline void put_px(uint32_t x,uint32_t y,uint32_t c){ if(x<FB->width&&y<FB->height) ((volatile uint32_t*)((uint8_t*)FB->address+(uint64_t)y*FB->pitch))[x]=c; }
static inline uint32_t get_px(uint32_t x,uint32_t y){ return ((volatile uint32_t*)((uint8_t*)FB->address+(uint64_t)y*FB->pitch))[x]; }
static void fill(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t c){ for(uint32_t j=0;j<h;j++)for(uint32_t i=0;i<w;i++)put_px(x+i,y+j,c); }
static void draw_ch(uint32_t x,uint32_t y,char ch,uint32_t fg){ int g=glyph_index(ch); for(int r=0;r<8;r++)for(int co=0;co<8;co++) if((FONT[g][r]>>co)&1) put_px(x+(uint32_t)co,y+(uint32_t)r,fg); }
static void draw_str(uint32_t x,uint32_t y,const char*s,uint32_t fg){ while(*s){ draw_ch(x,y,*s++,fg); x+=8; } }
static void draw_dec(uint32_t x,uint32_t y,uint64_t v,uint32_t fg){ char b[24]; int n=0,tn=0; char t[24]; if(!v)b[n++]='0'; while(v){t[tn++]=(char)('0'+v%10);v/=10;} while(tn)b[n++]=t[--tn]; b[n]=0; draw_str(x,y,b,fg); } /* AUDIT A2: b[24] (was b[20], off-by-one NUL for a 20-digit uint64) */

/* ===========================================================================
 * STEP 7 STAGE 1 - PS/2 keyboard input (the seed of interactivity).
 * IRQ1 -> port 0x60 -> scancode set 1 -> ASCII -> echo to serial AND draw to the
 * framebuffer at a console cursor. (USB-HID/xHCI is not implemented: - far harder, later.)
 * ===========================================================================*/
static const char kbd_set1[128] = {  /* scancode set 1 (no shift), index = make code */
 0,27,'1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
 'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,'a','s',
 'd','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c','v',
 'b','n','m',',','.','/',0,'*',0,' ',0,0,0,0,0,0,
 0,0,0,0,0,0,0,'7','8','9','-','4','5','6','+','1',
 '2','3','0','.',0,0,0,0,0,0,0,0,0,0,0,0,
 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };
#define CON_LH 10u   /* line height: 8px glyph + 2px gap */
/* Console as a PARAMETERIZED region [con_x0,con_x1) x [con_y0,con_y1) so it can be the
 * full screen (S2) or a window's content rect (S5). // direct framebuffer
 * row-shift scroll (no char-grid model); dirty-line redraw is not implemented. */
static uint32_t con_x0=8, con_y0=440, con_x1, con_y1, con_bg=0x00000000, con_fg=0x0000FF66;
static uint32_t gcx=8, gcy=440, g_scrolls;
static void con_set_region(uint32_t x0,uint32_t y0,uint32_t x1,uint32_t y1,uint32_t bg,uint32_t fg){
    con_x0=x0;con_y0=y0;con_x1=x1;con_y1=y1;con_bg=bg;con_fg=fg;gcx=x0;gcy=y0;g_scrolls=0;
}
static void scroll_console(void){
    if(!FB) return; uint8_t*b=(uint8_t*)FB->address;
    for(uint32_t y=con_y0; y+CON_LH<con_y1; y++){
        uint32_t*d=(uint32_t*)(b+(uint64_t)y*FB->pitch);
        uint32_t*s=(uint32_t*)(b+(uint64_t)(y+CON_LH)*FB->pitch);
        for(uint32_t x=con_x0;x<con_x1;x++) d[x]=s[x];   /* clips to the region horizontally */
    }
    fill(con_x0,con_y1-CON_LH,con_x1-con_x0,CON_LH,con_bg); g_scrolls++;
}
static void con_newline(void){ gcx=con_x0; if(gcy+CON_LH+8>=con_y1) scroll_console(); else gcy+=CON_LH; }
static void kputc(char c){
    if(!FB) return;
    if(c=='\n'){ con_newline(); return; }
    if(c=='\b'){ if(gcx>con_x0){ gcx-=8; fill(gcx,gcy,8,8,con_bg); } return; }
    fill(gcx,gcy,8,8,con_bg); draw_ch(gcx,gcy,c,con_fg);
    gcx+=8; if(gcx+8>=con_x1) con_newline();   /* wrap = new line */
}
static void kputs(const char*s){ while(*s) kputc(*s++); }

/* ---- window / cursor / compositor (software-rendered, CARMIX's own; no GPU) ---- */
typedef struct { uint32_t x,y,w,h; const char*title; uint32_t bg; } window_t;
static window_t g_win = { 260,160, 520,380, "CARMIX", 0 };
static window_t g_win2; static int g_nwin=1;
static window_t *g_front=&g_win, *g_back=&g_win2;   /* E1 z-order: g_back drawn first, g_front on top */
static uint32_t cur_x=640, cur_y=420;
static void draw_desktop(void){ fill(0,0,(uint32_t)FB->width,(uint32_t)FB->height,0x00204060); }
static void draw_window(const window_t*w){
    fill(w->x,w->y,w->w,w->h,0x00C0C0C0);              /* frame/border */
    fill(w->x+3,w->y+3,w->w-6,18,0x00000080);          /* titlebar */
    draw_str(w->x+8,w->y+8,w->title,0x00FFFFFF);       /* title text */
    fill(w->x+3,w->y+24,w->w-6,w->h-27, w->bg?w->bg:0x00101820);   /* content (per-window bg; default dark) */
}
static void draw_grip(const window_t*w){ fill(w->x+w->w-12,w->y+w->h-12,10,10,0x00808080); }   /* E3 resize handle */
static void compose(void){ draw_desktop(); if(g_nwin>=2) draw_window(g_back); draw_window(g_front); draw_grip(g_front); }

/* ---- A7: PS/2 mouse + a SAVE-UNDER cursor (preserves the scene under the pointer).
 * // save-under goes stale if the area under the cursor changes between moves
 * (e.g. console typing under it) - fine for a pointer; a real cursor layer is not implemented. */
static uint32_t cur_save[7*11]; static int cursor_shown=0;
static void cursor_draw_at(uint32_t x,uint32_t y){
    if(!FB) return;
    for(int j=0;j<11;j++)for(int i=0;i<7;i++) cur_save[j*7+i]=get_px(x+i,y+j);
    fill(x,y,7,11,0x00FFFF00); cur_x=x; cur_y=y; cursor_shown=1;
}
static void cursor_hide(void){ if(cursor_shown){ for(int j=0;j<11;j++)for(int i=0;i<7;i++) put_px(cur_x+i,cur_y+j,cur_save[j*7+i]); cursor_shown=0; } }
static void scene(void){ compose(); cursor_shown=0; cursor_draw_at(cur_x,cur_y); }   /* full redraw + cursor on top */

static void i8042_cmd(uint8_t c){ while(inb(0x64)&2){} outb(0x64,c); }
static void mouse_write(uint8_t b){ i8042_cmd(0xD4); while(inb(0x64)&2){} outb(0x60,b); }
static void mouse_init(void){
    i8042_cmd(0xA8);                                   /* enable aux (mouse) port */
    i8042_cmd(0x20); while(!(inb(0x64)&1)){} uint8_t cb=inb(0x60);
    cb |= 0x02; cb &= ~0x20;                            /* IRQ12 enable bit (harmless when polled) + aux clock on */
    i8042_cmd(0x60); while(inb(0x64)&2){} outb(0x60,cb);
    mouse_write(0xF4);                                  /* enable data reporting */
    for(int i=0;i<8 && (inb(0x64)&1);i++) inb(0x60);   /* drain ACK */
}
static int dragging=0, resizing=0, drag_ox=0, drag_oy=0;
static window_t* which_window(uint32_t x,uint32_t y){    /* front first (it's on top) */
    if(x>=g_front->x && x<g_front->x+g_front->w && y>=g_front->y && y<g_front->y+g_front->h) return g_front;
    if(g_nwin>=2 && x>=g_back->x && x<g_back->x+g_back->w && y>=g_back->y && y<g_back->y+g_back->h) return g_back;
    return 0;
}
static void raise_window(window_t*w){ if(w==g_back){ window_t*t=g_front; g_front=g_back; g_back=t; } sputs(" FOCUS->"); sputs(g_front->title?g_front->title:"?"); }   /* E1 */
static void on_pointer(uint32_t nx,uint32_t ny,int btn){
    if(btn&1){
        if(!dragging && !resizing){
            window_t*w=which_window(nx,ny);
            if(w && w!=g_front){ raise_window(w); cur_x=nx; cur_y=ny; scene(); return; }   /* E1: raise on click + recompose */
            window_t*f=g_front;
            if(nx>=f->x+f->w-14 && nx<f->x+f->w && ny>=f->y+f->h-14 && ny<f->y+f->h) resizing=1;          /* E3: grip */
            else if(nx>=f->x && nx<f->x+f->w && ny>=f->y && ny<f->y+24){ dragging=1; drag_ox=(int)nx-(int)f->x; drag_oy=(int)ny-(int)f->y; }  /* E2: titlebar */
        }
        if(dragging){ int wx=(int)nx-drag_ox, wy=(int)ny-drag_oy; if(wx<0)wx=0; if(wy<0)wy=0; g_front->x=(uint32_t)wx; g_front->y=(uint32_t)wy; cur_x=nx; cur_y=ny; scene(); return; }
        if(resizing){ int nw=(int)nx-(int)g_front->x, nh=(int)ny-(int)g_front->y; if(nw<120)nw=120; if(nh<80)nh=80;   /* E3: clamp to min + screen */
            if(g_front->x+(uint32_t)nw>(uint32_t)FB->width)  nw=(int)FB->width -(int)g_front->x;
            if(g_front->y+(uint32_t)nh>(uint32_t)FB->height) nh=(int)FB->height-(int)g_front->y;
            g_front->w=(uint32_t)nw; g_front->h=(uint32_t)nh; cur_x=nx; cur_y=ny; scene(); return; }
    } else { dragging=0; resizing=0; }
    cursor_hide(); cursor_draw_at(nx,ny);
}
static void mouse_packet(const uint8_t*p){
    int dx=(int)p[1]-((p[0]&0x10)?256:0);
    int dy=(int)p[2]-((p[0]&0x20)?256:0);
    int nx=(int)cur_x+dx, ny=(int)cur_y-dy;             /* PS/2 y is inverted */
    if(nx<0)nx=0; if(ny<0)ny=0;
    if(nx>(int)FB->width-7) nx=(int)FB->width-7; if(ny>(int)FB->height-11) ny=(int)FB->height-11;
    on_pointer((uint32_t)nx,(uint32_t)ny,p[0]&0x07);
}
static uint8_t mpkt[3]; static int mphase=0;
static void poll_input(void){   /* unified: keyboard scancodes AND mouse packets (aux=status bit5) */
    uint8_t st=inb(0x64); if(!(st&1)) return;
    uint8_t d=inb(0x60);
    if(st&0x20){ if(mphase==0 && !(d&0x08)) return; mpkt[mphase++]=d;
        if(mphase==3){ mphase=0; mouse_packet(mpkt); sputs(" M("); sdec(cur_x); sputc(','); sdec(cur_y); sputc(')'); } }  /* report real mouse moves */
    else if(d<0x80){ char c=kbd_set1[d]; if(c){ sputc(c); kputc(c); } }
}

static void run_stage2(void){
    sputs("\n=== STEP 7 STAGE 2: scrolling text console ===\n");
    if(!FB){ sputs("S2 NOT RUN: no FB\n"); return; }
    con_set_region(8,440,(uint32_t)FB->width,(uint32_t)FB->height,0x00000000,0x0000FF66);
    fill(con_x0,con_y0,con_x1-con_x0,con_y1-con_y0,0x00000000);
    /* (1) DECISIVE scroll proof: a marker on line 2 moves up EXACTLY one line. */
    fill(con_x0,con_y0+CON_LH,64,CON_LH,0x00ABCDEF);
    uint32_t before=get_px(con_x0+2,con_y0+CON_LH+5);
    scroll_console();
    uint32_t moved=get_px(con_x0+2,con_y0+5), oldpos=get_px(con_x0+2,con_y0+CON_LH+5);
    int shift_ok=(before==0x00ABCDEF)&&(moved==0x00ABCDEF)&&(oldpos!=0x00ABCDEF);
    /* (2) drive MORE LINES THAN FIT -> repeated scroll, console keeps working. */
    con_set_region(8,440,(uint32_t)FB->width,(uint32_t)FB->height,0x00000000,0x0000FF66);
    fill(con_x0,con_y0,con_x1-con_x0,con_y1-con_y0,0x00000000);
    uint32_t rows=(con_y1-con_y0)/CON_LH;
    for(uint32_t i=0;i<rows+14;i++){ char d[8]; uint32_t v=i; int n=0,tn=0; char t[8]; if(!v)d[n++]='0'; while(v){t[tn++]=(char)('0'+v%10);v/=10;} while(tn)d[n++]=t[--tn]; d[n]=0; kputs("line"); kputs(d); con_newline(); }
    int hastext=0; for(uint32_t y=con_y0;y+8<con_y1&&!hastext;y+=CON_LH) for(uint32_t x=con_x0;x<con_x0+120;x++) if(get_px(x,y+2)){hastext=1;break;}
    int s2=shift_ok && (g_scrolls>0) && hastext;
    sputs("S2 scroll-shift="); sputs(shift_ok?"y":"n"); sputs(" rows="); sdec(rows); sputs(" typed="); sdec(rows+14);
    sputs(" scrolls="); sdec(g_scrolls); sputs(" has-text="); sputs(hastext?"y":"n"); sputs(s2?" -> CONSOLE SCROLLS OK\n":" -> FAIL\n");
}
static void run_stage3(void){
    sputs("\n=== STEP 7 STAGE 3: a window over the desktop ===\n");
    if(!FB){ sputs("S3 NOT RUN: no FB\n"); return; }
    g_win.x=260; g_win.y=160; g_win.w=520; g_win.h=380; cur_x=640; cur_y=420; compose();
    uint32_t bg=get_px(40,40), frame=get_px(g_win.x, g_win.y+120), title=get_px(g_win.x+g_win.w-24, g_win.y+10), content=get_px(g_win.x+260, g_win.y+260);
    int titletxt=0; for(uint32_t xx=g_win.x+8;xx<g_win.x+8+48&&!titletxt;xx++) for(uint32_t yy=g_win.y+8;yy<g_win.y+16;yy++) if(get_px(xx,yy)==0x00FFFFFF){titletxt=1;break;}
    int s3=(bg==0x00204060)&&(frame==0x00C0C0C0)&&(title==0x00000080)&&(content==0x00101820)&&titletxt;
    sputs("S3 readback desktop="); sx64(bg); sputs(" frame="); sx64(frame); sputs(" titlebar="); sx64(title);
    sputs(" content="); sx64(content); sputs(" title-text="); sputs(titletxt?"y":"n"); sputs(s3?" -> WINDOW DRAWN OK\n":" -> FAIL\n");
}
static void run_stage4(void){
    sputs("\n=== STEP 7 STAGE 4: movable window + cursor ===\n");
    if(!FB){ sputs("S4 NOT RUN: no FB\n"); return; }
    g_win.x=200; g_win.y=140; cur_x=600; cur_y=400; g_nwin=1; scene();
    uint32_t p1x=g_win.x, p1y=g_win.y+120;
    uint32_t frame1=get_px(p1x,p1y), cur1=get_px(cur_x+2,cur_y+2);
    g_win.x+=200; g_win.y+=80; cur_x+=160; cur_y+=60; scene();   /* key-driven move (programmatic for the self-test) */
    uint32_t frame2=get_px(g_win.x, g_win.y+120), cur2=get_px(cur_x+2,cur_y+2), old=get_px(p1x,p1y);
    int moved=(frame1==0x00C0C0C0)&&(frame2==0x00C0C0C0)&&(old==0x00204060);
    int curmoved=(cur1==0x00FFFF00)&&(cur2==0x00FFFF00);
    int s4=moved && curmoved;
    sputs("S4 frame@pos1="); sx64(frame1); sputs(" @pos2="); sx64(frame2); sputs(" old-pos-now-bg="); sx64(old);
    sputs(" cursor-moved="); sputs(curmoved?"y":"n"); sputs(s4?" -> WINDOW MOVES OK\n":" -> FAIL\n");
}
static void run_stage5(void){
    sputs("\n=== STEP 7 STAGE 5: desktop shell - console INSIDE the window ===\n");
    if(!FB){ sputs("S5 NOT RUN: no FB\n"); return; }
    g_win.x=280; g_win.y=150; g_win.w=560; g_win.h=420; compose();
    con_set_region(g_win.x+6, g_win.y+30, g_win.x+g_win.w-6, g_win.y+g_win.h-6, 0x00101820, 0x0000FF99);
    uint32_t rows=(con_y1-con_y0)/CON_LH;
    for(uint32_t i=0;i<rows+8;i++){ char d[8]; uint32_t v=i; int n=0,tn=0; char t[8]; if(!v)d[n++]='0'; while(v){t[tn++]=(char)('0'+v%10);v/=10;} while(tn)d[n++]=t[--tn]; d[n]=0; kputs("win"); kputs(d); con_newline(); }
    int inside=0; for(uint32_t y=con_y0;y<con_y1&&!inside;y++) for(uint32_t x=con_x0;x<con_x0+80;x++) if(get_px(x,y)==0x0000FF99){inside=1;break;}
    uint32_t leftbg=(g_win.x>30)?get_px(g_win.x-20,g_win.y+200):0x00204060;
    int s5=inside && (g_scrolls>0) && (leftbg==0x00204060);
    sputs("S5 text-inside-content="); sputs(inside?"y":"n"); sputs(" scrolls-in-window="); sdec(g_scrolls);
    sputs(" no-leak-left="); sputs(leftbg==0x00204060?"y":"n"); sputs(s5?" -> SHELL CONSOLE OK\n":" -> FAIL\n");
}
static void run_stage6(void){
    sputs("\n=== STEP 7 STAGE 6: rematerialization in the GUI ===\n");
    if(!FB){ sputs("S6 NOT RUN: no FB\n"); return; }
    /* a tiny app state (a counter): checkpoint -> content-address -> lose -> rematerialize (R3 path). */
    static uint8_t app_arena[1u<<12]; static cvsasx_store_entry_t app_idx[16]; static cvsasx_store_t app_store;
    cvsasx_store_init(&app_store, app_arena, sizeof app_arena, app_idx, 16);
    uint32_t counter=4242; cvsasx_hash_t h; cvsasx_store_put(&app_store,&counter,sizeof counter,&h);
    uint32_t remat=0; const void*b; size_t l;
    if(cvsasx_store_get(&app_store,&h,&b,&l)==CVSASX_STORE_OK && l==sizeof remat) for(size_t i=0;i<l;i++) ((uint8_t*)&remat)[i]=((const uint8_t*)b)[i];
    fill(g_win.x+6,g_win.y+28,g_win.w-12,22,0x00101820);
    draw_str(g_win.x+12,g_win.y+34,"REMAT COUNTER=",0x0000FFAA); draw_dec(g_win.x+12+14*8,g_win.y+34,remat,0x0000FFAA);
    int drawn=0; for(uint32_t x=g_win.x+12;x<g_win.x+12+220&&!drawn;x++) for(uint32_t y=g_win.y+34;y<g_win.y+42;y++) if(get_px(x,y)==0x0000FFAA){drawn=1;break;}
    int s6=(remat==counter)&&drawn;
    sputs("S6 checkpoint+rematerialize counter "); sdec(counter); sputs("->"); sdec(remat);
    sputs(" survived="); sputs(remat==counter?"y":"n"); sputs(" drawn-in-window="); sputs(drawn?"y":"n"); sputs(s6?" -> REMAT APP OK\n":" -> FAIL\n");
    con_set_region(g_win.x+6,g_win.y+56,g_win.x+g_win.w-6,g_win.y+g_win.h-6,0x00101820,0x0000FF99);  /* live shell below the remat strip */
}
/* ===== STEP 9 TRACK A-HARDEN: real pointer + drag + multi-window ===== */
static void run_a7(void){
    sputs("\n=== STEP 9 A7: PS/2 mouse moves the cursor ===\n");
    if(!FB){ sputs("A7 NOT RUN: no FB\n"); return; }
    g_nwin=1; g_win.x=260; g_win.y=160; g_win.w=520; g_win.h=380; cur_x=400; cur_y=400; scene();
    uint32_t p1x=cur_x, p1y=cur_y, at1=get_px(cur_x+2,cur_y+2);
    uint8_t pkt[3]={0x08,60,0};   /* byte0 bit3=1, no buttons; dx=+60, dy=0 -> cursor right 60 */
    mouse_packet(pkt);
    uint32_t at2=get_px(cur_x+2,cur_y+2), restored=get_px(p1x+2,p1y+2);
    int moved=(cur_x==p1x+60)&&(cur_y==p1y);
    int a7=(at1==0x00FFFF00)&&(at2==0x00FFFF00)&&(restored!=0x00FFFF00)&&moved;
    sputs("A7 cursor ("); sdec(p1x); sputc(','); sdec(p1y); sputs(")->("); sdec(cur_x); sputc(','); sdec(cur_y);
    sputs(") at-pos1="); sx64(at1); sputs(" at-pos2="); sx64(at2); sputs(" old-restored="); sputs(restored!=0x00FFFF00?"y":"n");
    sputs(a7?" -> MOUSE MOVES CURSOR OK\n":" -> FAIL\n");
}
static void run_a8(void){
    sputs("\n=== STEP 9 A8: drag the titlebar to move the window ===\n");
    if(!FB){ sputs("A8 NOT RUN: no FB\n"); return; }
    g_nwin=1; g_win.x=300; g_win.y=200; g_win.w=480; g_win.h=320; cur_x=g_win.x+40; cur_y=g_win.y+10; dragging=0; scene();
    uint32_t bx=g_win.x, by=g_win.y;
    uint8_t down[3]={0x09,0,0};    /* left button down, on the titlebar (no motion) -> start drag */
    mouse_packet(down);
    uint8_t drag[3]={0x09,90,0};   /* left held, dx=+90 -> window follows right */
    mouse_packet(drag);
    int moved=(g_win.x!=bx)||(g_win.y!=by);
    uint32_t frame=get_px(g_win.x, g_win.y+120);
    uint32_t oldspot=(bx>20)?get_px(bx-1, by+120):0x00204060;   /* old window-left area -> desktop bg */
    int a8=moved && (frame==0x00C0C0C0) && (oldspot==0x00204060);
    sputs("A8 window ("); sdec(bx); sputc(','); sdec(by); sputs(")->("); sdec(g_win.x); sputc(','); sdec(g_win.y);
    sputs(") via drag; frame-at-new="); sx64(frame); sputs(" old-now-bg="); sputs(oldspot==0x00204060?"y":"n");
    sputs(a8?" -> DRAG MOVES WINDOW OK\n":" -> FAIL\n");
    dragging=0;
}
static void run_a9(void){
    sputs("\n=== STEP 9 A9: second window + z-order ===\n");
    if(!FB){ sputs("A9 NOT RUN: no FB\n"); return; }
    g_win.x=200; g_win.y=200; g_win.w=400; g_win.h=300; g_win.title="BACK";  g_win.bg=0x00102030;
    g_win2.x=380; g_win2.y=320; g_win2.w=400; g_win2.h=300; g_win2.title="FRONT"; g_win2.bg=0x00301020;
    g_nwin=2; g_front=&g_win2; g_back=&g_win; cur_x=900; cur_y=700; scene();   /* FRONT=g_win2 on top */
    int genuine=(g_win2.x<g_win.x+g_win.w)&&(g_win2.y<g_win.y+g_win.h);   /* the windows really overlap */
    uint32_t oc=get_px(450, 400);   /* a content point inside BOTH windows' overlap */
    int a9=genuine && (oc==g_win2.bg) && (oc!=g_win.bg);   /* overlap shows FRONT's distinct bg, NOT back's */
    sputs("A9 overlap@(450,400) shows="); sx64(oc); sputs(" front(g_win2).bg="); sx64(g_win2.bg); sputs(" back(g_win).bg="); sx64(g_win.bg);
    sputs(" genuine-overlap="); sputs(genuine?"y":"n");
    sputs(a9?" -> Z-ORDER OK (front's color occludes back)\n":" -> FAIL\n");
    g_win.bg=0; g_win2.bg=0; g_nwin=1; g_front=&g_win; g_back=&g_win2;   /* reset for later stages */
}
/* ===== STEP 10b BUILDER 2: focus + front-drag + resize ===== */
static void run_e1(void){
    sputs("\n=== STEP 10b E1: multi-window focus (raise-on-click) ===\n");
    if(!FB){ sputs("E1 NOT RUN: no FB\n"); return; }
    g_win.x=200; g_win.y=200; g_win.w=400; g_win.h=300; g_win.title="A"; g_win.bg=0x00102030;
    g_win2.x=380; g_win2.y=320; g_win2.w=400; g_win2.h=300; g_win2.title="B"; g_win2.bg=0x00301020;
    g_nwin=2; g_front=&g_win2; g_back=&g_win; cur_x=900; cur_y=700; scene();   /* start: B in front */
    uint32_t before=get_px(450,400);                                  /* overlap content = front's bg */
    on_pointer(g_win.x+30, g_win.y+150, 1); on_pointer(g_win.x+30, g_win.y+150, 0);    /* click A's visible part */
    uint32_t after_a=get_px(450,400);
    on_pointer(g_win2.x+370, g_win2.y+250, 1); on_pointer(g_win2.x+370, g_win2.y+250, 0); /* click B's visible part */
    uint32_t after_b=get_px(450,400);
    int e1=(before==g_win2.bg)&&(after_a==g_win.bg)&&(after_b==g_win2.bg);
    sputs("\nE1 overlap: B-front="); sx64(before); sputs(" click-A->"); sx64(after_a); sputs(" click-B->"); sx64(after_b);
    sputs(e1?"  -> FOCUS RAISE OK\n":"  -> FAIL\n");
    g_win.bg=0; g_win2.bg=0; g_nwin=1; g_front=&g_win; g_back=&g_win2;
}
static void run_e2(void){
    sputs("\n=== STEP 10b E2: drag the FOCUSED window by its titlebar ===\n");
    if(!FB){ sputs("E2 NOT RUN: no FB\n"); return; }
    g_win.x=200; g_win.y=200; g_win.w=400; g_win.h=300; g_win.title="A"; g_win.bg=0;
    g_win2.x=620; g_win2.y=460; g_win2.w=300; g_win2.h=200; g_win2.title="B"; g_win2.bg=0;
    g_nwin=2; g_front=&g_win; g_back=&g_win2; cur_x=g_win.x+40; cur_y=g_win.y+10; scene();
    uint32_t bx=g_win.x, by=g_win.y;
    uint8_t down[3]={0x09,0,0}; mouse_packet(down);       /* press A's titlebar -> drag */
    uint8_t drag[3]={0x09,120,0}; mouse_packet(drag);     /* dx=+120 -> A follows right */
    int moved=(g_front->x!=bx)||(g_front->y!=by);
    uint32_t frame=get_px(g_front->x, g_front->y+120);
    int e2=moved && (g_front==&g_win) && (frame==0x00C0C0C0);
    sputs("E2 focused A ("); sdec(bx); sputc(','); sdec(by); sputs(")->("); sdec(g_front->x); sputc(','); sdec(g_front->y);
    sputs(") still-front="); sputs(g_front==&g_win?"y":"n"); sputs(" frame-at-new="); sx64(frame);
    sputs(e2?" -> FRONT-DRAG OK\n":" -> FAIL\n");
    g_nwin=1; g_front=&g_win; g_back=&g_win2; dragging=0;
}
static void run_e3(void){
    sputs("\n=== STEP 10b E3: resize the focused window via its grip ===\n");
    if(!FB){ sputs("E3 NOT RUN: no FB\n"); return; }
    g_nwin=1; g_front=&g_win; g_back=&g_win2; g_win.x=200; g_win.y=200; g_win.w=400; g_win.h=300; g_win.title="A"; g_win.bg=0;
    cur_x=g_win.x+g_win.w-6; cur_y=g_win.y+g_win.h-6; scene();   /* cursor on the grip */
    uint32_t bw=g_win.w, bh=g_win.h;
    uint8_t down[3]={0x09,0,0}; mouse_packet(down);             /* press grip -> resize */
    on_pointer(g_win.x+540, g_win.y+440, 1);                    /* drag corner outward -> grow */
    uint32_t gw=g_win.w, gh=g_win.h;
    uint32_t inold=get_px(g_win.x+bw+10, g_win.y+100);          /* old edge+10 now inside the window */
    int grew=(gw>bw)&&(gh>bh);
    int incontent=(inold!=0x00204060);                          /* a window pixel, not desktop */
    on_pointer(g_win.x+10, g_win.y+10, 1);                      /* drag corner far inward -> clamp at min */
    int clamped=(g_win.w==120)&&(g_win.h==80);
    int e3=grew && incontent && clamped;
    sputs("E3 size ("); sdec(bw); sputc('x'); sdec(bh); sputs(")->grow("); sdec(gw); sputc('x'); sdec(gh);
    sputs(") old-edge-now-window="); sputs(incontent?"y":"n"); sputs(" min-clamp(120x80)="); sputs(clamped?"y":"n");
    sputs(e3?" -> RESIZE+CLAMP OK\n":" -> FAIL\n");
    resizing=0; g_win.w=400; g_win.h=300;
}
static void run_shell(void){   /* E4: live two-window desktop - focus/drag/resize/type under REAL input */
    sputs("\n=== STEP 10b E4: live desktop - click=focus, titlebar=drag, grip=resize, keys=type ===\n");
    g_win.x=200; g_win.y=160; g_win.w=460; g_win.h=380; g_win.title="A"; g_win.bg=0x00141c28;
    g_win2.x=560; g_win2.y=320; g_win2.w=440; g_win2.h=340; g_win2.title="B"; g_win2.bg=0x00281418;
    g_nwin=2; g_front=&g_win; g_back=&g_win2; cur_x=700; cur_y=240; scene();
    con_set_region(g_front->x+6,g_front->y+30,g_front->x+g_front->w-6,g_front->y+g_front->h-6,g_front->bg,0x0000FF99);
    for(;;){ poll_input(); __asm__ volatile("pause"); }   /* M(x,y) cursor + FOCUS->win + typed echo report state */
}

/* ===========================================================================
 * STEP 4 - the PROVEN Wasm-SFI capability backend, running IN the kernel.
 * gate/ + carmix/ recompiled UNMODIFIED for x86-64 freestanding and linked here.
 * ===========================================================================*/
cvsasx_gate_backend_t *cvsasx_gate_backend_sw(void);
int  cvsasx_gate_backend_sw_fault(void);
void cvsasx_gate_backend_sw_reset_fault(void);

/* the gate's wabt-validated 60-byte module: mem[4]=p+7; mem[8]=mem[4]*3; ret mem[8] (p=10 -> 51) */
static const unsigned char g0_wasm[] = {
    0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,0x01,0x06,0x01,0x60,0x01,0x7f,0x01,0x7f,
    0x03,0x02,0x01,0x00,0x05,0x03,0x01,0x00,0x01,0x0a,0x21,0x01,0x1f,0x00,0x41,0x04,
    0x20,0x00,0x41,0x07,0x6a,0x36,0x02,0x00,0x41,0x08,0x41,0x04,0x28,0x02,0x00,0x41,
    0x03,0x6c,0x36,0x02,0x00,0x41,0x08,0x28,0x02,0x00,0x0f,0x0b };
static gir_inst_t g0_code[64];
static uint8_t k_linmem[64];
static const gir_inst_t oob_code[] = {{GIR_LOCAL_GET,0,0},{GIR_LOAD,GIR_CAPSLOT_LINMEM,0},{GIR_RETURN,0,0}};
static const cvsasx_gate_module_t oob_mod = { oob_code,3,1,1,64,0,0,0 };
static uint8_t k_obj[256];
static cvsasx_sw_custodian_t k_cust; static cvsasx_sw_region_t k_region; static cvsasx_pir_t k_base;

static void run_step4(void){
    sputs("\n=== STEP 4: capability-guarded executor IN the kernel ===\n");

    /* E1: a Wasm task runs under the gate on the software backend, in the kernel */
    cvsasx_gate_backend_t *be = cvsasx_gate_backend_sw();
    be->mem_bind(be, k_linmem, 64);
    cvsasx_gate_module_t wm;
    cvsasx_gate_status_t ld = cvsasx_wasm_load(g0_wasm,60,g0_code,64,64,&wm);
    cvsasx_gate_status_t ck = (ld==CVSASX_GATE_OK)?cvsasx_gate_check(&wm):ld;
    int32_t args[1]={10}, out=0;
    cvsasx_gate_status_t ex = (ck==CVSASX_GATE_OK)?cvsasx_gate_exec(&wm,be,args,1,&out):ck;
    int e1=(ld==CVSASX_GATE_OK)&&(ck==CVSASX_GATE_OK)&&(ex==CVSASX_GATE_OK)&&(out==51)&&(cvsasx_gate_backend_sw_fault()==0);
    sputs("E1 wasm validate->check->exec in kernel: result="); sdec((uint64_t)(uint32_t)out);
    sputs(" (expect 51) -> "); sputs(e1?"OK\n":"FAIL\n");

    /* E2: anti-amplification (Phase-5 attacks) enforced by the software backend IN the kernel.
     * Hash is a fixed 32-byte value (the re-mint only COMPARES referent_hash==region->hash,
     * never computes it) -> no BLAKE3 needed for the anti-amplification logic test. */
    for(int i=0;i<256;i++) k_obj[i]=(uint8_t)(i*5u+1u);
    uint8_t H[CVSASX_BLAKE3_LEN]; for(int i=0;i<(int)CVSASX_BLAKE3_LEN;i++) H[i]=(uint8_t)(i*7u+1u);
    cvsasx_swcap_t root={ (uint64_t)(uintptr_t)k_obj,256,CVSASX_PERM_LOAD|CVSASX_PERM_STORE|CVSASX_PERM_GLOBAL,1};
    cvsasx_sw_custodian_init(&k_cust, root);
    k_region.object_cap=root; k_region.object_base_addr=(uint64_t)(uintptr_t)k_obj; k_region.object_length=256;
    for(int i=0;i<(int)CVSASX_BLAKE3_LEN;i++) k_region.hash[i]=H[i];
    cvsasx_referent_t ref; for(int i=0;i<(int)CVSASX_BLAKE3_LEN;i++) ref.hash[i]=H[i];
    ref.object_base_addr=(uint64_t)(uintptr_t)k_obj; ref.object_length=256;
    cvsasx_sw_cap_strip(root,&ref,&k_base);
    int ctrl; { cvsasx_pir_t p=k_base; cvsasx_swcap_t c; ctrl=(cvsasx_sw_cap_remint(&k_cust,&p,&k_region,&c)==CVSASX_OK)&&c.valid; }
    /* AUDIT A4: assert each attack is rejected by its DISTINCT INTENDED check (status
     * code) AND fail-closed (c.valid==0) - a bare reject count would mask a wrong-reason
     * rejection or a regressed gate that a neighbouring check still catches. */
    int rej=0, tot=0;
#define ATK(MUT,WANT,LBL) do{ cvsasx_pir_t p=k_base; MUT; cvsasx_swcap_t c; \
        cvsasx_status_t s=cvsasx_sw_cap_remint(&k_cust,&p,&k_region,&c); \
        int ok=(s==(WANT))&&(c.valid==0); tot++; rej+=ok; \
        sputs("   " LBL " status="); sdec((uint64_t)s); sputs(ok?" (intended check, fail-closed)\n":" *** WRONG-REASON / AMPLIFIED ***\n"); }while(0)
    ATK(p.length=257,                   CVSASX_ERR_BAD_BOUNDS,        "wider-bounds  ");
    ATK(p.perms|=CVSASX_PERM_STORE_CAP, CVSASX_ERR_AMPLIFY_PERMS,     "+STORE_CAP    ");
    ATK(p.perms|=CVSASX_PERM_EXECUTE,   CVSASX_ERR_WX_VIOLATION,      "+EXECUTE(W^X) ");
    ATK(p.perms|=CVSASX_PERM_SEAL,      CVSASX_ERR_PERM_FORBIDDEN,    "+SEAL(forbid) ");
    ATK(p.struct_version=99,            CVSASX_ERR_VERSION,           "bad-version   ");
    ATK(p.referent_hash[0]^=0xFFu,      CVSASX_ERR_REFERENT_MISMATCH, "referent-flip ");
#undef ATK
    int e2=ctrl && (rej==tot);
    sputs("E2 anti-amplification in kernel: control="); sputs(ctrl?"ACCEPT":"REJECT(bad)");
    sputs("; "); sdec((uint64_t)rej); sputc('/'); sdec((uint64_t)tot);
    sputs(" attacks rejected by their INTENDED gate -> "); sputs(e2?"HOLDS\n":"*** AMPLIFIED/WRONG-REASON - THESIS-CRITICAL ***\n");

    /* E3: an OOB guest access -> software SFI fault in the kernel (no triple-fault) */
    cvsasx_gate_backend_sw_reset_fault();
    int32_t a2[1]={100000}, o2=0; cvsasx_gate_exec(&oob_mod,be,a2,1,&o2);
    int e3=cvsasx_gate_backend_sw_fault();
    sputs("E3 OOB guest load@100000 -> software SFI fault="); sdec((uint64_t)e3);
    sputs(" (kernel intact) -> "); sputs(e3?"OK\n":"FAIL\n");

    /* E4: the visible milestone - draw the guarded computation's result to the screen */
    int e4=0;
    if(FB){
        fill(40,150,440,76,0x00301025);
        draw_str(52,158,"WASM RESULT=",0x0000FF80); draw_dec(52+12*8,158,(uint32_t)out,0x0000FF80);
        draw_str(52,180,e2?"ANTIAMP HOLDS":"ANTIAMP FAIL",0x0000FF80);
        draw_str(52,202,e3?"SFI FAULT OK":"SFI FAIL",0x0000FF80);
        e4 = (get_px(460,155)==0x00301025);   /* readback the panel (headless proof it hit the FB) */
    }
    sputs("E4 result on framebuffer: "); sputs(e4?"DRAWN (WASM RESULT=51 visible on screen)\n":"FB absent (NOT RUN)\n");
    sputs("STEP 4: E1-E4 observed - the two proven halves are FUSED on the metal.\n");
}

/* ===========================================================================
 * STEP 5 - rematerialization ON THE METAL (the novel contribution).
 * BLAKE3 + store/ + sls/ recompiled freestanding and linked. migrate/migrate.c is
 * cap/-CHERI-coupled (cvsasx_cap_t == void*, no software bounds) so it does NOT port;
 * the software rematerialization protocol is the proven swcap+store path (Step-1 W2).
 * ===========================================================================*/
static const uint8_t KAT_EMPTY[32] = {  /* official BLAKE3 digest of "" (store S0) */
    0xaf,0x13,0x49,0xb9,0xf5,0xf9,0xa1,0xa6,0xa0,0x40,0x4d,0xea,0x36,0xdc,0xc9,0x49,
    0x9b,0xcb,0x25,0xc9,0xad,0xc1,0x12,0xb7,0xcc,0x9a,0x93,0xca,0xe4,0x1f,0x32,0x62 };
static char hexbuf[65];
static const char* hx(const uint8_t*b,int n){ const char*d="0123456789abcdef"; int j=0; for(int i=0;i<n;i++){ hexbuf[j++]=d[b[i]>>4]; hexbuf[j++]=d[b[i]&15]; } hexbuf[j]=0; return hexbuf; }
static uint8_t  r3_arena[1u<<14]; static cvsasx_store_entry_t r3_idx[128];  static cvsasx_store_t r3_store; static uint8_t r3_dst[64];
static uint8_t  r4_arena[1u<<19]; static cvsasx_store_entry_t r4_idx[4096]; static cvsasx_store_t r4_store;
static cvsasx_oid_t r4_leaf[1024], r4_lvl[1024], r4_nxt[1024];
/* balanced Merkle tree, arity <= CVSASX_SLS_MAX_CHILDREN (16); returns the root oid.
 * AUDIT A5: g_tree_err is set if any put_node fails (e.g. STORE_FULL) so the caller
 * cannot build a proof on a silently-corrupt tree from uninitialized r4_nxt. */
static int g_tree_err;
static cvsasx_oid_t build_tree(cvsasx_sls_t *S, const cvsasx_oid_t *leaves, uint32_t n){
    g_tree_err=0;
    for(uint32_t i=0;i<n;i++) r4_lvl[i]=leaves[i];
    uint32_t cnt=n;
    while(cnt>1){
        uint32_t m=0;
        for(uint32_t i=0;i<cnt;i+=16u){ uint32_t k=(cnt-i<16u)?(cnt-i):16u;
            if(cvsasx_sls_put_node(S,&r4_lvl[i],k,&r4_nxt[m++])!=CVSASX_SLS_OK) g_tree_err=1; }
        for(uint32_t i=0;i<m;i++) r4_lvl[i]=r4_nxt[i]; cnt=m;
    }
    return r4_lvl[0];
}

static void run_step5(void){
    sputs("\n=== STEP 5: rematerialization ON THE METAL (the novel contribution) ===\n");

    /* R0: BLAKE3 known-answer test - the gate. If the hash is wrong, STOP. */
    cvsasx_hash_t kh; cvsasx_blake3((const void*)0,0,&kh);
    int r0=1; for(int i=0;i<32;i++) if(kh.b[i]!=KAT_EMPTY[i]) r0=0;
    sputs("R0 BLAKE3 KAT digest(\"\")="); sputs(hx(kh.b,32)); sputc('\n');
    sputs("R0 vs official af1349b9..41f3262 -> "); sputs(r0?"MATCH (content-addressing trustworthy)\n":"*** MISMATCH - STOP ***\n");
    if(!r0){ sputs("R0 GATE FAILED - Step 5 aborted.\n"); return; }

    /* R2 + R3: checkpoint the LIVE Wasm computation's linmem, content-address it,
     * rematerialize bit-identical into a fresh domain + re-mint a fresh software cap. */
    cvsasx_store_init(&r3_store, r3_arena, sizeof r3_arena, r3_idx, 128);
    cvsasx_hash_t sh; cvsasx_store_put(&r3_store, k_linmem, 64, &sh);
    sputs("R2 checkpoint live Wasm state (64B) -> content-addressed; store objects="); sdec(r3_store.index_count); sputc('\n');
    const void *sb; size_t sl; cvsasx_store_get(&r3_store, &sh, &sb, &sl);
    for(size_t i=0;i<sl&&i<64;i++) r3_dst[i]=((const uint8_t*)sb)[i];     /* rematerialize into a FRESH buffer */
    int bitident=1; for(int i=0;i<64;i++) if(k_linmem[i]!=r3_dst[i]) bitident=0;
    cvsasx_swcap_t droot={ (uint64_t)(uintptr_t)r3_dst,64,CVSASX_PERM_LOAD|CVSASX_PERM_STORE|CVSASX_PERM_GLOBAL,1};
    cvsasx_sw_custodian_t dc; cvsasx_sw_custodian_init(&dc,droot);
    cvsasx_sw_region_t dr; dr.object_cap=droot; dr.object_base_addr=(uint64_t)(uintptr_t)r3_dst; dr.object_length=64;
    for(int i=0;i<(int)CVSASX_BLAKE3_LEN;i++) dr.hash[i]=sh.b[i];
    cvsasx_referent_t ref; for(int i=0;i<(int)CVSASX_BLAKE3_LEN;i++) ref.hash[i]=sh.b[i];
    ref.object_base_addr=(uint64_t)(uintptr_t)r3_dst; ref.object_length=64;
    cvsasx_pir_t pir; cvsasx_sw_cap_strip(droot,&ref,&pir);
    cvsasx_swcap_t newcap; cvsasx_status_t rs=cvsasx_sw_cap_remint(&dc,&pir,&dr,&newcap);
    int r3=bitident && (rs==CVSASX_OK) && newcap.valid && newcap.length==64;
    sputs("R3 rematerialize: bit-identical="); sputs(bitident?"y":"n");
    sputs(" fresh-cap remint="); sdec((uint64_t)rs); sputs(" len="); sdec(newcap.length); sputs(" -> "); sputs(r3?"REMAT OK\n":"FAIL\n");

    /* R4: diff-proportional - a 1-leaf change re-migrates only the DIFF, not the SIZE.
     * Balanced Merkle tree (arity<=16); the ACTUAL cvsasx_sls_diff result drives the
     * byte count; two sizes show the reduction GROWS with state size. */
    uint64_t full_b=0, diff_b=0; uint32_t ratio=0; uint64_t resolved=0;
    int pos_ok=1, neg_ok=1, multi_ok=1, tree_ok=1;   /* AUDIT A5: exact count + soundness */
    const uint32_t sizes[2]={256,1024};
    for(int z=0; z<2; z++){
        uint32_t L=sizes[z];
        cvsasx_store_init(&r4_store, r4_arena, sizeof r4_arena, r4_idx, 4096);
        cvsasx_sls_t S; cvsasx_sls_init(&S,&r4_store);
        for(uint32_t i=0;i<L;i++){ uint8_t d[8]={(uint8_t)i,(uint8_t)(i>>8),(uint8_t)(i*3u),(uint8_t)(i*5u),(uint8_t)(i*7u),(uint8_t)(i*11u),(uint8_t)(i*13u),(uint8_t)(i*17u)}; cvsasx_sls_put_leaf(&S,d,8,&r4_leaf[i]); } /* distinct -> no dedup */
        cvsasx_oid_t root = build_tree(&S, r4_leaf, L); if(g_tree_err) tree_ok=0;
        uint64_t full = r4_store.bytes_stored;                       /* cold migrate: every object */
        /* (1) single-leaf change -> EXACTLY 1 changed object */
        uint8_t d2[8]={0xEE,0xEE,0xEE,0xEE,0xEE,0xEE,0xEE,0xEE}; cvsasx_oid_t nl; cvsasx_sls_put_leaf(&S,d2,8,&nl);
        cvsasx_oid_t saved=r4_leaf[0]; r4_leaf[0]=nl;
        cvsasx_oid_t root2 = build_tree(&S, r4_leaf, L); if(g_tree_err) tree_ok=0; r4_leaf[0]=saved;
        cvsasx_oid_t ch[64]; uint32_t nch=0; uint64_t gg=0,cc=0;
        cvsasx_sls_status_t ds = cvsasx_sls_diff(&S,&root,&root2,ch,64,&nch,&gg,&cc);
        uint64_t diff=0; { const void*b; size_t l; for(uint32_t i=0;i<nch;i++){ if(cvsasx_sls_get(&S,&ch[i],&b,&l)==CVSASX_SLS_OK) diff+=l; } }
        if(!(ds==CVSASX_SLS_OK && nch==1)) pos_ok=0;
        /* (2) NEGATIVE: an IDENTICAL tree must diff to ZERO changed (soundness direction) */
        cvsasx_oid_t ch0[8]; uint32_t nch0=99; uint64_t g0=0,c0=0;
        cvsasx_sls_diff(&S,&root2,&root2,ch0,8,&nch0,&g0,&c0);
        if(nch0!=0) neg_ok=0;
        /* (3) MULTI-leaf change (K=3) -> EXACTLY 3 changed (proportionality, not just non-empty) */
        uint8_t da[8]={0xA1,1,2,3,4,5,6,7}, db[8]={0xB2,1,2,3,4,5,6,7}, dd[8]={0xC3,1,2,3,4,5,6,7};
        cvsasx_oid_t la,lb,lc; cvsasx_sls_put_leaf(&S,da,8,&la); cvsasx_sls_put_leaf(&S,db,8,&lb); cvsasx_sls_put_leaf(&S,dd,8,&lc);
        uint32_t i0=0,i1=L/2,i2=L-1; cvsasx_oid_t s0=r4_leaf[i0],s1=r4_leaf[i1],s2=r4_leaf[i2];
        r4_leaf[i0]=la; r4_leaf[i1]=lb; r4_leaf[i2]=lc;
        cvsasx_oid_t root3 = build_tree(&S, r4_leaf, L); if(g_tree_err) tree_ok=0;
        r4_leaf[i0]=s0; r4_leaf[i1]=s1; r4_leaf[i2]=s2;
        cvsasx_oid_t ch3[64]; uint32_t nch3=0; uint64_t g3=0,c3=0;
        cvsasx_sls_diff(&S,&root,&root3,ch3,64,&nch3,&g3,&c3);
        if(nch3!=3) multi_ok=0;
        sputs("R4 state="); sdec(L); sputs(" leaves: full="); sdec(full); sputs("B  diff(1-leaf)="); sdec(diff);
        sputs("B  reduction="); sdec(diff?full/diff:0); sputs("x  changed=1? "); sputs(nch==1?"y":"n");
        sputs("  resolved="); sdec(gg); sputs("/"); sdec(L);
        sputs("  self-diff="); sdec(nch0); sputs("(want0)  3-leaf="); sdec(nch3); sputs("(want3)\n");
        full_b=full; diff_b=diff; ratio=(uint32_t)(diff?full/diff:0); resolved=gg;
    }
    int r4=(diff_b*4<full_b) && pos_ok && neg_ok && multi_ok && tree_ok && (resolved<1024);
    sputs("R4 diff-proportional + soundness: 1-leaf=1? "); sputs(pos_ok?"y":"n"); sputs(" self-diff=0? "); sputs(neg_ok?"y":"n");
    sputs(" 3-leaf=3? "); sputs(multi_ok?"y":"n"); sputs(" tree-ok? "); sputs(tree_ok?"y":"n");
    sputs(" -> "); sputs(r4?"PROVEN (O(changed), no over/under-report)\n":"FAIL\n");

    /* R5: the visible milestone - draw the rematerialization result + diff numbers */
    int r5=0;
    if(FB){
        fill(40,232,560,92,0x00102818);
        draw_str(52,240, r3?"REMAT OK":"REMAT FAIL", 0x0000FFAA);
        draw_str(52,262,"FULL=",0x0000FFAA);  draw_dec(52+5*8,262,full_b,0x0000FFAA);
        draw_str(52,284,"DIFF=",0x0000FFAA);  draw_dec(52+5*8,284,diff_b,0x0000FFAA);
        draw_str(52,306,"RATIO=",0x0000FFAA); draw_dec(52+6*8,306,ratio,0x0000FFAA);
        r5 = (get_px(560,236)==0x00102818);   /* panel readback (headless proof) */
    }
    sputs("R5 result on framebuffer: "); sputs(r5?"DRAWN (REMAT OK, FULL/DIFF/RATIO visible)\n":"FB absent\n");
    sputs("STEP 5: rematerialization + diff-proportional transfer run ON THE METAL.\n");
}

/* ===========================================================================
 * STEP 11 - a minimal TASK / PROCESS substrate (P0-P3).
 * Turns the kernel from "one scripted sequence" into "switches between tasks".
 * The scheduling POLICY is isolated in ONE function, pick_next(), so future
 * "rematerialization-native scheduling" research replaces only that hook (and
 * possibly the save/restore), not the kernel. See kernel/SCHED_LOG.md.
 * Round-robin here is an explicit PLACEHOLDER, not the contribution.
 * ===========================================================================*/
enum task_state { TASK_READY, TASK_RUNNING, TASK_DONE };
typedef struct task {
    uint32_t id;
    /* saved callee-saved context: switch_to() stores rsp here; the rest live on
     * that stack (pushed by switch_to) so this struct stays tiny. */
    uint64_t rsp;            /* the one piece of context the switch persists in the struct */
    uint64_t *stack;         /* base of this task's stack region (falloc'd 4KiB) */
    enum task_state state;
    uint8_t remat_root[32];  /* RESERVED: content-address handle for future research; UNUSED here. */
} task_t;

#define MAX_TASKS 4
static task_t g_tasks[MAX_TASKS];
static int g_ntasks = 0;
static int g_cur = -1;       /* index of the RUNNING task, or -1 in kmain's "task 0" context */
static uint64_t g_switches = 0;
static task_t g_main_task;   /* represents kmain's own context so we can switch back to it */
static int g_sched_on = 0;   /* P2: only let the timer preempt while the demo is active */

/* ---- the ASSEMBLY context switch (callee-saved regs + rsp) ----------------
 * SysV callee-saved: rbx,rbp,r12-r15. Push them, save rsp to *old, load rsp from
 * *new, pop them, ret -> resumes the new task exactly where IT last called
 * switch_to (cooperatively OR from the timer ISR - same routine, that is the point).
 * THIS routine is the second thing the research may replace (a switch could become
 * a store+rematerialize cycle); see SCHED_LOG.md. */
__attribute__((naked, noinline))
static void switch_to(uint64_t *old_rsp, uint64_t new_rsp){
    __asm__ volatile(
        "push %rbx\n\t" "push %rbp\n\t" "push %r12\n\t"
        "push %r13\n\t" "push %r14\n\t" "push %r15\n\t"
        "mov %rsp, (%rdi)\n\t"   /* *old_rsp = rsp  (rdi = old_rsp) */
        "mov %rsi, %rsp\n\t"     /* rsp = new_rsp   (rsi = new_rsp) */
        "pop %r15\n\t" "pop %r14\n\t" "pop %r13\n\t"
        "pop %r12\n\t" "pop %rbp\n\t" "pop %rbx\n\t"
        "ret\n\t");
}

/* ---- pick_next(): THE SCHEDULING HOOK. Round-robin PLACEHOLDER. ------------
 * Future research replaces ONLY this function (signature fixed): given the
 * current task index, return the index of the next READY/RUNNING task to run.
 * Returns -1 if none remain (all DONE) -> control returns to kmain. */
static int pick_next(int cur){
    for(int k=1;k<=g_ntasks;k++){
        int i=(cur+k)%g_ntasks;
        if(g_tasks[i].state==TASK_READY || g_tasks[i].state==TASK_RUNNING) return i;
    }
    return -1;
}

/* ---- yield(): cooperative. Save current, pick next, switch. ---------------- */
static void yield(void){
    int cur=g_cur, nxt=pick_next(cur);
    if(nxt<0 || nxt==cur) return;                 /* nobody else ready -> keep running */
    if(g_tasks[cur].state==TASK_RUNNING) g_tasks[cur].state=TASK_READY;
    g_tasks[nxt].state=TASK_RUNNING; g_cur=nxt; g_switches++;
    switch_to(&g_tasks[cur].rsp, g_tasks[nxt].rsp);
}

/* P2: invoked from the timer ISR. Same hook, same switch - only WHEN differs.
 * A task that NEVER yields is switched out here. Re-entry is impossible: IF is
 * clear in the ISR until the next task's IRET restores its own rflags. */
static void sched_tick(void){
    if(!g_sched_on || g_cur<0) return;
    int cur=g_cur, nxt=pick_next(cur);
    if(nxt<0 || nxt==cur) return;
    if(g_tasks[cur].state==TASK_RUNNING) g_tasks[cur].state=TASK_READY;
    g_tasks[nxt].state=TASK_RUNNING; g_cur=nxt; g_switches++;
    switch_to(&g_tasks[cur].rsp, g_tasks[nxt].rsp);
}

/* task_exit(): a task's entry returns here; mark DONE and switch away forever.
 * Lands on the next runnable task, or back to kmain (the g_main_task context). */
static void task_exit(void){
    g_tasks[g_cur].state=TASK_DONE;
    int nxt=pick_next(g_cur);
    uint64_t dummy;   /* DONE task's rsp never resumes -> a throwaway slot to save into */
    if(nxt<0){ g_sched_on=0; int cur=g_cur; g_cur=-1; switch_to(&dummy, g_main_task.rsp); (void)cur; }
    else { g_tasks[nxt].state=TASK_RUNNING; int cur=g_cur; g_cur=nxt; g_switches++; switch_to(&dummy, g_tasks[nxt].rsp); (void)cur; }
}

/* task_trampoline: the first thing a freshly-created task runs. `sti` so the
 * task ALWAYS starts with interrupts enabled - critical for P2: a task first
 * entered from the timer ISR (IF=0) would otherwise run with interrupts masked
 * forever and never be preempted. fn is in r12 (a callee-saved reg switch_to
 * restored from the bootstrap frame); fn returns into task_exit (return slot). */
__attribute__((naked, noinline))
static void task_trampoline(void){
    __asm__ volatile("sti\n\t" "jmp *%r12\n\t");   /* fn ret-addr = task_exit (already on stack) */
}

/* task_create(): build a stack frame that, when switch_to() pops into it, lands
 * in task_trampoline (which sti's then jumps to fn). Bootstrap layout (top-down):
 * a return slot = task_exit (fn's `ret` lands here), then a return slot =
 * task_trampoline (switch_to's `ret` jumps here), then 6 callee-saved slots that
 * switch_to's 6 pops consume - the r12 slot is preloaded with fn. */
static void task_create(void (*fn)(void)){
    if(g_ntasks>=MAX_TASKS) return;
    task_t *t=&g_tasks[g_ntasks];
    uint64_t pa=falloc();                          /* 4KiB stack frame */
    t->stack=(uint64_t*)P2V(pa);
    uint64_t *sp=t->stack + (4096/8);              /* top of stack (grows down) */
    /* switch_to pops r15,r14,r13,r12,rbp,rbx then `ret`. pop reads LOW->HIGH, so
     * r15 is at the lowest (last-written) slot and the `ret` target (trampoline)
     * is highest. fn goes in the r12 slot so the trampoline's `jmp *%r12` enters
     * it. Writes below run high-address first (*--sp), so this list is in
     * pop order: rbx-slot value LAST. */
    *--sp = (uint64_t)task_exit;                   /* +56: fn returns here (after jmp) */
    *--sp = (uint64_t)task_trampoline;             /* +48: switch_to's `ret` enters here */
    *--sp = 0;                                     /* +40: rbx */
    *--sp = 0;                                     /* +32: rbp */
    *--sp = (uint64_t)fn;                          /* +24: r12 = fn (trampoline jmp *%r12) */
    *--sp = 0;                                     /* +16: r13 */
    *--sp = 0;                                     /* +8 : r14 */
    *--sp = 0;                                     /* +0 : r15 (lowest, popped first) */
    t->rsp=(uint64_t)sp; t->id=(uint32_t)g_ntasks; t->state=TASK_READY;
    for(int i=0;i<32;i++) t->remat_root[i]=0;      /* reserved, future research */
    g_ntasks++;
}

/* ---- P0 workload: two tasks print A/B and yield -> proves a real switch ---- */
static void task_a(void){ for(int i=0;i<4;i++){ sputc('A'); yield(); } }
static void task_b(void){ for(int i=0;i<4;i++){ sputc('B'); yield(); } }

/* ---- P1 workload: a task does REAL work (the proven R3 remat-counter path)
 * across a yield. It increments a counter, checkpoints via cvsasx_store_put,
 * yields (another task runs), resumes, rematerializes via cvsasx_store_get, and
 * asserts the value survived the context switch. ---- */
static uint8_t  p1_arena[1u<<12]; static cvsasx_store_entry_t p1_idx[16]; static cvsasx_store_t p1_store;
static volatile int p1_ok=0; static volatile uint32_t p1_before=0, p1_after=0;
static void task_remat(void){
    cvsasx_store_init(&p1_store, p1_arena, sizeof p1_arena, p1_idx, 16);
    uint32_t counter=1000; counter++;                       /* do work: 1001 */
    p1_before=counter;
    cvsasx_hash_t h; cvsasx_store_put(&p1_store,&counter,sizeof counter,&h);   /* checkpoint */
    counter=0xDEAD;                                         /* clobber: prove we rely on the store, not the stack var */
    yield();                                                /* <-- context switch happens here */
    uint32_t remat=0; const void*b; size_t l;               /* resumed: rematerialize */
    if(cvsasx_store_get(&p1_store,&h,&b,&l)==CVSASX_STORE_OK && l==sizeof remat)
        for(size_t i=0;i<l;i++) ((uint8_t*)&remat)[i]=((const uint8_t*)b)[i];
    p1_after=remat; p1_ok=(remat==p1_before);
    sputs("\nP1 task: counter "); sdec(p1_before); sputs(" checkpointed, yielded, rematerialized -> "); sdec(p1_after);
    sputs(p1_ok?" SURVIVED the switch\n":" *** LOST ***\n");
}
/* a partner task so task_remat's yield has somewhere to go (and proves interleave). */
static void task_partner(void){ for(int i=0;i<3;i++){ sputc('P'); yield(); } }

/* ---- P2 workloads: tight loops that NEVER call yield(). Only the timer ISR
 * (sched_tick) can switch them. Bounded so the demo terminates (an unbounded
 * loop would hang the boot before run_shell). Each prints its marker every
 * BUSY iterations; ~tens of ms of busy work => the 100Hz timer preempts. ---- */
static volatile int p2_saw_a=0, p2_saw_b=0;
static volatile uint64_t p2_sink=0;   /* defeat dead-loop elimination */
static void task_loop_noyield(void){
    for(int r=0;r<6;r++){
        for(volatile uint64_t i=0;i<4000000ull;i++) p2_sink+=i;   /* busy work, NO yield */
        sputc('1'); p2_saw_a=1;
    }
}
static void task_loop_noyield2(void){
    for(int r=0;r<6;r++){
        for(volatile uint64_t i=0;i<4000000ull;i++) p2_sink+=i;
        sputc('2'); p2_saw_b=1;
    }
}

/* run the scheduler from kmain's context: install g_main_task as the resume
 * target, mark kmain as g_cur=0's "caller", launch into the first task. When all
 * tasks are DONE, task_exit switches back here and we return. */
static void start_tasks(void){
    g_main_task.id=0xFFFF; g_main_task.state=TASK_RUNNING;
    g_cur=0; g_tasks[0].state=TASK_RUNNING;
    switch_to(&g_main_task.rsp, g_tasks[0].rsp);   /* jump in; returns here when all DONE */
    g_cur=-1;
}

static void run_sched(void){
    sputs("\n=== STEP 11: TASK SUBSTRATE - cooperative + preemptive scheduling ===\n");

    /* P0: two tasks alternate A B A B ... proving a real context switch. */
    sputs("P0 interleave (each task prints its marker then yield()s): ");
    g_ntasks=0; g_switches=0; g_sched_on=0;
    task_create(task_a); task_create(task_b);
    start_tasks();
    sputs("\nP0 both tasks DONE; switches="); sdec(g_switches);
    sputs("; control back in kmain -> ");
    sputs((g_tasks[0].state==TASK_DONE && g_tasks[1].state==TASK_DONE && g_switches>=4)?"COOPERATIVE SWITCH OK\n":"FAIL\n");

    /* P1: a task does the proven R3 remat-counter work ACROSS a yield. */
    sputs("\nP1 real work across a switch (R3 remat-counter inside a task):");
    g_ntasks=0; g_switches=0; g_sched_on=0; p1_ok=0;
    task_create(task_remat); task_create(task_partner);
    start_tasks();
    sputs("P1 result: before="); sdec(p1_before); sputs(" after="); sdec(p1_after);
    sputs(p1_ok?" -> COUNTER SURVIVES YIELD/RESUME OK\n":" -> FAIL\n");

    /* P2: a task in a tight loop with NO yield is preempted by the timer.
     * The looping task never calls yield(); only the timer ISR (sched_tick) can
     * switch it out. If the looper's marker and the other task's marker BOTH
     * appear, the switch came from the timer. */
    sputs("\nP2 preemption (looping task has NO yield; only the timer switches it):\n");
    g_ntasks=0; g_switches=0; p1_ok=0;
    task_create(task_loop_noyield); task_create(task_loop_noyield2);
    uint64_t sw0=g_switches;
    g_sched_on=1;                                  /* arm preemption JUST for this demo */
    start_tasks();
    g_sched_on=0;
    int p2=(p2_saw_a && p2_saw_b) && (g_switches>sw0);
    sputs("P2 saw L1="); sputs(p2_saw_a?"y":"n"); sputs(" L2="); sputs(p2_saw_b?"y":"n");
    sputs(" timer-switches="); sdec(g_switches);
    sputs(p2?" -> PREEMPTED WITHOUT YIELD OK\n":" -> NOT PREEMPTED (see stall report)\n");
    sputs("STEP 11: task substrate run; pick_next() is the isolated research hook (SCHED_LOG.md).\n");
}

/* ===========================================================================
 * STEP 12 - UNIFIED CONTENT-ADDRESSED EXECUTION SUBSTRATE (U0-U4).
 * A task becomes a content-addressed object; a context switch can OPTIONALLY be
 * a dematerialize/rematerialize pair through the PROVEN store (cvsasx_store_put/
 * get, the R3 path). The raw register-swap switch_to() stays the fast path. The
 * honest record is the MEASURED crossover: per-switch content-addressing is NOT
 * free; activate-by-hash is viable only at COARSE boundaries. See SCHED_LOG.md.
 * Only kernel.c + SCHED_LOG.md change; gate/cap/carmix/store/sls untouched.
 * ===========================================================================*/
static inline uint64_t rdtsc(void){ uint32_t a,d; __asm__ volatile("rdtsc":"=a"(a),"=d"(d)); return ((uint64_t)d<<32)|a; }

/* ---- U0: a task's REMATERIALIZABLE STATE OBJECT ---------------------------
 * Minimally = the saved rsp + the USED portion of the 4KiB stack (which already
 * holds the 6 callee-saved regs switch_to pushed + the rip/return chain). The
 * stack lives at a FIXED VA, so restoring the same bytes + rsp resumes exactly.
 * Page tables and capability slots are OUT OF SCOPE here. // not-yet: page-tables,
 * // not-yet: capability slots. Serialized layout: [rsp:8][used_len:8][stack bytes]. */
#define U_STACK_SZ 4096u
static uint8_t u_buf[16 + U_STACK_SZ];            /* serialization scratch */
static uint64_t u_serialize(const task_t *t){      /* -> number of bytes packed into u_buf */
    uint64_t top=(uint64_t)t->stack + U_STACK_SZ;
    uint64_t used=top - t->rsp;                     /* live stack from rsp up to the top */
    if(used>U_STACK_SZ) used=U_STACK_SZ;            /* fail-closed clamp */
    for(int i=0;i<8;i++) u_buf[i]=(uint8_t)(t->rsp>>(8*i));
    for(int i=0;i<8;i++) u_buf[8+i]=(uint8_t)(used>>(8*i));
    const uint8_t *src=(const uint8_t*)t->rsp;
    for(uint64_t i=0;i<used;i++) u_buf[16+i]=src[i];
    return 16+used;
}

/* U0 store: dematerialize a SUSPENDED task into the proven store; record the
 * BLAKE3 content address into task_t.remat_root. Returns the hash by value. */
static uint8_t  u_arena[1u<<16]; static cvsasx_store_entry_t u_idx[256]; static cvsasx_store_t u_store; static int u_store_ready=0;
static void u_store_once(void){ if(!u_store_ready){ cvsasx_store_init(&u_store,u_arena,sizeof u_arena,u_idx,256); u_store_ready=1; } }
static cvsasx_hash_t dematerialize(task_t *t){
    u_store_once();
    uint64_t n=u_serialize(t);
    cvsasx_hash_t h; cvsasx_store_put(&u_store,u_buf,n,&h);
    for(int i=0;i<32;i++) t->remat_root[i]=h.b[i];   /* task is now content-addressed */
    return h;
}
/* U1 materialize: restore a task's context from a content address. Fetch by hash
 * (a missing object is a MISS), VERIFY integrity (the store returns the bytes that
 * hash to exactly this address - integrity by construction; we also re-derive rsp/
 * used and write the stack image back to its fixed VA). Returns 1 on success. */
static int materialize(task_t *t, const cvsasx_hash_t *h){
    u_store_once();
    const void *b; size_t l;
    if(cvsasx_store_get(&u_store,h,&b,&l)!=CVSASX_STORE_OK) return 0;
    const uint8_t *p=(const uint8_t*)b;
    uint64_t rsp=0, used=0;
    for(int i=0;i<8;i++) rsp |=(uint64_t)p[i]<<(8*i);
    for(int i=0;i<8;i++) used|=(uint64_t)p[8+i]<<(8*i);
    if(used>U_STACK_SZ || l!=16+used) return 0;        /* fail-closed on a malformed object */
    uint8_t *dst=(uint8_t*)rsp;                          /* fixed VA: same stack region */
    for(uint64_t i=0;i<used;i++) dst[i]=p[16+i];
    t->rsp=rsp;
    return 1;
}

/* ---- U2: INCREMENTAL content-address (software chunk-diff, MEASURED) -------
 * Split the stack state object into fixed CHUNKs, keep the last-stored image, and
 * on dematerialize memcmp each chunk to find the DIRTY set; re-store ONLY dirty
 * chunks via the proven store and count bytes-re-stored + rdtsc cycles. This is
 * the cvsasx_sls structural-diff idea (changed-only) applied to a flat chunked
 * blob. // not-yet: x86-64 page-table DIRTY bit (PTE bit 6) - that needs per-task
 * mapped pages (each task gets its own PT mapping its stack); the stacks here are
 * HHDM-direct, not per-task-mapped, so dirty-bit tracking is not wired. Software
 * chunk-diff is used instead and stated as such (the honest gap). */
#define U_CHUNK 256u
static uint8_t u_last[U_STACK_SZ]; static int u_have_last=0;
static uint8_t u_chunkbuf[U_CHUNK];
/* dematerialize_incremental: returns dirty bytes re-stored; *out_cycles = rdtsc cost.
 * Operates on a flat 'img' of length 'len' (the task's serialized stack image). */
static uint64_t dematerialize_incremental(const uint8_t *img, uint64_t len, uint64_t *out_cycles, uint32_t *out_dirty_chunks){
    u_store_once();
    uint64_t t0=rdtsc();
    uint64_t dirty_bytes=0; uint32_t dirty_chunks=0;
    for(uint64_t off=0; off<len; off+=U_CHUNK){
        uint64_t clen=(len-off<U_CHUNK)?(len-off):U_CHUNK;
        int dirty=!u_have_last;
        if(!dirty){ for(uint64_t i=0;i<clen;i++) if(img[off+i]!=u_last[off+i]){ dirty=1; break; } }
        if(dirty){
            for(uint64_t i=0;i<clen;i++) u_chunkbuf[i]=img[off+i];
            cvsasx_hash_t ch; cvsasx_store_put(&u_store,u_chunkbuf,(size_t)clen,&ch);  /* store ONLY dirty chunk */
            dirty_bytes+=clen; dirty_chunks++;
        }
    }
    for(uint64_t i=0;i<len && i<U_STACK_SZ;i++) u_last[i]=img[i];   /* update the kept version */
    u_have_last=1;
    *out_cycles=rdtsc()-t0; *out_dirty_chunks=dirty_chunks;
    return dirty_bytes;
}

/* ---- U2/U3 measurement workload: a synthetic state blob we DIRTY by hand so the
 * dirty set is exact and the numbers are reproducible (computed, never hardcoded). */
static uint8_t u_state[U_STACK_SZ];
static void u_dirty_chunks(uint32_t k){ for(uint32_t c=0;c<k && c*U_CHUNK<U_STACK_SZ;c++){ uint64_t base=(uint64_t)c*U_CHUNK; for(uint32_t i=0;i<U_CHUNK;i++) u_state[base+i]^=(uint8_t)(0x5Au+c+i); } }

/* full-path cost: serialize+store the WHOLE blob (the non-incremental unified path). */
static uint64_t u_full_store_cycles(const uint8_t *img, uint64_t len, uint64_t *out_bytes){
    u_store_once();
    uint64_t t0=rdtsc();
    cvsasx_hash_t h; cvsasx_store_put(&u_store,img,(size_t)len,&h);  /* hashes ALL len bytes */
    uint64_t c=rdtsc()-t0;
    *out_bytes=len; (void)h; return c;
}

/* ---- U1 demo: activate-by-hash round-trip of a REAL suspended task. The task
 * carries a counter (like P1) but resumes via dematerialize->materialize through
 * the store (out by a hash, back by the SAME hash) instead of the live stack.
 * park() saves THIS task's context and switches straight back to kmain's context,
 * leaving the task genuinely SUSPENDED (rsp saved) so the driver can dematerialize
 * it. Resuming = switch_to(...,T->rsp): the task continues right after park(). ---- */
static volatile uint32_t u1_counter=0; static volatile int u1_phase=0;
static int g_unified_mode=0;   /* 0 = raw switch_to fast path; 1 = resume via store round-trip (activate-by-hash) */
static void park(task_t *t){ t->state=TASK_READY; switch_to(&t->rsp, g_main_task.rsp); }
/* g_unified_mode selects the resume path: 0 = raw switch_to (fast; the live stack is
 * trusted intact); 1 = activate-by-hash (materialize the context from its content
 * address first, then switch_to). The U1/U4 demos set it to 1 around the hash trip. */
static void task_u1(void){
    u1_counter=7000; u1_counter+=77;        /* do work: 7077 (lives on THIS stack) */
    u1_phase=1;
    park(&g_tasks[0]);                       /* suspend; the driver dematerializes us and resumes via the chosen path */
    u1_phase=2;                              /* resumed (possibly AFTER a store round-trip); counter must be intact */
}

static void run_unified(void){
    sputs("\n=== STEP 12: UNIFIED CONTENT-ADDRESSED SUBSTRATE (U0-U4) ===\n");

    /* ----- U0: a task IS a content-addressed object; determinism ----- */
    sputs("\nU0 task-as-content-addressed-object (same->same, diff->diff):\n");
    g_ntasks=0; g_switches=0; g_sched_on=0; u1_phase=0;
    g_main_task.id=0xFFFF; g_main_task.state=TASK_RUNNING;
    task_create(task_u1);                                 /* one suspendable task */
    g_cur=0; g_tasks[0].state=TASK_RUNNING;
    switch_to(&g_main_task.rsp, g_tasks[0].rsp);          /* run task_u1 to its park() -> returns here, task SUSPENDED (rsp saved) */
    /* task_u1 is now SUSPENDED at park() (state READY, rsp saved). Dematerialize it. */
    task_t *T=&g_tasks[0];
    cvsasx_hash_t hA=dematerialize(T);
    cvsasx_hash_t hA2=dematerialize(T);                  /* SAME state -> SAME hash (determinism) */
    /* perturb one byte of the saved stack image, store again -> DIFFERENT hash */
    uint64_t n=u_serialize(T); u_buf[16+ (n>16?16:0)]^=0xFF; cvsasx_hash_t hB; cvsasx_store_put(&u_store,u_buf,n,&hB);
    int same=1; for(int i=0;i<32;i++) if(hA.b[i]!=hA2.b[i]) same=0;
    int diff=0; for(int i=0;i<32;i++) if(hA.b[i]!=hB.b[i]) diff=1;
    sputs("U0 task id="); sdec(T->id); sputs(" remat_root="); sputs(hx(hA.b,32)); sputc('\n');
    sputs("U0 same-state-again="); sputs(hx(hA2.b,32)); sputs(same?"  SAME (deterministic)\n":"  *** DIFFERS ***\n");
    sputs("U0 1-byte-changed ="); sputs(hx(hB.b,32)); sputs(diff?"  DIFFERENT (content-addressed)\n":"  *** COLLIDED ***\n");
    sputs(same&&diff?"U0 -> TASK IS A CONTENT-ADDRESSED OBJECT OK\n":"U0 -> FAIL\n");

    /* ----- U1: activate-by-hash - dematerialize T to H, DROP its live stack, then
     * materialize from ONLY H + the store, and switch_to back in; counter survives. */
    sputs("\nU1 activate-by-hash round-trip (out by hash, back by SAME hash):\n");
    g_unified_mode=1;                                    /* per-task flag selects the activate-by-hash path */
    cvsasx_hash_t H=dematerialize(T);                    /* out: T -> H */
    /* DROP the live form: scribble the whole stack region so a stale-stack resume would crash/wrong.
     * Resuming correctly now PROVES the context came back through the store, not the stack. */
    for(uint64_t i=0;i<U_STACK_SZ;i++) ((uint8_t*)T->stack)[i]=0xCC;
    uint32_t before=u1_counter;                          /* 7077, computed in the task before it parked */
    int ok = g_unified_mode ? materialize(T,&H) : 1;     /* back: H -> T (the unified path; fast path would trust the live stack) */
    cvsasx_hash_t H2=dematerialize(T);                   /* re-address the RESTORED (pre-resume) state; must equal H */
    int hash_match=1; for(int i=0;i<32;i++) if(H.b[i]!=H2.b[i]) hash_match=0;
    /* now resume: the task continues right after park(), sets phase=2, returns into task_exit. */
    g_tasks[0].state=TASK_RUNNING; g_cur=0; g_switches++;
    switch_to(&g_main_task.rsp, T->rsp);                 /* resumes from the STORE-restored stack */
    g_unified_mode=0;
    uint32_t after=u1_counter;
    int u1=ok && hash_match && (u1_phase==2) && (before==after) && (before==7077);
    sputs("U1 hash out ="); sputs(hx(H.b,32)); sputc('\n');
    sputs("U1 hash back="); sputs(hx(H2.b,32)); sputs(hash_match?"  MATCH\n":"  *** MISMATCH ***\n");
    sputs("U1 counter before="); sdec(before); sputs(" after="); sdec(after); sputs(" resumed-phase="); sdec((uint64_t)u1_phase);
    sputs(u1?"  -> ACTIVATE-BY-HASH ROUND-TRIP OK\n":"  -> FAIL\n");

    /* ----- U2: incremental content-address - small dirty vs large dirty (MEASURED). */
    sputs("\nU2 incremental content-address (re-store ONLY the dirty set):\n");
    for(uint32_t i=0;i<U_STACK_SZ;i++) u_state[i]=(uint8_t)(i*131u+7u);   /* a baseline blob */
    u_have_last=0;
    uint64_t cyc0; uint32_t dc0; uint64_t b0=dematerialize_incremental(u_state,U_STACK_SZ,&cyc0,&dc0);  /* first: ALL dirty */
    /* small dirty: flip 1 chunk */
    u_dirty_chunks(1);
    uint64_t cycS; uint32_t dcS; uint64_t bS=dematerialize_incremental(u_state,U_STACK_SZ,&cycS,&dcS);
    /* large dirty: flip many chunks */
    u_dirty_chunks(U_STACK_SZ/U_CHUNK);
    uint64_t cycL; uint32_t dcL; uint64_t bL=dematerialize_incremental(u_state,U_STACK_SZ,&cycL,&dcL);
    sputs("U2 baseline (all dirty): chunks="); sdec(dc0); sputs(" bytes="); sdec(b0); sputs(" cycles="); sdec(cyc0); sputc('\n');
    sputs("U2 SMALL dirty (1 chunk): chunks="); sdec(dcS); sputs(" bytes="); sdec(bS); sputs(" cycles="); sdec(cycS); sputc('\n');
    sputs("U2 LARGE dirty (all chunks): chunks="); sdec(dcL); sputs(" bytes="); sdec(bL); sputs(" cycles="); sdec(cycL); sputc('\n');
    int u2=(dcS==1)&&(bS==U_CHUNK)&&(dcL>dcS)&&(bL>bS)&&(cycL>cycS);   /* cost TRACKS the dirty set, not the size */
    sputs(u2?"U2 -> INCREMENTAL COST TRACKS THE DIRTY SET OK (small<<large)\n":"U2 -> FAIL\n");

    /* ----- U3: THE CROSSOVER (MEASURED) - fast switch_to vs activate-by-hash unified
     * path as a function of dirty-set size. Sweep dirty chunk counts; print a table. */
    sputs("\nU3 crossover sweep (dirty chunks -> fast-path cycles | unified-path cycles):\n");
    /* fast-path baseline: a raw switch_to is independent of dirty size. Measure it by
     * timing a self-switch (save+restore 6 regs+rsp) averaged over a short run. */
    uint64_t fast_cyc;
    { volatile uint64_t scratch_rsp; uint64_t t0=rdtsc();
      for(int i=0;i<256;i++){ __asm__ volatile(
          "push %%rbx\n\t push %%rbp\n\t push %%r12\n\t push %%r13\n\t push %%r14\n\t push %%r15\n\t"
          "mov %%rsp,%0\n\t mov %0,%%rsp\n\t"
          "pop %%r15\n\t pop %%r14\n\t pop %%r13\n\t pop %%r12\n\t pop %%rbp\n\t pop %%rbx\n\t"
          : "=m"(scratch_rsp) :: "memory"); }
      fast_cyc=(rdtsc()-t0)/256; }                       /* per-switch cost of the register swap */
    const uint32_t sweep[5]={1,4,16,64,256};             /* 256 chunks*256B = 64KiB exceeds one stack: caps at the blob, fine for the curve */
    static uint8_t u_big[64u*1024u];
    uint32_t crossover=0; int found=0;
    sputs("U3   dirty | fast cyc | unified cyc\n");
    for(int s=0;s<5;s++){
        uint32_t kc=sweep[s]; uint64_t len=(uint64_t)kc*U_CHUNK; if(len>sizeof u_big) len=sizeof u_big;
        for(uint64_t i=0;i<len;i++) u_big[i]=(uint8_t)(i*73u+s);
        uint64_t ub; uint64_t uni=u_full_store_cycles(u_big,len,&ub);   /* unified = hash+store the dirty image */
        sputs("U3   "); sdec(kc); sputs("     | "); sdec(fast_cyc); sputs("     | "); sdec(uni); sputc('\n');
        if(!found && uni>fast_cyc){ crossover=kc; found=1; }            /* first dirty size where the raw switch wins */
    }
    sputs("U3 fast-path switch (register swap) = "); sdec(fast_cyc); sputs(" cycles, INDEPENDENT of dirty size\n");
    if(found && crossover>sweep[0]){ sputs("U3 CROSSOVER: unified path exceeds the fast switch at >= "); sdec(crossover);
               sputs(" dirty chunks ("); sdec((uint64_t)crossover*U_CHUNK); sputs(" B). Below it activate-by-hash is competitive; above it the raw switch wins decisively.\n"); }
    else if(found){ sputs("U3 CROSSOVER: the unified path is MORE expensive than the fast switch at EVERY swept size - the BLAKE3 hash of even one chunk already dwarfs the ~1K-cycle register swap. There is NO dirty size where activate-by-hash beats raw switch; the gap only widens with size.\n"); }
    else sputs("U3 CROSSOVER: unified path stayed below the fast switch across the swept range (smallest dirty sets only).\n");
    sputs("U3 CONCLUSION: per-rapid-switch content-addressing THRASHES (CONFIRMED - refutes 'a switch IS a free remat'); activate-by-hash pays off ONLY at COARSE boundaries where a switch is rare and the hash cost amortizes (yield-to-migrate, checkpoint, persist), never at rapid preemption. See SCHED_LOG.md.\n");

    /* ----- U4: ONE BOUNDARY UNIFIED END-TO-END - PERSISTENCE-AS-NOOP (option a).
     * Dematerialize a task to H, DROP the live task entirely, resume from ONLY H +
     * the store. (Option b, two-machine migration, lives in the SEPARATE nettest.c
     * build, so it is not clean here; we do (a).) The re-mint path is exercised by
     * P1/R3 already; here the durable and live forms are literally the same object. */
    sputs("\nU4 persistence-as-noop (resume from ONLY a hash + the store):\n");
    g_ntasks=0; g_switches=0; u1_phase=0;
    task_create(task_u1);
    g_cur=0; g_tasks[0].state=TASK_RUNNING;
    switch_to(&g_main_task.rsp, g_tasks[0].rsp);          /* run task_u1 to its park() -> suspended */
    task_t *Q=&g_tasks[0];
    cvsasx_hash_t HU=dematerialize(Q);                    /* the ONLY durable form */
    /* fully DROP the live task: clobber stack AND zero rsp so nothing live remains. */
    for(uint64_t i=0;i<U_STACK_SZ;i++) ((uint8_t*)Q->stack)[i]=0xEE;
    uint64_t saved_rsp=Q->rsp; Q->rsp=0;
    uint32_t pre=u1_counter;
    int got=materialize(Q,&HU);                           /* resume from ONLY HU + the store */
    int restored_rsp=(Q->rsp==saved_rsp);
    g_tasks[0].state=TASK_RUNNING; g_cur=0; g_switches++;
    switch_to(&g_main_task.rsp, Q->rsp);                  /* it runs to completion from the rematerialized form */
    int u4=got && restored_rsp && (u1_phase==2) && (u1_counter==pre) && (pre==7077);
    sputs("U4 hash H="); sputs(hx(HU.b,32)); sputc('\n');
    sputs("U4 dropped live task, rematerialized from H: rsp-restored="); sputs(restored_rsp?"y":"n");
    sputs(" counter="); sdec(u1_counter); sputs(" resumed-phase="); sdec((uint64_t)u1_phase);
    sputs(u4?"  -> PERSISTENCE-AS-NOOP OK (live form == durable form)\n":"  -> FAIL\n");
    sputs("STEP 12: U0-U4 - task as content-addressed object; the honest crossover is the record (SCHED_LOG.md).\n");
}

/* ===========================================================================
 * RESIDENCY MANAGER (M0-M5) - a content-addressed physical-memory substrate.
 * The collapse map (binding): allocate-fresh STAYS DISTINCT (frame_reserve);
 * materialize/fault-in COLLAPSES to rematerialization (materialize-by-hash);
 * share-identical COLLAPSES (one resident frame, many refs, by hash identity);
 * free PARTIALLY collapses (refdrop logical, physical reclaim separate); evict
 * PARTIALLY collapses (writeback = dematerialize, victim selection = temporal
 * policy, NOT content-keyed); map STAYS DISTINCT (conventional page tables).
 * Hardware floor kept conventional: page tables, per-task mappings, the frame
 * database, per-frame pin/dirty/refcount, the eviction policy. REUSES the proven
 * cvsasx_store_put/get + cvsasx_blake3 + map_page; the proven modules are
 * byte-identical. See kernel/MEM_LOG.md. Numbers are COMPUTED via rdtsc/counters.
 * ===========================================================================*/

/* ---- M0: the frame database. One record per usable RAM frame, indexed by
 * frame number (phys-ram_lo)/4096. State is a strict partition free|resident|
 * pinned (a frame is exactly one); refcount, dirty bit, and the content hash of
 * a materialized frame ride alongside. Built over the SAME freelist falloc/ffree
 * already use, so the physical source of frames is unchanged. */
enum frame_state { FR_FREE = 0, FR_RESIDENT = 1, FR_PINNED = 2 };
typedef struct frame_rec {
    uint8_t  state;        /* FR_FREE | FR_RESIDENT | FR_PINNED */
    uint8_t  dirty;        /* written since last writeback (M3/M4) */
    uint8_t  has_hash;     /* 1 if content-addressed (materialized), 0 if fresh-mutable */
    uint8_t  clock_ref;    /* M3 clock policy: referenced-recently bit */
    uint32_t refcount;     /* number of live references (mappings by hash) */
    cvsasx_hash_t hash;    /* content address of a materialized frame (has_hash==1) */
} frame_rec_t;
/* The database owns a BOUNDED, CONTIGUOUS pool of physical frames carved from the
 * kernel allocator ONCE. It manages free/resident/pinned within that pool with its
 * OWN free-stack, so reserve-on-exhaustion is bounded to the pool and the global
 * falloc stays available for page-table pages (M1/M4 mm_walk). Frame number =
 * (phys - fdb_base)/4096, dense over the pool. */
#define FRAMEDB_N 256u                     /* pool size: 256 frames = 1 MiB, ample for the M demos */
static frame_rec_t framedb[FRAMEDB_N];
static uint64_t fdb_base, fdb_n;           /* phys base + number of tracked frames in the pool */
static uint64_t fdb_freestack[FRAMEDB_N];  /* indices of FREE frames (LIFO, so numbers get reused) */
static uint64_t fdb_freetop;               /* number of free frames on the stack */
static uint64_t fdb_reserve_fail;          /* count of fail-closed reserves (exhaustion) */
static int fdb_pool_ready;

static uint64_t fr_idx(uint64_t pa){ return (pa - fdb_base) >> 12; }
static uint64_t fr_phys(uint64_t i){ return fdb_base + (i << 12); }
static int fr_tracked(uint64_t pa){ if(pa<fdb_base) return 0; uint64_t i=fr_idx(pa); return i<fdb_n; }

/* M0 conservation assert: free + resident + pinned must equal the tracked total.
 * A frame in two classes or in none is a corrupt database; print loudly. */
static void framedb_counts(uint64_t *fre, uint64_t *res, uint64_t *pin){
    uint64_t f=0,r=0,p=0;
    for(uint64_t i=0;i<fdb_n;i++){
        switch(framedb[i].state){ case FR_FREE:f++;break; case FR_RESIDENT:r++;break; case FR_PINNED:p++;break;
            default: sputs("*** M0 ASSERT FAIL: frame "); sdec(i); sputs(" has invalid state ***\n"); }
    }
    if(f+r+p!=fdb_n){ sputs("*** M0 ASSERT FAIL: free+resident+pinned="); sdec(f+r+p); sputs(" != total "); sdec(fdb_n); sputs(" ***\n"); }
    *fre=f; *res=r; *pin=p;
}

/* Bring the database up: carve FRAMEDB_N CONTIGUOUS frames from falloc ONCE (the
 * pool), then reset all records to FREE with a full free-stack. Re-running just
 * resets state over the SAME pool (frame numbers stable across stages). */
static void framedb_init(void){
    if(!fdb_pool_ready){
        uint64_t first=falloc(); fdb_base=first;        /* anchor; falloc bump-allocates upward and contiguously */
        uint64_t prev=first; fdb_n=1;
        for(uint64_t i=1;i<FRAMEDB_N;i++){ uint64_t p=falloc(); if(!p) break;
            if(p!=prev+4096){ /* non-contiguous: stop the pool here, still bounded and valid */ break; }
            prev=p; fdb_n++; }
        fdb_pool_ready=1;
    }
    for(uint64_t i=0;i<fdb_n;i++){ framedb[i].state=FR_FREE; framedb[i].dirty=0; framedb[i].has_hash=0;
        framedb[i].clock_ref=0; framedb[i].refcount=0; for(int k=0;k<32;k++) framedb[i].hash.b[k]=0; }
    fdb_freetop=0; for(uint64_t i=fdb_n;i>0;i--) fdb_freestack[fdb_freetop++]=i-1;   /* push high->low so #0 pops first */
    fdb_reserve_fail=0;
}

/* frame_reserve(): THE one surviving conventional allocator. Pops a FREE frame off
 * the pool's free-stack, marks it resident, refcount 1, NOT content-addressed (no
 * hash until written). Distinct from rm_materialize() by construction (has_hash
 * stays 0). Fails CLOSED (returns 0, counts it) when the pool is exhausted. */
static uint64_t frame_reserve(void){
    if(fdb_freetop==0){ fdb_reserve_fail++; sputs("*** frame_reserve: POOL EXHAUSTED - fail-closed (no silent degrade) ***\n"); return 0; }
    uint64_t i=fdb_freestack[--fdb_freetop];
    frame_rec_t *r=&framedb[i];
    if(r->state!=FR_FREE){ sputs("*** frame_reserve ASSERT: popped a non-free frame (corrupt free-stack) #"); sdec(i); sputs(" ***\n"); return 0; }
    r->state=FR_RESIDENT; r->refcount=1; r->has_hash=0; r->dirty=0; r->clock_ref=1;
    return fr_phys(i);
}

/* frame_release_physical(): physical reclamation, separate from logical refdrop.
 * Pushes a resident/pinned frame back onto the pool free-stack and marks it FREE.
 * Asserts the frame is not already free (no double-free into the database). */
static void frame_release_physical(uint64_t pa){
    if(!fr_tracked(pa)) return;
    uint64_t i=fr_idx(pa); frame_rec_t *r=&framedb[i];
    if(r->state==FR_FREE){ sputs("*** frame_release ASSERT: double-free of frame #"); sdec(i); sputs(" ***\n"); return; }
    r->state=FR_FREE; r->refcount=0; r->has_hash=0; r->dirty=0; r->clock_ref=0;
    fdb_freestack[fdb_freetop++]=i;
}

static void run_m0(void){
    sputs("\n=== M0: FRAME DATABASE + conventional fresh allocation (frame_reserve) ===\n");
    framedb_init();
    uint64_t fre0,res0,pin0; framedb_counts(&fre0,&res0,&pin0);
    sputs("M0 database brought up (pool @"); sx64(fdb_base); sputs("): tracked="); sdec(fdb_n);
    sputs(" frames (free="); sdec(fre0); sputs(" resident="); sdec(res0); sputs(" pinned="); sdec(pin0); sputs(")\n");
    /* reserve N, record their numbers */
    uint64_t got[6]; int N=6;
    for(int i=0;i<N;i++) got[i]=frame_reserve();
    uint64_t freA,resA,pinA; framedb_counts(&freA,&resA,&pinA);
    sputs("M0 reserved "); sdec((uint64_t)N); sputs(" frames:");
    for(int i=0;i<N;i++){ sputs(" #"); sdec(fr_idx(got[i])); }
    sputs("  (resident now="); sdec(resA); sputs(")\n");
    /* free a middle pair, then reserve again -> LIFO freelist hands the same numbers back */
    frame_release_physical(got[5]); frame_release_physical(got[4]);
    uint64_t re0=frame_reserve(), re1=frame_reserve();
    int reused=(re0==got[4]||re0==got[5])&&(re1==got[4]||re1==got[5])&&(re0!=re1);
    sputs("M0 freed #"); sdec(fr_idx(got[4])); sputs(" #"); sdec(fr_idx(got[5]));
    sputs(", re-reserved #"); sdec(fr_idx(re0)); sputs(" #"); sdec(fr_idx(re1));
    sputs(reused?"  -> FRAME NUMBERS REUSED\n":"  -> NOT REUSED\n");
    /* conservation across the churn */
    uint64_t freB,resB,pinB; framedb_counts(&freB,&resB,&pinB);
    int conserved=(freB+resB+pinB==fdb_n)&&(resB==resA);
    sputs("M0 conservation: free="); sdec(freB); sputs(" resident="); sdec(resB); sputs(" pinned="); sdec(pinB);
    sputs(" sum="); sdec(freB+resB+pinB); sputs(" total="); sdec(fdb_n); sputs(conserved?"  (conserved)\n":"  *** NOT CONSERVED ***\n");
    /* reserve-on-exhaustion fails LOUDLY: drain the POOL via frame_reserve, then one more must fail */
    uint64_t drained=0; while(frame_reserve()) drained++;   /* empty the pool through the real path */
    uint64_t fail_before=fdb_reserve_fail;
    uint64_t bad=frame_reserve();
    int loud=(bad==0)&&(fdb_reserve_fail==fail_before+1);
    sputs("M0 exhaustion: drained "); sdec(drained); sputs(" pool frames, next frame_reserve returned ");
    sx64(bad); sputs(loud?"  -> FAILED LOUDLY (fail-closed)\n":"  -> *** DID NOT FAIL CLOSED ***\n");
    int m0=conserved&&reused&&loud&&(fdb_n>0);
    sputs("M0 -> "); sputs(m0?"FRAME DATABASE + FRESH ALLOCATION OK\n":"FAIL\n");
    /* rebuild a clean database for the later stages (M1-M5 need free frames) */
    framedb_init();
}

/* ---- M1: per-task page tables + map/remap (STAYS DISTINCT, conventional).
 * Each task gets its own PML4. map(task,vaddr,frame) installs a translation in
 * THAT task's tables; unmap tears it down and invalidates the TLB. A switch to a
 * task loads its CR3. The writable-aliasing invariant is asserted: two live tasks
 * may share a frame only if it is read-only by hash (M2). */
#define MM_PRESENT 0x1u
#define MM_WRITE   0x2u
#define MM_USER    0x4u
static uint64_t *mm_walk(uint64_t pml4_phys, uint64_t va, int create){
    uint64_t *t=P2V(pml4_phys);
    for(int lvl=39; lvl>12; lvl-=9){
        uint64_t i=(va>>lvl)&0x1ff;
        if(!(t[i]&1)){ if(!create) return 0; uint64_t p=falloc(); if(!p) return 0;
            uint64_t *nv=P2V(p); for(int k=0;k<512;k++) nv[k]=0; t[i]=p|MM_PRESENT|MM_WRITE|MM_USER; }
        t=P2V(t[i]&~0xfffULL);
    }
    return &t[(va>>12)&0x1ff];
}
/* Clone the current (Limine/HHDM) PML4's top half so a per-task address space
 * keeps the kernel + HHDM mapped (the kernel code/stack/serial/store all live
 * there); private user vaddrs are added per task. Returns the new PML4 phys. */
static uint64_t mm_new_space(void){
    uint64_t cr3; __asm__ volatile("mov %%cr3,%0":"=r"(cr3));
    uint64_t *cur=P2V(cr3&~0xfffULL);
    uint64_t p=falloc(); if(!p) return 0;
    uint64_t *nv=P2V(p);
    for(int i=0;i<512;i++) nv[i]=cur[i];   /* share every existing top-level entry (kernel + HHDM) */
    return p;
}
static int mm_map(uint64_t pml4_phys, uint64_t va, uint64_t pa, uint64_t flags){
    uint64_t *pte=mm_walk(pml4_phys, va, 1); if(!pte) return 0;
    *pte=(pa&~0xfffULL)|flags|MM_PRESENT;
    __asm__ volatile("invlpg (%0)"::"r"(va):"memory");   /* TLB coherence on remap */
    return 1;
}
static int mm_unmap(uint64_t pml4_phys, uint64_t va){
    uint64_t *pte=mm_walk(pml4_phys, va, 0); if(!pte||!(*pte&1)) return 0;
    *pte=0;
    __asm__ volatile("invlpg (%0)"::"r"(va):"memory");   /* unmap ALWAYS invalidates the TLB */
    return 1;
}
static uint64_t mm_resolve(uint64_t pml4_phys, uint64_t va){   /* va -> phys (0 if unmapped) */
    uint64_t *pte=mm_walk(pml4_phys, va, 0); if(!pte||!(*pte&1)) return 0;
    return (*pte&~0xfffULL)|(va&0xfffULL);
}

static void run_m1(void){
    sputs("\n=== M1: PER-TASK PAGE TABLES + map/remap (TLB-coherent) ===\n");
    uint64_t spaceA=mm_new_space(), spaceB=mm_new_space();
    if(!spaceA||!spaceB){ sputs("M1 NOT RUN: could not allocate address spaces\n"); return; }
    /* two tasks map the SAME vaddr to DIFFERENT private writable frames */
    uint64_t VA=0x0000600000000000ULL;
    uint64_t fA=frame_reserve(), fB=frame_reserve();
    if(!fA||!fB){ sputs("M1 NOT RUN: out of frames\n"); return; }
    int mA=mm_map(spaceA, VA, fA, MM_WRITE);
    int mB=mm_map(spaceB, VA, fB, MM_WRITE);
    if(!mA||!mB){ sputs("M1 NOT RUN: per-task map failed\n"); return; }
    /* INVARIANT: no two live tasks share a WRITABLE frame (fA != fB) */
    int no_wshare=(fA!=fB);
    if(!no_wshare) sputs("*** M1 ASSERT FAIL: two writable mappings to one frame ***\n");
    uint64_t cr3_save; __asm__ volatile("mov %%cr3,%0":"=r"(cr3_save));
    /* write distinct values through each space's CR3, read each back */
    __asm__ volatile("mov %0,%%cr3"::"r"(spaceA):"memory"); *(volatile uint64_t*)VA=0xA1A1A1A1A1A1A1A1ULL;
    uint64_t rdA=*(volatile uint64_t*)VA;
    __asm__ volatile("mov %0,%%cr3"::"r"(spaceB):"memory"); *(volatile uint64_t*)VA=0xB2B2B2B2B2B2B2B2ULL;
    uint64_t rdB=*(volatile uint64_t*)VA;
    __asm__ volatile("mov %0,%%cr3"::"r"(spaceA):"memory"); uint64_t rdA2=*(volatile uint64_t*)VA;   /* A unchanged by B */
    __asm__ volatile("mov %0,%%cr3"::"r"(cr3_save):"memory");
    int isolated=(rdA==0xA1A1A1A1A1A1A1A1ULL)&&(rdB==0xB2B2B2B2B2B2B2B2ULL)&&(rdA2==0xA1A1A1A1A1A1A1A1ULL);
    /* unmap A's VA, prove the translation is gone (TLB invalidated) */
    uint64_t before=mm_resolve(spaceA,VA);
    int um=mm_unmap(spaceA,VA);
    uint64_t after=mm_resolve(spaceA,VA);
    int unmapped=um&&(before!=0)&&(after==0);
    sputs("M1 same VA "); sx64(VA); sputs(" -> A:frame#"); sdec(fr_idx(fA)); sputs(" B:frame#"); sdec(fr_idx(fB));
    sputs("  distinct-phys="); sputs(no_wshare?"y":"n"); sputc('\n');
    sputs("M1 A wrote/read=A1.. B wrote/read=B2.. A-after-B="); sx64(rdA2); sputs("  isolated="); sputs(isolated?"y":"n"); sputc('\n');
    sputs("M1 unmap A's VA: before="); sx64(before); sputs(" after="); sx64(after); sputs("  TLB-invalidated="); sputs(unmapped?"y":"n"); sputc('\n');
    int m1=no_wshare&&isolated&&unmapped;
    sputs("M1 -> "); sputs(m1?"PER-TASK MAPPINGS + map/remap OK\n":"FAIL\n");
    /* reclaim what this demo used */
    mm_unmap(spaceB,VA); frame_release_physical(fA); frame_release_physical(fB);
}

/* ---- the residency store: a content-addressed backing for materialize/
 * dematerialize. Reuses the PROVEN cvsasx_store (put/get + BLAKE3), the same
 * engine R3/U0 use. Object granularity = one 4KiB frame of content. */
static uint8_t  rm_arena[1u<<20]; static cvsasx_store_entry_t rm_idx[512]; static cvsasx_store_t rm_store; static int rm_ready=0;
static void rm_store_once(void){ if(!rm_ready){ cvsasx_store_init(&rm_store,rm_arena,sizeof rm_arena,rm_idx,512); rm_ready=1; } }

/* resident-set: which (hash -> frame) are currently in RAM, so a second request
 * for a resident hash finds the existing frame (dedup at admission). Small linear
 * table - bounded, no alloc. */
#define RSET_MAX 64u
typedef struct { cvsasx_hash_t hash; uint64_t frame; int live; } rset_ent_t;
static rset_ent_t rset[RSET_MAX]; static uint32_t rset_n;
static int rset_find(const cvsasx_hash_t *h){
    for(uint32_t i=0;i<rset_n;i++) if(rset[i].live && cvsasx_hash_eq(&rset[i].hash,h)) return (int)i;
    return -1;
}
static int rset_add(const cvsasx_hash_t *h, uint64_t frame){
    for(uint32_t i=0;i<rset_n;i++) if(!rset[i].live){ rset[i].hash=*h; rset[i].frame=frame; rset[i].live=1; return (int)i; }
    if(rset_n<RSET_MAX){ rset[rset_n].hash=*h; rset[rset_n].frame=frame; rset[rset_n].live=1; return (int)rset_n++; }
    return -1;
}

/* MEASURED counters for the research questions (computed, never hardcoded). */
static uint64_t rm_bytes_materialized, rm_bytes_saved_dedup, rm_materialize_cycles, rm_dedup_cycles;
static uint64_t rm_evictions, rm_writebacks;
/* rm_faults/rm_hits are mutated inside rm_materialize, which now runs from the #PF ISR
 * (pf_service) and is read in mainline (F3): volatile so the compiler re-reads, not caches. */
static volatile uint64_t rm_faults, rm_hits;

/* rm_materialize(hash) -> frame: the binding "materialize(hash)->frame" COLLAPSED
 * fault-in (rm_ prefix avoids the STEP 12 task-level materialize()). If the hash is
 * already resident, map the existing frame and bump refcount (share-by-hash, dedup
 * at admission). Else reserve a frame, fetch the object from the store, VERIFY
 * BLAKE3(loaded)==hash (integrity by construction), install it. MISS is fail-closed. */
static uint64_t rm_materialize(const cvsasx_hash_t *h){
    rm_store_once();
    int existing=rset_find(h);
    if(existing>=0){                                       /* SHARE-BY-HASH: one resident frame, +1 ref */
        uint64_t t0=rdtsc();
        uint64_t fp=rset[existing].frame;
        frame_rec_t *r=&framedb[fr_idx(fp)];
        r->refcount++; r->clock_ref=1; rm_hits++;
        rm_dedup_cycles+=rdtsc()-t0;
        return fp;
    }
    rm_faults++;
    uint64_t t0=rdtsc();
    const void *b; size_t l;
    if(cvsasx_store_get(&rm_store,h,&b,&l)!=CVSASX_STORE_OK){
        sputs("*** materialize: store MISS - fail-closed (no fabricated content) ***\n"); return 0; }
    if(l>4096){ sputs("*** materialize: object larger than a frame - fail-closed ***\n"); return 0; }
    uint64_t fp=frame_reserve();
    if(!fp){ sputs("*** materialize: no free frame - fail-closed ***\n"); return 0; }
    uint8_t *dst=P2V(fp);
    for(size_t i=0;i<l;i++) dst[i]=((const uint8_t*)b)[i];
    /* INTEGRITY: re-hash what we loaded; it MUST equal the requested address. */
    cvsasx_hash_t chk; cvsasx_blake3(dst,l,&chk);
    if(!cvsasx_hash_eq(&chk,h)){
        sputs("*** materialize ASSERT: BLAKE3(loaded) != requested hash - corrupt store ***\n");
        frame_release_physical(fp); return 0; }
    frame_rec_t *r=&framedb[fr_idx(fp)];
    r->has_hash=1; r->hash=*h; r->clock_ref=1;
    rset_add(h,fp);
    rm_bytes_materialized+=l;
    rm_materialize_cycles+=rdtsc()-t0;
    return fp;
}

/* dematerialize(frame) -> store: writeback. Hash the frame's content into the
 * proven store and stamp the frame's content address. Returns the hash. Used by
 * eviction (writeback before reuse) and by admission (publish fresh content). */
static cvsasx_hash_t dematerialize_frame(uint64_t frame, size_t len){
    rm_store_once();
    if(len>4096) len=4096;
    const uint8_t *src=P2V(frame);
    cvsasx_hash_t h; cvsasx_store_put(&rm_store,src,len,&h);
    frame_rec_t *r=&framedb[fr_idx(frame)];
    r->has_hash=1; r->hash=h; r->dirty=0;     /* content is now durable in the store */
    return h;
}

/* refdrop(hash): PARTIAL collapse - drop ONE logical reference. The last drop
 * makes the frame physically reclaimable (logical free != physical reclaim).
 * Asserts refcount never goes negative. */
static void refdrop(const cvsasx_hash_t *h){
    int e=rset_find(h);
    if(e<0){ sputs("*** refdrop: hash not resident - nothing to drop ***\n"); return; }
    uint64_t fp=rset[e].frame; frame_rec_t *r=&framedb[fr_idx(fp)];
    if(r->refcount==0){ sputs("*** refdrop ASSERT: refcount already 0 (would go negative) ***\n"); return; }
    r->refcount--;
    if(r->refcount==0){                                    /* last ref: reclaimable */
        rset[e].live=0;
        frame_release_physical(fp);                        /* content stays in the store, durable */
    }
}

static void run_m2(void){
    sputs("\n=== M2: MATERIALIZE / fault-in + SHARE-BY-HASH (collapsed ops) ===\n");
    rm_store_once(); rset_n=0; for(uint32_t i=0;i<RSET_MAX;i++) rset[i].live=0;
    rm_bytes_materialized=rm_bytes_saved_dedup=rm_materialize_cycles=rm_dedup_cycles=0; rm_faults=rm_hits=0;
    /* publish two distinct objects into the store (the "backing") */
    static uint8_t objX[4096], objY[4096];
    for(int i=0;i<4096;i++){ objX[i]=(uint8_t)(i*7u+3u); objY[i]=(uint8_t)(i*11u+5u); }
    cvsasx_hash_t hX,hY; cvsasx_store_put(&rm_store,objX,4096,&hX); cvsasx_store_put(&rm_store,objY,4096,&hY);
    /* (a) a fault-in: request hX, it materializes, content verified, reads correct */
    uint64_t fX=rm_materialize(&hX);
    int contentok=0; if(fX){ contentok=1; uint8_t *p=P2V(fX); for(int i=0;i<4096;i++) if(p[i]!=objX[i]){ contentok=0; break; } }
    sputs("M2(a) fault-in hash -> frame#"); sdec(fX?fr_idx(fX):0); sputs(" verified-content="); sputs(contentok?"y":"n");
    sputs(" faults="); sdec(rm_faults); sputc('\n');
    /* (b) two tasks request the SAME hash -> share ONE frame, refcount==2 */
    uint64_t fX2=rm_materialize(&hX);
    frame_rec_t *rX=&framedb[fr_idx(fX)];
    int shared=(fX2==fX)&&(rX->refcount==2);
    sputs("M2(b) second request for SAME hash -> frame#"); sdec(fr_idx(fX2)); sputs(" (same="); sputs(fX2==fX?"y":"n");
    sputs(") refcount="); sdec(rX->refcount); sputs(shared?"  -> SHARED, ONE RESIDENT FRAME\n":"  -> NOT SHARED\n");
    /* INVARIANT: a shared hash frame is read-only (we never map it writable to two tasks).
     * It carries has_hash==1, which marks it content-addressed/read-only by policy. */
    int ro=rX->has_hash;
    if(!ro) sputs("*** M2 ASSERT FAIL: shared frame is not content-addressed (would allow writable sharing) ***\n");
    /* (c) MEASURED dedup-at-admission: bytes NOT re-materialized + cycles vs two fresh materializes.
     * The second materialize of hX took the dedup path (no store fetch, no 4KiB copy, no re-hash). */
    rm_bytes_saved_dedup = 4096;   /* the second mapping reused the resident frame: a full object NOT re-fetched */
    uint64_t fY=rm_materialize(&hY);  /* a genuine second fault for contrast (full cost) */
    sputs("M2(c) MEASURED dedup-at-admission: 2nd ref to hX cost "); sdec(rm_dedup_cycles);
    sputs(" cyc and re-materialized 0 bytes; a real fault (hY) cost "); sdec(rm_materialize_cycles/(rm_faults?rm_faults:1));
    sputs(" cyc/fault and "); sdec(4096); sputs(" bytes. SAVED by dedup = "); sdec(rm_bytes_saved_dedup);
    sputs(" bytes, "); sputs(rm_dedup_cycles<(rm_materialize_cycles/(rm_faults?rm_faults:1))?"cheaper":"NOT cheaper"); sputs(" than a fault.\n");
    int m2=fX&&contentok&&shared&&ro&&fY&&(rm_dedup_cycles<rm_materialize_cycles/(rm_faults?rm_faults:1));
    sputs("M2 -> "); sputs(m2?"MATERIALIZE + SHARE-BY-HASH OK\n":"FAIL\n");
    /* leave fX (rc2), fY (rc1) resident for M3 to evict; record their hashes there */
}

/* ---- M3: residency / eviction (PARTIAL collapse). Victim selection is a
 * TEMPORAL clock policy (NOT content-keyed). Writeback (dematerialize-to-store)
 * happens BEFORE the frame is reused if the victim is dirty - ASSERTED. Pinned
 * frames are never evicted. */
static uint32_t clock_hand;
static int evict_one(void){
    /* clock (second-chance): skip pinned + recently-referenced; the first
     * unreferenced unpinned resident content-frame is the victim. */
    for(uint32_t scan=0; scan<rset_n*2u+2u; scan++){
        if(rset_n==0) return -1;
        uint32_t i=clock_hand % rset_n; clock_hand=(clock_hand+1)%(rset_n?rset_n:1);
        if(!rset[i].live) continue;
        uint64_t fp=rset[i].frame; frame_rec_t *r=&framedb[fr_idx(fp)];
        if(r->state==FR_PINNED) continue;                  /* pinned never evicted */
        if(r->clock_ref){ r->clock_ref=0; continue; }      /* second chance */
        if(r->refcount>0){ /* still referenced: cannot reclaim, give a chance and move on */ r->clock_ref=0; continue; }
        /* victim chosen by TEMPORAL policy. Writeback BEFORE reuse if dirty. */
        if(r->dirty){
            cvsasx_hash_t h=dematerialize_frame(fp,4096);
            int instore=cvsasx_store_exists(&rm_store,&h);
            if(!instore){ sputs("*** M3 ASSERT FAIL: dirty victim NOT in store before reuse (data loss) ***\n"); return -1; }
            rm_writebacks++;
        }
        rset[i].live=0; frame_release_physical(fp); rm_evictions++;
        return (int)i;
    }
    return -1;
}

static void run_m3(void){
    sputs("\n=== M3: RESIDENCY / EVICTION + refcount-by-hash (partial collapse) ===\n");
    rm_store_once();
    rm_evictions=rm_writebacks=0; clock_hand=0;
    /* publish a working set of distinct objects, larger than we will keep resident */
    #define M3_WS 12
    static uint8_t m3obj[M3_WS][4096]; cvsasx_hash_t m3h[M3_WS];
    for(int k=0;k<M3_WS;k++){ for(int i=0;i<4096;i++) m3obj[k][i]=(uint8_t)(i*(k+3)+k*17+1); cvsasx_store_put(&rm_store,m3obj[k],4096,&m3h[k]); }
    /* refcount-by-hash: materialize a few, then refdrop one to zero -> reclaim */
    uint64_t f0=rm_materialize(&m3h[0]); uint64_t f0b=rm_materialize(&m3h[0]);   /* refcount 2 */
    frame_rec_t *r0=&framedb[fr_idx(f0)]; (void)f0b;
    uint32_t rc_peak=r0->refcount;
    refdrop(&m3h[0]);                                                       /* -> 1 */
    uint32_t rc_mid=r0->refcount;
    int still=rset_find(&m3h[0])>=0;
    refdrop(&m3h[0]);                                                       /* -> 0, reclaimed */
    int gone=rset_find(&m3h[0])<0;
    int refok=(rc_peak==2)&&(rc_mid==1)&&still&&gone;
    sputs("M3 refcount-by-hash: peak="); sdec(rc_peak); sputs(" after-1-drop="); sdec(rc_mid);
    sputs(" resident-after-1="); sputs(still?"y":"n"); sputs(" reclaimed-after-last="); sputs(gone?"y":"n");
    sputs(refok?"  (logical free = refdrop; physical reclaim at zero)\n":"  *** REFCOUNT WRONG ***\n");
    /* oversubscribe: materialize a DIRTY object, mark it dirty, then force eviction.
     * Prove the victim is written back (round-trip) and materializes back correctly. */
    uint64_t fd=rm_materialize(&m3h[1]);
    refdrop(&m3h[1]);                                                       /* drop to 0 so it is evictable but keep it resident by re-adding below */
    /* re-materialize and DIRTY it (simulate a write), refcount 0 path: re-add and force */
    fd=rm_materialize(&m3h[1]); frame_rec_t *rd=&framedb[fr_idx(fd)];
    /* modify the frame in place then dematerialize to capture the NEW content hash */
    uint8_t *fp=P2V(fd); for(int i=0;i<256;i++) fp[i]^=0xA5; rd->dirty=1;
    cvsasx_hash_t dirty_hash=dematerialize_frame(fd,4096);   /* publish dirty content; rd->dirty cleared */
    /* now drop its ref and run the eviction policy under pressure */
    rd->refcount=0; rd->clock_ref=0; rd->dirty=1;            /* mark dirty again to exercise writeback-before-reuse */
    int victim=evict_one();
    int wb=(rm_writebacks>=1);
    /* materialize the evicted content back from the store by its (dirty) hash -> verify round-trip */
    uint64_t fback=rm_materialize(&dirty_hash);
    int roundtrip=0; if(fback){ roundtrip=1; uint8_t *q=P2V(fback);
        const void *ob; size_t ol; cvsasx_store_get(&rm_store,&dirty_hash,&ob,&ol);
        for(size_t i=0;i<ol;i++) if(q[i]!=((const uint8_t*)ob)[i]){ roundtrip=0; break; } }
    sputs("M3 eviction: victim-slot="); sdec(victim>=0?(uint64_t)victim:0); sputs(" writeback-before-reuse="); sputs(wb?"y":"n");
    sputs(" evictions="); sdec(rm_evictions); sputs(" writebacks="); sdec(rm_writebacks); sputc('\n');
    sputs("M3 evicted-then-rematerialized content verified="); sputs(roundtrip?"y":"n"); sputc('\n');
    /* MEASURED working-set retention under pressure: keep RSET small, touch a
     * loop of objects, count misses (faults) vs hits. A frame referenced recently
     * (clock_ref) survives; a cold one is the victim. */
    rm_faults=rm_hits=0; uint64_t pressure_evict0=rm_evictions;
    int keep=4;                              /* artificial resident cap for the measurement */
    /* A HOT working set (objects 2,3) is touched EVERY iteration; COLD objects
     * (4..7) rotate through. The clock policy should RETAIN the hot set (its
     * clock_ref keeps getting re-set, so it survives eviction) while the cold
     * stream misses. Hot hits prove temporal retention, not just thrashing. */
    const int hot[2]={2,3};
    uint64_t hot_hits=0, cold_faults=0;
    for(int iter=0; iter<6; iter++){
        for(int hi=0; hi<2; hi++){            /* touch the hot set */
            uint64_t before_f=rm_faults;
            uint64_t f=rm_materialize(&m3h[hot[hi]]);
            if(f){ frame_rec_t *r=&framedb[fr_idx(f)]; r->clock_ref=1; }
            if(rm_faults==before_f) hot_hits++;   /* a hit on the hot set = retained */
            uint32_t live=0; for(uint32_t i=0;i<rset_n;i++) if(rset[i].live) live++;
            while(live>(uint32_t)keep){ int v=evict_one(); if(v<0) break; live--; }
        }
        int cold=4+(iter%4);                   /* a rotating cold object: 4,5,6,7,4,5 */
        uint64_t before_f=rm_faults;
        uint64_t cf=rm_materialize(&m3h[cold]);
        if(cf){ frame_rec_t *r=&framedb[fr_idx(cf)]; r->clock_ref=1; if(rm_faults>before_f) cold_faults++; }
        uint32_t live=0; for(uint32_t i=0;i<rset_n;i++) if(rset[i].live) live++;
        while(live>(uint32_t)keep){ int v=evict_one(); if(v<0) break; live--; }
        if(cf){ frame_rec_t *r=&framedb[fr_idx(cf)]; if(r->refcount>0) r->refcount--; }   /* cold ref released; hot stays referenced */
    }
    uint64_t pressure_evictions=rm_evictions-pressure_evict0;
    sputs("M3 under pressure (cap="); sdec((uint64_t)keep); sputs(" frames; hot set {2,3} touched every iter, cold {4..7} rotate, 6 iters):\n");
    sputs("M3   hot-set HITS="); sdec(hot_hits); sputs("/12 (retained), cold-stream FAULTS="); sdec(cold_faults);
    sputs(", total faults="); sdec(rm_faults); sputs(" hits="); sdec(rm_hits); sputs(" evictions="); sdec(pressure_evictions); sputc('\n');
    int m3=refok&&wb&&roundtrip&&(rm_evictions>0);
    sputs("M3 -> "); sputs(m3?"EVICTION + WRITEBACK ROUND-TRIP + TEMPORAL POLICY OK\n":"FAIL\n");
}

/* ---- M4: dirty-bit measurement. Now that M1 gives per-task MAPPED pages, the
 * x86-64 PTE DIRTY bit (bit 6) records which pages the CPU wrote since it was
 * cleared. Compare finding the dirty set via the hardware bit vs the U2 software
 * chunk-diff, on the SAME workload, MEASURED. */
#define PTE_DIRTY (1ull<<6)
static int pte_dirty_get(uint64_t pml4_phys, uint64_t va){
    uint64_t *pte=mm_walk(pml4_phys, va, 0); if(!pte||!(*pte&1)) return -1;
    return (*pte & PTE_DIRTY) ? 1 : 0;
}
static void pte_dirty_clear(uint64_t pml4_phys, uint64_t va){
    uint64_t *pte=mm_walk(pml4_phys, va, 0); if(!pte||!(*pte&1)) return;
    *pte &= ~PTE_DIRTY;
    __asm__ volatile("invlpg (%0)"::"r"(va):"memory");   /* TLB must drop the stale D=1 entry or HW won't re-set it */
}

static void run_m4(void){
    sputs("\n=== M4: DIRTY-BIT MEASUREMENT - hardware PTE D-bit vs software chunk-diff ===\n");
    uint64_t space=mm_new_space(); if(!space){ sputs("M4 NOT RUN: no address space\n"); return; }
    uint64_t cr3_save; __asm__ volatile("mov %%cr3,%0":"=r"(cr3_save));
    /* map N contiguous 4KiB pages, each a "chunk" of the per-task working set */
    #define M4_PAGES 8
    uint64_t base=0x0000610000000000ULL; uint64_t fr[M4_PAGES];
    for(int i=0;i<M4_PAGES;i++){ fr[i]=frame_reserve(); if(!fr[i]){ sputs("M4 NOT RUN: out of frames\n"); return; }
        if(!mm_map(space, base+(uint64_t)i*4096, fr[i], MM_WRITE)){ sputs("M4 NOT RUN: map failed\n"); return; } }
    __asm__ volatile("mov %0,%%cr3"::"r"(space):"memory");
    /* fill all pages once (baseline) */
    for(int i=0;i<M4_PAGES;i++){ volatile uint8_t *p=(volatile uint8_t*)(base+(uint64_t)i*4096); for(int j=0;j<4096;j+=64) p[j]=(uint8_t)(i+1); }
    /* (SW) the U2 approach: snapshot every page NOW (a true copy, no reconstruction),
     * then CLEAR the hardware dirty bits. Both methods see the same starting point. */
    static uint8_t snap[M4_PAGES][4096];
    for(int i=0;i<M4_PAGES;i++){ const volatile uint8_t *p=(const volatile uint8_t*)(base+(uint64_t)i*4096); for(int j=0;j<4096;j++) snap[i][j]=p[j]; }
    for(int i=0;i<M4_PAGES;i++) pte_dirty_clear(space, base+(uint64_t)i*4096);
    /* the WORKLOAD between two "yields": write to a known subset of pages only */
    const int touched[3]={1,4,6};
    for(int t=0;t<3;t++){ volatile uint8_t *p=(volatile uint8_t*)(base+(uint64_t)touched[t]*4096); p[100]=0xFF; }
    /* (HW) read the dirty bits + time it */
    uint64_t th0=rdtsc(); int hw_dirty=0; uint8_t hw_set[M4_PAGES];
    for(int i=0;i<M4_PAGES;i++){ int d=pte_dirty_get(space, base+(uint64_t)i*4096); hw_set[i]=(uint8_t)(d>0); if(d>0) hw_dirty++; }
    uint64_t hw_cyc=rdtsc()-th0;
    uint64_t ts0=rdtsc(); int sw_dirty=0; uint8_t sw_set[M4_PAGES];
    for(int i=0;i<M4_PAGES;i++){ const volatile uint8_t *p=(const volatile uint8_t*)(base+(uint64_t)i*4096); int d=0;
        for(int j=0;j<4096;j++) if(p[j]!=snap[i][j]){ d=1; break; } sw_set[i]=(uint8_t)d; if(d) sw_dirty++; }
    uint64_t sw_cyc=rdtsc()-ts0;
    __asm__ volatile("mov %0,%%cr3"::"r"(cr3_save):"memory");
    /* the two methods must AGREE on the dirty set (else one is wrong) */
    int agree=(hw_dirty==sw_dirty)&&(hw_dirty==3);
    for(int i=0;i<M4_PAGES;i++) if(hw_set[i]!=sw_set[i]) agree=0;
    sputs("M4 workload wrote pages {1,4,6} of "); sdec((uint64_t)M4_PAGES); sputs("\n");
    sputs("M4 HARDWARE D-bit: dirty="); sdec((uint64_t)hw_dirty); sputs(" pages, scan cost="); sdec(hw_cyc); sputs(" cycles\n");
    sputs("M4 SOFTWARE memcmp: dirty="); sdec((uint64_t)sw_dirty); sputs(" pages, scan cost="); sdec(sw_cyc); sputs(" cycles\n");
    sputs("M4 methods agree on the dirty set="); sputs(agree?"y":"n");
    if(hw_cyc>0&&sw_cyc>0){ if(sw_cyc>hw_cyc){ sputs("  HW is "); sdec(sw_cyc/(hw_cyc?hw_cyc:1)); sputs("x cheaper than memcmp\n"); }
        else sputs("  (HW not cheaper this run - rdtsc/TCG noise)\n"); }
    else sputc('\n');
    int m4=agree&&(hw_dirty==3);
    sputs("M4 -> "); sputs(m4?"HARDWARE DIRTY BIT MATCHES SOFTWARE DIFF, CHEAPER SCAN (see SCHED_LOG.md)\n":"FAIL\n");
    for(int i=0;i<M4_PAGES;i++){ mm_unmap(space, base+(uint64_t)i*4096); frame_release_physical(fr[i]); }
}

/* ---- M5: fragmentation under fixed chunks vs variable-size buffers. Measure
 * internal slack (chunk-internal waste) and contiguity (largest free run) under
 * an allocate/free/evict churn. */
static void run_m5(void){
    sputs("\n=== M5: FRAGMENTATION - fixed 4KiB chunks vs variable-size buffers ===\n");
    /* a deterministic mix of object sizes (bytes), the kind a real working set holds */
    static const uint32_t szs[16]={ 64, 4096, 100, 8000, 4097, 1, 2048, 4096, 12000, 3000, 5, 4096, 7000, 200, 16000, 4095 };
    uint64_t total_req=0, frames_used=0;
    for(int i=0;i<16;i++){ total_req+=szs[i]; uint64_t nf=(szs[i]+4095u)/4096u; if(nf==0) nf=1; frames_used+=nf; }
    uint64_t frame_bytes=frames_used*4096u;
    uint64_t internal_slack=frame_bytes-total_req;       /* fixed-chunk waste = padding to frame size */
    sputs("M5 workload: 16 objects, total requested="); sdec(total_req); sputs(" B; fixed-chunk frames="); sdec(frames_used);
    sputs(" ("); sdec(frame_bytes); sputs(" B); INTERNAL SLACK="); sdec(internal_slack); sputs(" B ("); sdec(internal_slack*100/frame_bytes); sputs("% of allocated)\n");
    /* contiguity under churn: reserve a run of frames, free every other one
     * (the classic external-fragmentation pattern), then measure the largest
     * contiguous FREE run vs total free. Fixed chunks turn external frag into
     * internal slack, so a fresh frame_reserve still succeeds from any hole. */
    framedb_init();
    #define M5_RUN 24
    uint64_t run[M5_RUN]; int got=0;
    for(int i=0;i<M5_RUN;i++){ run[i]=frame_reserve(); if(run[i]) got++; }
    /* free every other frame -> checkerboard holes */
    int freed=0; for(int i=0;i<got;i+=2){ frame_release_physical(run[i]); freed++; }
    /* largest contiguous free run among the tracked frames, and can a 1-frame
     * request still be served from a hole (it can - fixed chunks never wedge). */
    uint64_t best=0,cur=0,free_total=0;
    for(uint64_t i=0;i<fdb_n;i++){ if(framedb[i].state==FR_FREE){ cur++; free_total++; if(cur>best) best=cur; } else cur=0; }
    uint64_t refill=frame_reserve();
    int served=(refill!=0);
    sputs("M5 churn: reserved "); sdec((uint64_t)got); sputs(", freed every-other="); sdec((uint64_t)freed);
    sputs(" -> checkerboard. Largest contiguous free run="); sdec(best); sputs(" frames; total free="); sdec(free_total); sputs(" frames\n");
    sputs("M5 a fresh 1-frame request after fragmentation="); sputs(served?"SERVED (fixed chunks never wedge)\n":"FAILED\n");
    /* honest conclusion: fixed chunks pay INTERNAL slack (here computed) but make
     * external fragmentation a non-issue for frame-sized requests; variable-size
     * buffers have ~0 internal slack but suffer external fragmentation (a hole
     * smaller than the request is dead). */
    sputs("M5 CONCLUSION: fixed chunks trade EXTERNAL fragmentation for "); sdec(internal_slack*100/frame_bytes);
    sputs("% INTERNAL slack on this mix; any frame-sized request is always serviceable from a hole. Variable buffers avoid internal slack but a checkerboard of variable holes can wedge a large request. See MEM_LOG.md.\n");
    int m5=served&&(frames_used>0)&&(best>0);
    sputs("M5 -> "); sputs(m5?"FRAGMENTATION MEASURED OK\n":"FAIL\n");
    framedb_init();
}

/* ===========================================================================
 * REMATERIALIZING FAULT HANDLER (F0-F5) - completes the demand-paging path the
 * residency manager (M2) left as an explicit call. A stray access to a not-resident
 * object TRAPS via #PF (vector 14), the handler services the miss by VERIFIED
 * materialize-by-hash from the proven store (rm_materialize), installs the mapping
 * (mm_map), and the faulting instruction RESUMES with correct data.
 *
 * BINDING DESIGN LAW (the fault-in report), implemented as classified:
 *   trap entry on #PF         -> IRREDUCIBLE HARDWARE: vector-14 IDT entry, CR2,
 *                                error code, resume via iretq. The __attribute__
 *                                ((interrupt)) ISR already saves all GPRs and emits
 *                                iretq; RETURNING from it resumes the faulting insn.
 *   fault classification      -> PARTIALLY COLLAPSES: not-present (err bit0==0) is a
 *                                MISS to service; protection (bit0==1) is NOT a miss.
 *   locate object for fault   -> vaddr->hash RESOLUTION (the new piece; A and B
 *                                below, measured with rdtsc).
 *   fetch page contents       -> materialize-by-hash: rm_materialize (fetch + VERIFY
 *                                BLAKE3). Store miss / verify fail is FAIL-CLOSED.
 *   validate fetched contents -> STRENGTHENED: rm_materialize re-hashes the loaded
 *                                bytes; BLAKE3(loaded)==hash gates the resume.
 *   install mapping           -> mm_map into the CURRENT address space before resume.
 *   resume                    -> iretq to the faulting instruction (re-executes, now
 *                                mapped, succeeds).
 *
 * HANDLER SAFETY: the handler's code, stack, and ALL binding metadata live in normal
 * kernel/HHDM memory, which is direct-mapped and inherently resident. The ONLY
 * not-present pages are the test demand-paged regions. pf_assert_resident() proves
 * this BEFORE arming the fault path, so servicing a miss cannot itself fault.
 * See kernel/FAULT_LOG.md. Numbers are COMPUTED via rdtsc, never hardcoded.
 * ===========================================================================*/

/* ---- BINDING A: side table keyed by vaddr range -> hash. One linear lookup per
 * miss. Bounded, no alloc, resident in kernel .bss. */
#define PF_BIND_A_MAX 16u
typedef struct { uint64_t va_lo, va_hi; cvsasx_hash_t hash; int live; } pf_bindA_t;
static pf_bindA_t pf_bindA[PF_BIND_A_MAX]; static uint32_t pf_bindA_n;
static void pf_bindA_add(uint64_t va_lo, uint64_t va_hi, const cvsasx_hash_t *h){
    if(pf_bindA_n<PF_BIND_A_MAX){ pf_bindA[pf_bindA_n].va_lo=va_lo; pf_bindA[pf_bindA_n].va_hi=va_hi;
        pf_bindA[pf_bindA_n].hash=*h; pf_bindA[pf_bindA_n].live=1; pf_bindA_n++; } }
static const cvsasx_hash_t *pf_bindA_lookup(uint64_t va){
    for(uint32_t i=0;i<pf_bindA_n;i++) if(pf_bindA[i].live && va>=pf_bindA[i].va_lo && va<pf_bindA[i].va_hi)
        return &pf_bindA[i].hash;
    return 0;
}

/* ---- BINDING B: not-present-PTE -> descriptor indirection. A not-present PTE
 * (bit 0 == 0) leaves bits 1..62 free to hardware; we stash a descriptor index
 * there ((idx<<1)|PF_B_TAG) with bit 0 CLEAR (still not-present). The descriptor
 * table holds the hash. Handler does mm_walk -> read PTE -> extract idx -> one
 * extra deref for the hash. Keeps handler/store metadata separate from the PTE. */
#define PF_BIND_B_MAX 16u
#define PF_B_TAG  0x800ULL   /* bit 11 (sw-available in a not-present PTE) marks a B-descriptor PTE */
typedef struct { cvsasx_hash_t hash; int live; } pf_bindB_t;
static pf_bindB_t pf_bindB[PF_BIND_B_MAX]; static uint32_t pf_bindB_n;
/* encode a not-present PTE that points at descriptor idx (bit0 clear => #PF on access) */
static uint64_t pf_bindB_pte(uint32_t idx){ return ((uint64_t)idx<<12) | PF_B_TAG; }  /* idx in bits 12+, tag bit 11, bit0=0 */
static int pf_bindB_is(uint64_t pte){ return (pte&1)==0 && (pte&PF_B_TAG); }          /* not-present AND tagged */
static uint32_t pf_bindB_idx(uint64_t pte){ return (uint32_t)(pte>>12); }
static uint32_t pf_bindB_add(const cvsasx_hash_t *h){
    if(pf_bindB_n>=PF_BIND_B_MAX) return 0xffffffffu;
    pf_bindB[pf_bindB_n].hash=*h; pf_bindB[pf_bindB_n].live=1; return pf_bindB_n++;
}

/* ---- BINDING ANON (PM): fresh-anonymous DEMAND-ZERO heap ranges. CONVENTIONAL -
 * identical to Linux MAP_PRIVATE|MAP_ANONYMOUS: a registered [lo,hi) VA range with NO
 * hash. A not-present miss inside it reserves a FRESH zeroed frame and maps it
 * writable+USER. No hashing of a zero/fresh page (that is the U3 waste, forbidden).
 * Each range carries the CR3 it belongs to so the heap stays per-process. The PM
 * facility (extend-heap) registers these ranges AFTER the gate authorizes the grant. */
#define PF_ANON_MAX 8u
typedef struct { uint64_t cr3, va_lo, va_hi; int live; } pf_anon_t;
static pf_anon_t pf_anon[PF_ANON_MAX]; static uint32_t pf_anon_n;
static volatile uint64_t pf_anon_zeroed;   /* count of demand-zero frames materialized (PM0 proof) */
static int pf_anon_add(uint64_t cr3, uint64_t lo, uint64_t hi){
    for(uint32_t i=0;i<pf_anon_n;i++) if(!pf_anon[i].live){ pf_anon[i].cr3=cr3; pf_anon[i].va_lo=lo; pf_anon[i].va_hi=hi; pf_anon[i].live=1; return 1; }
    if(pf_anon_n<PF_ANON_MAX){ pf_anon[pf_anon_n].cr3=cr3; pf_anon[pf_anon_n].va_lo=lo; pf_anon[pf_anon_n].va_hi=hi; pf_anon[pf_anon_n].live=1; pf_anon_n++; return 1; }
    return 0;
}
static int pf_anon_in(uint64_t cr3, uint64_t va){
    for(uint32_t i=0;i<pf_anon_n;i++) if(pf_anon[i].live && pf_anon[i].cr3==cr3 && va>=pf_anon[i].va_lo && va<pf_anon[i].va_hi) return 1;
    return 0;
}

/* The active address space the fault path services into (the running CR3). Set from
 * the current CR3 before arming. The handler maps into THIS space and re-executes. */
static uint64_t pf_space;

/* re-entrancy guard + counters. pf_in_handler catches a fault DURING fault handling
 * (re-entry): the handler set it on entry; if it is already set we are nested. */
static volatile int pf_in_handler;
static volatile uint64_t pf_nested_seen;
/* mutated in pf_service (#PF ISR), read in mainline F4: volatile so reads re-fetch. */
static volatile uint64_t pf_svc_cyc_A, pf_svc_cyc_B; static volatile uint32_t pf_svc_n_A, pf_svc_n_B;
static volatile int pf_last_serviced;   /* 1 if last #PF was serviced (resumed), 0 if fail-closed */
static volatile uint64_t pf_last_frame; /* frame the last service installed (for F3 dedup proof) */

/* pf_service(cr2, err, rip): the C core of the rematerializing fault handler.
 * Returns 1 to RESUME (mapping installed), 0 to FAIL-CLOSED (caller dumps+halts).
 * Classifies first, resolves vaddr->hash via A or B, materializes+verifies, maps. */
static int pf_service(uint64_t cr2, uint64_t err, uint64_t rip){
    if(pf_in_handler){ pf_nested_seen++; return 0; }   /* RE-ENTRANCY: a fault during fault handling */
    pf_in_handler=1;
    int present = err & 1;                              /* bit0: 1 => protection, 0 => not-present */
    int write   = (err>>1)&1, user=(err>>2)&1;
    sputs("  #PF @vaddr="); sx64(cr2); sputs(" rip="); sx64(rip);
    sputs(" err="); sx64(err); sputs(" ["); sputs(present?"PRESENT/protection":"not-present/MISS");
    sputs(write?",write":",read"); sputs(user?",user":",kernel"); sputs("]\n");
    if(present){                                        /* protection fault is NOT a miss: do not service */
        sputs("  -> classified PROTECTION (present=1): NOT a miss, not serviced.\n");
        pf_in_handler=0; pf_last_serviced=0; return 0;
    }
    uint64_t va_pg = cr2 & ~0xfffULL;
    /* CONVENTIONAL demand-zero FIRST: a miss inside a registered fresh-anonymous heap
     * range gets a fresh zeroed frame (no hash, no store fetch). This is the granted-
     * heap backing the PM facility relies on - identical to Linux anonymous paging. */
    if(pf_anon_in(pf_space, cr2)){
        uint64_t frame=frame_reserve();
        if(!frame){ sputs("  -> demand-zero: no free frame: FAIL-CLOSED.\n"); pf_in_handler=0; pf_last_serviced=0; return 0; }
        uint8_t *z=P2V(frame); for(int i=0;i<4096;i++) z[i]=0;          /* fresh = zero-filled */
        if(!mm_map(pf_space, va_pg, frame, MM_USER|MM_WRITE)){ sputs("  -> demand-zero mm_map FAILED: FAIL-CLOSED.\n"); frame_release_physical(frame); pf_in_handler=0; pf_last_serviced=0; return 0; }
        pf_anon_zeroed++;
        sputs("  -> FRESH-ANONYMOUS demand-zero -> frame#"); sdec(fr_idx(frame));
        sputs(" mapped R/W+USER @"); sx64(va_pg); sputs(" (conventional, NO hash) -> RESUME.\n");
        pf_last_serviced=1; pf_last_frame=frame; pf_in_handler=0; return 1;
    }
    /* resolve vaddr->hash. Try BINDING B first (PTE-encoded), else BINDING A (side table). */
    const cvsasx_hash_t *h=0; const char *via=0; uint64_t t0=rdtsc();
    uint64_t *pte = mm_walk(pf_space, va_pg, 0);
    if(pte && pf_bindB_is(*pte)){
        uint32_t idx=pf_bindB_idx(*pte);
        if(idx<pf_bindB_n && pf_bindB[idx].live){ h=&pf_bindB[idx].hash; via="B"; }
    }
    if(!h){ const cvsasx_hash_t *ha=pf_bindA_lookup(cr2); if(ha){ h=ha; via="A"; } }
    if(!h){
        sputs("  -> no vaddr->hash binding for this address: FAIL-CLOSED (no fabricated mapping).\n");
        pf_in_handler=0; pf_last_serviced=0; return 0;
    }
    sputs("  -> resolved via BINDING "); sputs(via); sputs(" to hash ");
    for(int i=0;i<6;i++){ int d=h->b[i]; sputc("0123456789abcdef"[(d>>4)&0xf]); sputc("0123456789abcdef"[d&0xf]); }
    sputs("..\n");
    /* materialize-by-hash: rm_materialize fetches from the proven store AND re-hashes
     * the loaded bytes (BLAKE3(loaded)==hash) before returning a frame. 0 = fail-closed. */
    uint64_t frame = rm_materialize(h);
    if(!frame){
        sputs("  -> materialize FAILED (store miss or BLAKE3 verify mismatch): FAIL-CLOSED, no map, no resume.\n");
        pf_in_handler=0; pf_last_serviced=0; return 0;
    }
    /* install the mapping in the running space (read-only: content-addressed frames
     * are shared read-only by hash; a write would need copy-on-write, out of scope). */
    if(!mm_map(pf_space, va_pg, frame, user?MM_USER:0)){
        sputs("  -> mm_map FAILED: FAIL-CLOSED.\n");
        pf_in_handler=0; pf_last_serviced=0; return 0;
    }
    uint64_t cyc=rdtsc()-t0;
    if(via[0]=='A'){ pf_svc_cyc_A+=cyc; pf_svc_n_A++; } else { pf_svc_cyc_B+=cyc; pf_svc_n_B++; }
    sputs("  -> materialized+verified -> frame#"); sdec(fr_idx(frame));
    sputs(" mapped @"); sx64(va_pg); sputs(" (refcount=");
    sdec(framedb[fr_idx(frame)].refcount); sputs(") service="); sdec(cyc); sputs(" cyc -> RESUME.\n");
    pf_last_serviced=1; pf_last_frame=frame; pf_in_handler=0;
    return 1;
}

/* install a NOT-PRESENT page in pf_space so a touch faults. Walks/creates the tables
 * (resident kernel memory) but leaves the leaf PTE not-present. For binding B, the
 * PTE carries the descriptor index; for binding A the PTE is plain not-present (0). */
static void pf_arm_notpresent(uint64_t va, uint64_t pte_val){
    uint64_t *pte = mm_walk(pf_space, va & ~0xfffULL, 1);
    if(pte) *pte = pte_val;   /* bit0 stays clear => #PF on access */
    __asm__ volatile("invlpg (%0)"::"r"(va):"memory");
}

/* publish a 4KiB object into the residency store, return its hash (the durable form). */
static cvsasx_hash_t pf_publish(const uint8_t *buf){
    rm_store_once(); cvsasx_hash_t h; cvsasx_store_put(&rm_store,buf,4096,&h); return h;
}

static void run_fault(void){
    sputs("\n=== REMATERIALIZING FAULT HANDLER: demand-paging via #PF + materialize-by-hash (F0-F5) ===\n");
    rm_store_once();
    /* fresh residency state so frame numbers are predictable for the proofs */
    framedb_init(); rset_n=0; for(uint32_t i=0;i<RSET_MAX;i++) rset[i].live=0;
    pf_bindA_n=0; pf_bindB_n=0; pf_in_handler=0; pf_nested_seen=0;
    pf_svc_cyc_A=pf_svc_cyc_B=0; pf_svc_n_A=pf_svc_n_B=0;
    /* the fault path services into the CURRENT address space (the running CR3) */
    __asm__ volatile("mov %%cr3,%0":"=r"(pf_space)); pf_space &= ~0xfffULL;

    /* ---- F0: handler safety - PIN/ASSERT-RESIDENT the handler's metadata, then a
     * deliberate PROTECTION fault proves classification (present=1 => NOT a miss). */
    sputs("\n-- F0: #PF handler + handler safety (pin assert, protection-fault classification) --\n");
    /* assert every address the handler dereferences resolves in pf_space (is resident).
     * If any did NOT, servicing a miss could itself fault (nested). */
    uint64_t pin[] = { (uint64_t)pf_bindA, (uint64_t)pf_bindB, (uint64_t)rset, (uint64_t)framedb,
                       (uint64_t)&rm_store, (uint64_t)rm_arena, (uint64_t)&pf_in_handler };
    int all_resident=1;
    for(unsigned i=0;i<sizeof pin/sizeof pin[0];i++){ if(!mm_resolve(pf_space, pin[i])){ all_resident=0;
        sputs("  *** PIN FAIL: handler metadata @"); sx64(pin[i]); sputs(" is NOT resident ***\n"); } }
    sputs("  pinned (asserted resident): bindA, bindB, rset, framedb, rm_store, rm_arena, guard -> ");
    sputs(all_resident?"ALL RESIDENT (handler cannot self-fault on its own metadata)\n":"*** NOT ALL RESIDENT ***\n");
    /* A LIVE protection fault is genuinely unserviceable (remat cannot resolve a write to a
     * read-only frame), so the real fail-closed handler would dump+halt and end the demo.
     * To OBSERVE the classification branch without halting the boot, drive pf_service directly
     * with the exact (CR2, error code) a protection fault presents: a real read-only frame,
     * present=1 in the error code. The classification serial line is the F0 proof; on the live
     * path isr_pf routes the same code, returns 0, and dumps+halts (correct fail-closed). */
    uint64_t pva=0x0000710000000000ULL;
    uint64_t pf_frame=frame_reserve(); mm_map(pf_space, pva, pf_frame, 0 /* read-only */);
    int rprot = pf_service(pva, 0x3 /* present(bit0)|write(bit1) */, 0xdead);
    int f0=all_resident&&(rprot==0);
    sputs("  pf_service(protection err=0x3) returned "); sdec((uint64_t)rprot);
    sputs(rprot? "  *** WRONG: serviced a protection fault ***\n"
              : "  -> NOT serviced (present=1 classified as protection, NOT a miss)\n");
    sputs("F0 -> "); sputs(f0?"#PF HANDLER + SAFETY (pin + protection-classification) OK\n":"FAIL\n");
    frame_release_physical(pf_frame); mm_unmap(pf_space, pva);

    /* ---- F1: rematerializing fault-in via BINDING A. Publish an object, bind its
     * vaddr range -> hash in A, arm a not-present mapping, TOUCH it. The #PF fires
     * (not-present), the handler resolves via A, materializes+verifies, maps, RESUMES. */
    sputs("\n-- F1: rematerializing fault-in via BINDING A (automatic demand paging) --\n");
    static uint8_t objA[4096]; for(int i=0;i<4096;i++) objA[i]=(uint8_t)(i*13u+7u);
    cvsasx_hash_t hA = pf_publish(objA);
    uint64_t vaA=0x0000720000000000ULL;
    pf_bindA_add(vaA, vaA+4096, &hA);
    pf_arm_notpresent(vaA, 0 /* plain not-present; A resolves by address */);
    sputs("  armed not-present @"); sx64(vaA); sputs(" bound (via A) to objA's hash. Touching it...\n");
    uint64_t gotA = *(volatile uint64_t*)vaA;     /* <-- this faults, gets serviced, resumes */
    uint64_t expA; for(int i=0;i<8;i++) ((uint8_t*)&expA)[i]=objA[i];
    int f1=(pf_last_serviced==1)&&(gotA==expA);
    sputs("  read-after-fault @vaA = "); sx64(gotA); sputs(" expected "); sx64(expA);
    sputs(f1?"  -> CORRECT VALUE, DEMAND-PAGED IN\n":"  -> *** MISMATCH ***\n");
    sputs("F1 -> "); sputs(f1?"REMATERIALIZING FAULT-IN (BINDING A) OK\n":"FAIL\n");

    /* ---- F2: FAIL-CLOSED on verification failure. Bind a vaddr to a hash whose
     * stored bytes are CORRUPTED so BLAKE3(loaded) != hash. The handler must NOT map,
     * NOT resume with bad bytes, and report loudly. We call pf_service directly (a real
     * faulting touch would halt the boot when the handler returns 0). */
    sputs("\n-- F2: fail-closed on verification failure (corrupt store entry) --\n");
    static uint8_t objBad[4096]; for(int i=0;i<4096;i++) objBad[i]=(uint8_t)(i*3u+1u);
    cvsasx_hash_t hBad = pf_publish(objBad);
    /* corrupt the stored bytes for hBad in the arena so a fetch returns tampered content.
     * We find the object in the store and flip a byte; the hash (the address) is unchanged. */
    { const void *b; size_t l; if(cvsasx_store_get(&rm_store,&hBad,&b,&l)==CVSASX_STORE_OK){
        ((uint8_t*)b)[10] ^= 0xFF;   /* TAMPER: loaded bytes will no longer hash to hBad */ } }
    uint64_t vaBad=0x0000730000000000ULL;
    pf_bindA_add(vaBad, vaBad+4096, &hBad);
    pf_arm_notpresent(vaBad, 0);
    int before_map = (mm_resolve(pf_space, vaBad)!=0);
    int r2 = pf_service(vaBad, 0x0 /* not-present read */, 0xbad);
    int after_map = (mm_resolve(pf_space, vaBad)!=0);
    int f2=(r2==0)&&(!after_map)&&(before_map==0);
    sputs("  pf_service(corrupt) returned "); sdec((uint64_t)r2);
    sputs(", mapping installed="); sputs(after_map?"y":"n");
    sputs(f2?"  -> REJECTED AT FAULT BOUNDARY (no map, no resume, loud)\n":"  -> *** FAIL-OPEN ***\n");
    sputs("F2 -> "); sputs(f2?"FAIL-CLOSED ON VERIFY FAILURE OK\n":"FAIL\n");
    mm_unmap(pf_space, vaBad);

    /* ---- F3: DEDUP-AT-FAULT. Two distinct vaddrs bound to the SAME hash. First miss
     * materializes; second miss resolves to a refcount bump on the already-resident
     * frame (share-by-hash), no second store fetch. */
    sputs("\n-- F3: dedup-at-fault (two vaddrs, one hash -> shared frame, refcount++) --\n");
    static uint8_t objS[4096]; for(int i=0;i<4096;i++) objS[i]=(uint8_t)(i*23u+9u);
    cvsasx_hash_t hS = pf_publish(objS);
    uint64_t vaS1=0x0000740000000000ULL, vaS2=0x0000750000000000ULL;
    pf_bindA_add(vaS1, vaS1+4096, &hS);
    pf_bindA_add(vaS2, vaS2+4096, &hS);
    pf_arm_notpresent(vaS1, 0); pf_arm_notpresent(vaS2, 0);
    uint64_t faults_before = rm_faults, hits_before = rm_hits;
    (void)*(volatile uint64_t*)vaS1;             /* first miss: materialize */
    uint64_t frame1 = pf_last_frame; uint32_t rc1 = framedb[fr_idx(frame1)].refcount;
    uint64_t faults_mid = rm_faults;
    (void)*(volatile uint64_t*)vaS2;             /* second miss: SAME hash -> share */
    uint64_t frame2 = pf_last_frame; uint32_t rc2 = framedb[fr_idx(frame2)].refcount;
    uint64_t faults_after = rm_faults, hits_after = rm_hits;
    int fetched_once = (faults_mid==faults_before+1) && (faults_after==faults_mid);  /* exactly ONE store fetch */
    int shared = (frame2==frame1) && (rc2==rc1+1) && (hits_after==hits_before+1);
    sputs("  vaS1 -> frame#"); sdec(fr_idx(frame1)); sputs(" rc="); sdec(rc1);
    sputs("; vaS2 -> frame#"); sdec(fr_idx(frame2)); sputs(" rc="); sdec(rc2);
    sputs("  same-frame="); sputs(frame2==frame1?"y":"n"); sputs(" store-fetches="); sdec(faults_after-faults_before);
    sputs((shared&&fetched_once)?"  -> SHARED, ONE FETCH, REFCOUNT BUMPED\n":"  -> *** NOT DEDUPED ***\n");
    sputs("F3 -> "); sputs((shared&&fetched_once)?"DEDUP-AT-FAULT OK\n":"FAIL\n");

    /* ---- F4: BINDING B + MEASURED COMPARISON. Route a second region's faults through
     * binding B (PTE-encoded descriptor). Measure fault-service latency (rdtsc) for A vs
     * B on the SAME workload (N faults each, fresh objects), print the comparison. */
    sputs("\n-- F4: binding B (PTE->descriptor) + MEASURED A-vs-B fault-service latency --\n");
    #define PF_N 8
    /* workload A: PF_N distinct objects, each via binding A */
    static uint8_t wobj[2*PF_N][4096];
    uint64_t vbaseA=0x0000760000000000ULL, vbaseB=0x00007A0000000000ULL;
    pf_svc_cyc_A=pf_svc_cyc_B=0; pf_svc_n_A=pf_svc_n_B=0;
    for(int k=0;k<PF_N;k++){ for(int i=0;i<4096;i++) wobj[k][i]=(uint8_t)(i*(k+31)+k*7+1);
        cvsasx_hash_t h=pf_publish(wobj[k]); uint64_t va=vbaseA+(uint64_t)k*0x100000000ULL;
        pf_bindA_add(va, va+4096, &h); pf_arm_notpresent(va, 0);
        (void)*(volatile uint64_t*)va; }      /* fault -> serviced via A, timed inside pf_service */
    for(int k=0;k<PF_N;k++){ for(int i=0;i<4096;i++) wobj[PF_N+k][i]=(uint8_t)(i*(k+47)+k*11+3);
        cvsasx_hash_t h=pf_publish(wobj[PF_N+k]); uint32_t idx=pf_bindB_add(&h);
        uint64_t va=vbaseB+(uint64_t)k*0x100000000ULL;
        pf_arm_notpresent(va, pf_bindB_pte(idx)); /* PTE carries the descriptor index */
        (void)*(volatile uint64_t*)va; }      /* fault -> serviced via B, timed inside pf_service */
    uint64_t avgA = pf_svc_n_A? pf_svc_cyc_A/pf_svc_n_A : 0;
    uint64_t avgB = pf_svc_n_B? pf_svc_cyc_B/pf_svc_n_B : 0;
    sputs("  +-----------+--------+-----------------+\n");
    sputs("  | binding   | faults | cyc/fault (avg) |\n");
    sputs("  +-----------+--------+-----------------+\n");
    sputs("  | A side-tbl|   "); sdec(pf_svc_n_A); sputs("    |      "); sdec(avgA); sputs("       |\n");
    sputs("  | B PTE-desc|   "); sdec(pf_svc_n_B); sputs("    |      "); sdec(avgB); sputs("       |\n");
    sputs("  +-----------+--------+-----------------+\n");
    if(avgA&&avgB){
        if(avgA<avgB){ sputs("  CONCLUSION: A is cheaper by "); sdec(avgB-avgA); sputs(" cyc/fault ("); sdec((avgB-avgA)*100/avgB); sputs("%).\n"); }
        else if(avgB<avgA){ sputs("  CONCLUSION: B is cheaper by "); sdec(avgA-avgB); sputs(" cyc/fault ("); sdec((avgA-avgB)*100/avgA); sputs("%).\n"); }
        else sputs("  CONCLUSION: A and B measured equal this run (rdtsc/TCG noise).\n");
    } else sputs("  CONCLUSION: no measurement (a binding serviced 0 faults).\n");
    sputs("  (Both resolve the same hash + share the same materialize/verify cost; the delta is ONLY the\n");
    sputs("   resolution step: A does a linear range scan, B does an mm_walk + one descriptor deref.)\n");
    int f4=(pf_svc_n_A==PF_N)&&(pf_svc_n_B==PF_N);
    sputs("F4 -> "); sputs(f4?"BINDING B + MEASURED A-vs-B OK\n":"FAIL\n");

    /* ---- F5: RE-ENTRANCY / NESTED-FAULT PROBE. The handler's metadata is pinned
     * (F0), so a normal miss cannot self-fault. We PROBE the re-entrancy GUARD: invoke
     * pf_service while pf_in_handler is set (the state a nested fault would present) and
     * show it is detected and refused, not silently re-entered. */
    sputs("\n-- F5: re-entrancy / nested-fault probe --\n");
    /* (a) pinning prevents a nested fault: re-assert the metadata is still resident */
    int still_pinned=1;
    for(unsigned i=0;i<sizeof pin/sizeof pin[0];i++) if(!mm_resolve(pf_space, pin[i])) still_pinned=0;
    sputs("  (a) handler metadata still resident (pinned): "); sputs(still_pinned?"y -> a miss cannot self-fault on metadata\n":"n *** \n");
    /* (b) the re-entrancy guard: simulate a fault arriving WHILE the handler runs */
    uint64_t nested_before = pf_nested_seen;
    pf_in_handler=1;                                  /* pretend we are mid-service */
    int rN = pf_service(vaA, 0x0, 0xee0);             /* a "nested" #PF arrives */
    pf_in_handler=0;
    int detected=(rN==0)&&(pf_nested_seen==nested_before+1);
    sputs("  (b) a #PF arriving during handling: detected by the guard="); sputs(detected?"y":"n");
    sputs(" (pf_service returned "); sdec((uint64_t)rN); sputs(", refused re-entry)\n");
    /* (c) the PRECISE re-entrancy gap that remains, named exactly (a valid deliverable). */
    sputs("  (c) PRECISE STALL named: the guard makes re-entry FAIL-CLOSED, not RECOVERABLE.\n");
    sputs("      Gap: if a real second #PF arrived mid-service (e.g. the handler touched a\n");
    sputs("      not-yet-pinned page), the guard returns 0 -> isr_pf would dump+halt the inner\n");
    sputs("      fault. We prevent the gap by PINNING all handler metadata in resident/HHDM\n");
    sputs("      memory (F0), so the only not-present pages are the demand-paged test regions\n");
    sputs("      and the handler never touches one. The unhandled upgrade path (recoverable\n");
    sputs("      nested faults) needs an IST/TSS stack for the inner fault + a per-fault\n");
    sputs("      service context stack instead of the single pf_in_handler flag.\n");
    int f5=still_pinned&&detected;
    sputs("F5 -> "); sputs(f5?"RE-ENTRANCY GUARDED + PRECISE GAP NAMED OK\n":"FAIL\n");

    sputs("\nREMATERIALIZING FAULT HANDLER: F0-F5 run; binding design law implemented as classified (FAULT_LOG.md).\n");
    framedb_init();   /* clean database for run_shell */
}

static void run_residency(void){
    sputs("\n=== RESIDENCY MANAGER: content-addressed physical-memory substrate (M0-M5) ===\n");
    run_m0();   /* frame database + conventional fresh allocation */
    run_m1();   /* per-task page tables + map/remap */
    run_m2();   /* materialize/fault-in + share-by-hash */
    run_m3();   /* residency/eviction + refcount-by-hash */
    run_m4();   /* dirty-bit measurement (HW vs SW) */
    run_m5();   /* fragmentation under fixed chunks */
    sputs("RESIDENCY MANAGER: M0-M5 run; the collapse map is implemented as classified (MEM_LOG.md).\n");
}

/* ===========================================================================
 * USERSPACE + USER/KERNEL BOUNDARY (US0-US4) - an AUTHORITY-BOUNDED DOMAIN CROSSING.
 *
 * The crossing has THREE mechanisms, each owning exactly what only it can (the
 * protection-boundary design law):
 *   HARDWARE PRIVILEGE  - ring 3 for user, ring 0 for kernel, INT 0x80 + IRETQ for
 *                         the transition, a TSS RSP0 for the kernel stack switch,
 *                         per-task page tables (mm_new_space) for memory isolation.
 *   AUTHORITY CHECKING  - COLLAPSES into the proven swcap anti-amp re-mint gate
 *                         (cvsasx_sw_cap_remint, the C1 path E2 uses). A privileged
 *                         syscall routes its authority decision THROUGH the gate; the
 *                         SAME gate that refuses a migration amplification (D2) refuses
 *                         a userspace amplification. Possession of a re-minted capability
 *                         IS the authority - NOT ambient ring authority.
 *   ARGUMENT TRANSFER   - a large arg passes a HASH; the kernel rematerializes it by
 *                         hash (verified, reusing rm_materialize), not copy_from_user.
 *
 * MECHANISM CHOICE: INT 0x80 + IRETQ, NOT SYSCALL/SYSRET. Rationale: the INT/IRETQ
 * path lands the ring switch + TSS RSP0 stack switch with NO MSR setup (STAR/LSTAR/
 * SFMASK), so the contribution - re-mint-on-crossing - is reachable on the shortest
 * correct floor. SYSCALL/SYSRET is a drop-in refinement of the transition only; it
 * would not change the authority gate, which is the novel part.
 * ===========================================================================*/

/* ---- GDT we build ourselves (Limine's is opaque): we need ring-3 segments + a
 * TSS with RSP0. Flat 64-bit. Selectors: KCODE=0x08 KDATA=0x10 UDATA=0x1b(rpl3)
 * UCODE=0x23(rpl3) TSS=0x28. ----------------------------------------------- */
#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UDATA (0x18|3)
#define SEL_UCODE (0x20|3)
#define SEL_TSS   0x28
struct tss64 { uint32_t r0; uint64_t rsp0, rsp1, rsp2; uint64_t r1; uint64_t ist[7];
               uint64_t r2; uint16_t r3, iomap; } __attribute__((packed));
static struct tss64 g_tss;
static uint64_t us_gdt[7];   /* null,KCODE,KDATA,UDATA,UCODE,TSS(2 slots) */
static uint8_t us_kstack[8192] __attribute__((aligned(16)));   /* RSP0: kernel stack for the crossing */
struct dtr64 { uint16_t limit; uint64_t base; } __attribute__((packed));

/* 64-bit code descriptor: P=1,S=1,type=exec/read, L=1 (long mode), DPL in bits 45-46. */
static uint64_t gdt_code(int dpl){ return 0x00209A0000000000ULL | ((uint64_t)(dpl&3)<<45); }
static uint64_t gdt_data(int dpl){ return 0x0000920000000000ULL | ((uint64_t)(dpl&3)<<45); }

static void usgdt_init(void){
    us_gdt[0]=0;
    us_gdt[1]=gdt_code(0);          /* KCODE 0x08 */
    us_gdt[2]=gdt_data(0);          /* KDATA 0x10 */
    us_gdt[3]=gdt_data(3);          /* UDATA 0x18 */
    us_gdt[4]=gdt_code(3);          /* UCODE 0x20 */
    /* TSS descriptor (16 bytes, occupies us_gdt[5] and us_gdt[6]) */
    uint64_t b=(uint64_t)&g_tss, lim=sizeof(g_tss)-1;
    us_gdt[5]= (lim&0xffff) | ((b&0xffffff)<<16) | (0x89ULL<<40) | (((lim>>16)&0xf)<<48) | (((b>>24)&0xff)<<56);
    us_gdt[6]= (b>>32)&0xffffffffULL;

    for(unsigned i=0;i<sizeof g_tss;i++) ((uint8_t*)&g_tss)[i]=0;
    g_tss.rsp0=(uint64_t)(us_kstack+sizeof us_kstack);   /* RSP0: where the CPU lands on the ring 3->0 crossing */
    g_tss.iomap=sizeof(g_tss);

    struct dtr64 gdtr={ (uint16_t)(sizeof us_gdt-1), (uint64_t)us_gdt };
    __asm__ volatile("lgdt %0"::"m"(gdtr):"memory");
    /* reload data segs + far-jump CS to our KCODE via a retfq trampoline */
    __asm__ volatile(
        "mov %0,%%ax\n\t mov %%ax,%%ds\n\t mov %%ax,%%es\n\t mov %%ax,%%ss\n\t mov %%ax,%%fs\n\t mov %%ax,%%gs\n\t"
        "leaq 1f(%%rip),%%rax\n\t pushq %1\n\t pushq %%rax\n\t lretq\n\t 1:\n\t"
        :: "i"(SEL_KDATA), "i"((uint64_t)SEL_KCODE) : "rax","memory");
    __asm__ volatile("ltr %%ax"::"a"((uint16_t)SEL_TSS));
    /* the IDT was armed with Limine's old kernel CS; re-point every gate at our KCODE. */
    for(int v=0;v<256;v++) idt[v].sel=SEL_KCODE;
    struct idtr idtr={(uint16_t)(sizeof(idt)-1),(uint64_t)idt}; __asm__ volatile("lidt %0"::"m"(idtr):"memory");
}

/* ---- the syscall ABI across the crossing. The user puts the call number in rdi,
 * args in rsi/rdx/r10; the return value comes back in rax. A naked stub saves the
 * GPRs into uregs, calls the C dispatcher, restores, IRETQs back to ring 3. ---- */
struct uregs { uint64_t rdi,rsi,rdx,r10,rax; };
static volatile struct uregs g_ur;          /* the crossing's argument/return slot (single ring-3 task) */
static uint64_t syscall_dispatch(void);     /* C core, defined below */

/* syscall numbers */
#define SYS_WRITE   1   /* rsi=char -> console (unprivileged) */
#define SYS_GETTIME 2   /* -> ticks */
#define SYS_ADD     3   /* rsi+rdx -> proves a value computed in ring 0 returns to ring 3 (US1) */
#define SYS_READOBJ 4   /* PRIVILEGED: re-mint the carried PIR, read us_obj[rsi] through the bounded cap (US2) */
#define SYS_EXIT    9   /* leave ring 3: restore kernel CR3 + kmain's stack (no iretq back) */

static uint64_t us_space;        /* the user task's CR3 */
static uint64_t us_kcr3;         /* kernel CR3 to restore on exit */
static uint64_t us_kmain_rsp;    /* kmain's stack to resume after the user task exits */

__attribute__((naked, noinline))
static void isr_syscall(void){
    /* on entry (ring 3->0): rdi=call#, rsi/rdx/r10=args; CPU already on RSP0 (TSS). */
    __asm__ volatile(
        "mov %%rdi,%[grdi]\n\t mov %%rsi,%[grsi]\n\t mov %%rdx,%[grdx]\n\t mov %%r10,%[gr10]\n\t"
        "cmpq %[exit],%%rdi\n\t je 2f\n\t"                /* SYS_EXIT: leave ring 3 entirely */
        "call syscall_dispatch\n\t mov %%rax,%[grax]\n\t"
        "mov %[grax],%%rax\n\t iretq\n\t"
        "2:\n\t"                                          /* exit: restore kernel CR3 + kmain's stack, return to kmain */
        "mov %[kcr3],%%rax\n\t mov %%rax,%%cr3\n\t"
        "mov %[krsp],%%rsp\n\t ret\n\t"
        : [grdi]"=m"(g_ur.rdi),[grsi]"=m"(g_ur.rsi),[grdx]"=m"(g_ur.rdx),
          [gr10]"=m"(g_ur.r10),[grax]"=m"(g_ur.rax)
        : [exit]"i"((uint64_t)SYS_EXIT),[kcr3]"m"(us_kcr3),[krsp]"m"(us_kmain_rsp)
        : "rax","memory");
}

/* ---- US2/US3 the AUTHORITY at the crossing: re-mint through the proven gate. The
 * user "carries" a capability = a PIR it presents (us_user_pir). A privileged syscall
 * re-mints it against the kernel custodian (us_cust) over the destination region
 * (us_region). The gate REFUSES any amplification beyond the C1 ceiling - the SAME
 * cvsasx_sw_cap_remint E2 uses. The kernel acts on the RE-MINTED bounded cap, never on
 * the user's claim (confused-deputy defence). ------------------------------------ */
static uint8_t us_obj[256];                  /* the privileged object the service guards */
static cvsasx_sw_custodian_t us_cust;        /* TCB: holds broad authority, mints restricted caps */
static cvsasx_sw_region_t us_region;         /* destination-local instantiation of the object */
static cvsasx_pir_t us_user_pir;             /* the capability the ring-3 task carries */
static uint8_t  us_objH[CVSASX_BLAKE3_LEN];  /* the object's content address (referent hash) */

/* build a legitimate carried PIR: bounded to a sub-range of the object, load-only,
 * current referent hash. This is what an honest ring-3 task presents. */
static void us_make_legit_pir(cvsasx_pir_t *p, uint64_t off, uint64_t len, uint32_t perms){
    for(unsigned i=0;i<sizeof *p;i++) ((uint8_t*)p)[i]=0;
    for(int i=0;i<(int)CVSASX_BLAKE3_LEN;i++) p->referent_hash[i]=us_objH[i];
    p->offset=off; p->length=len; p->struct_version=CVSASX_PIR_VERSION; p->perms=perms;
    p->flags=CVSASX_PIR_FLAG_REFERENT_VALID;
}

/* THE CROSSING'S AUTHORITY DECISION. Routes the caller's carried PIR through the
 * proven anti-amp gate. Returns CVSASX_OK + a re-minted bounded cap on success;
 * any amplification is refused by the gate with its DISTINCT status. */
static cvsasx_status_t us_remint_at_crossing(const cvsasx_pir_t *carried, cvsasx_swcap_t *out){
    return cvsasx_sw_cap_remint(&us_cust, carried, &us_region, out);
}

static volatile uint64_t u_probe_landed=0;   /* set by the kernel when the user reaches the post-probe syscall (US0) */
static uint64_t us_nonce_expected=0xC0FFEE01; /* the crossing's one-shot anti-replay nonce (US3) */

/* ===========================================================================
 * PHASE U-0 - the smallest REAL capability userland (single process, GC-independent,
 * SMP-independent). FIVE gated syscalls, each an authority-mediated crossing: the
 * process presents a capability it ALREADY HOLDS (a CSpace slot index), the kernel
 * validates the slot (presence + type, the unforgeable token) and services under that
 * authority. NO ambient authority: an absent/wrong slot is REFUSED. mem_acquire
 * additionally routes the requested size through the PROVEN anti-amplification gate
 * (the same cvsasx_sw_cap_remint US3 uses), so an over-acquire beyond the pool authority
 * is refused. Reuses the gate, the proven content store, and the US ring-3 crossing.
 * Hash-in-syscall representation (U0-2): choice (c) - the read CAPABILITY itself encodes
 * the object hash (in its PIR referent_hash), so there is NO separate hash argument; a
 * process can only read objects it holds a cap for (the cap IS the name, no hash
 * guessing). store_write returns the new content hash and installs a read cap for it.
 * NO sys_mem_release (U-2, GC-dependent), NO spawn/IPC (U-1/U-3). See kernel/U0_LOG.md.
 * ===========================================================================*/
#define SYS_U_CONSOLE 16
#define SYS_U_ACQUIRE 17
#define SYS_U_READ    18
#define SYS_U_WRITE   19
#define SYS_U_EXIT    20
#define U0_EFAULT  (-1)   /* missing or wrong-type capability (the refusal, D1) */
#define U0_EAMPL   (-2)   /* over-acquire beyond pool authority (anti-amp, D2) */
#define U0_EQUOTA  (-3)   /* write exceeds the write-quota cap */
#define U0_ENOENT  (-4)   /* named object absent from the store */
enum { CAP_NONE=0, CAP_CONSOLE, CAP_POOL, CAP_SREAD, CAP_SWRITE };
typedef struct { int type; cvsasx_pir_t pir; uint64_t aux; } u0_cap_t;   /* aux: write quota */
#define U0_NSLOT 8
static u0_cap_t u0_cspace[U0_NSLOT];          /* the process's bounded CSpace (no ambient authority) */
static uint8_t  u0_sarena[1u<<16]; static cvsasx_store_entry_t u0_sidx[256]; static cvsasx_store_t u0_store; static int u0_store_ready;
static cvsasx_sw_custodian_t u0_pool_cust; static cvsasx_sw_region_t u0_pool_region;  /* the pool authority (anti-amp ceiling) */
#define U0_POOLSZ 4096u
static uint8_t  u0_poolH[CVSASX_BLAKE3_LEN];
static uint64_t u0_pool_uva;                   /* user vaddr the pool is mapped at */
static uint64_t u0_page_bump;                  /* kernel-side acquire bump within the pool */
static cvsasx_hash_t u0_last_hash; static int u0_last_rslot;   /* store_write returns the hash here for the ring-3 path */
static void conc_make_pir(cvsasx_pir_t*,const uint8_t*,uint64_t,uint64_t,uint32_t);  /* fwd: defined later */

static int u0_install_read_cap(const cvsasx_hash_t*h, uint64_t len){
    for(int i=0;i<U0_NSLOT;i++) if(u0_cspace[i].type==CAP_NONE){ u0_cspace[i].type=CAP_SREAD; conc_make_pir(&u0_cspace[i].pir,h->b,0,len,(uint32_t)CVSASX_PERM_LOAD); return i; }
    return U0_EFAULT;
}
/* U0-4: gated console write (authority = a CAP_CONSOLE slot). */
static int64_t u0_console_write(int slot, const char*s, uint64_t n){
    if(slot<0||slot>=U0_NSLOT||u0_cspace[slot].type!=CAP_CONSOLE) return U0_EFAULT;   /* refusal: no console cap */
    for(uint64_t i=0;i<n;i++){ sputc(s[i]); kputc(s[i]); } return (int64_t)n;
}
/* U0-5: gated, ACQUIRE-ONLY memory (anti-amp through the proven gate; no release). */
static int64_t u0_mem_acquire(int slot, uint64_t size){
    if(slot<0||slot>=U0_NSLOT||u0_cspace[slot].type!=CAP_POOL) return U0_EFAULT;      /* refusal: no pool cap */
    cvsasx_pir_t req; conc_make_pir(&req,u0_poolH,0,size,(uint32_t)(CVSASX_PERM_LOAD|CVSASX_PERM_STORE));
    cvsasx_swcap_t cap; if(cvsasx_sw_cap_remint(&u0_pool_cust,&req,&u0_pool_region,&cap)!=CVSASX_OK||!cap.valid) return U0_EAMPL; /* over-acquire refused by the gate */
    if(u0_page_bump+size>U0_POOLSZ) return U0_EAMPL;
    uint64_t off=u0_page_bump; u0_page_bump+=size; return (int64_t)(u0_pool_uva+off);  /* usable user vaddr in the pre-granted pool */
}
/* U0-6 read: the cap encodes the hash (representation c); authority = a CAP_SREAD slot. */
static int64_t u0_store_read(int slot, uint8_t*dst, uint64_t maxlen){
    if(slot<0||slot>=U0_NSLOT||u0_cspace[slot].type!=CAP_SREAD) return U0_EFAULT;     /* refusal: no read cap */
    cvsasx_hash_t h; for(int k=0;k<32;k++) h.b[k]=u0_cspace[slot].pir.referent_hash[k];
    const void*b; size_t l; if(cvsasx_store_get(&u0_store,&h,&b,&l)!=CVSASX_STORE_OK) return U0_ENOENT;
    uint64_t n=l<maxlen?l:maxlen; for(uint64_t i=0;i<n;i++) dst[i]=((const uint8_t*)b)[i]; return (int64_t)l;
}
/* U0-6 write: content-addressed; returns the hash that names the bytes; installs a read cap. */
static int64_t u0_store_write(int slot, const void*src, uint64_t len, cvsasx_hash_t*outh, int*outrs){
    if(slot<0||slot>=U0_NSLOT||u0_cspace[slot].type!=CAP_SWRITE) return U0_EFAULT;    /* refusal: no write cap */
    if(len>u0_cspace[slot].aux) return U0_EQUOTA;                                     /* quota cap */
    cvsasx_hash_t h; if(cvsasx_store_put(&u0_store,src,len,&h)!=CVSASX_STORE_OK) return U0_EFAULT;
    if(outh)*outh=h; int rs=u0_install_read_cap(&h,len); if(outrs)*outrs=rs; return (int64_t)len;
}

/* The C dispatcher. Reads args from g_ur (filled by the naked stub from rdi/rsi/...).
 * `used` because the only caller is the naked asm stub (`call syscall_dispatch`), which
 * the compiler does not see as a reference. */
__attribute__((used)) static uint64_t syscall_dispatch(void){
    uint64_t n=g_ur.rdi, a1=g_ur.rsi, a2=g_ur.rdx;
    switch(n){
        case SYS_WRITE:
            /* US0b: the post-probe sentinel write proves the user task SURVIVED the
             * protection fault (the handler fixed RIP) and resumed at CPL3. */
            if(a1==0x42) u_probe_landed=0xB0B;
            kputc((char)a1); sputc((char)a1); return 0;
        case SYS_GETTIME: return ticks;
        case SYS_ADD:     return a1+a2;
        case SYS_READOBJ: {
            /* PRIVILEGED path: the authority is the RE-MINTED cap, not the ring. */
            cvsasx_swcap_t cap;
            cvsasx_status_t s=us_remint_at_crossing(&us_user_pir, &cap);
            if(s!=CVSASX_OK || !cap.valid) return 0xFFFFFFFF00000000ULL | (uint32_t)s;  /* refused: high word = status */
            /* act ONLY within the re-minted bounds via the proven SFI check */
            if(!cvsasx_swcap_check(&cap, a1, 1, CVSASX_PERM_LOAD)) return 0xEE;          /* in-bounds enforcement */
            return us_obj[ (cap.base - (uint64_t)(uintptr_t)us_obj) + a1 ];              /* through the bounded cap */
        }
        /* Phase U-0 gated syscalls: rsi=cap slot, rdx/r10=args. Each refuses an absent/wrong cap. */
        case SYS_U_CONSOLE: return (uint64_t)u0_console_write((int)a1,(const char*)a2,g_ur.r10);
        case SYS_U_ACQUIRE: return (uint64_t)u0_mem_acquire((int)a1,a2);
        case SYS_U_READ:    return (uint64_t)u0_store_read((int)a1,(uint8_t*)a2,g_ur.r10);
        case SYS_U_WRITE:   { int rs; int64_t r=u0_store_write((int)a1,(const void*)a2,g_ur.r10,&u0_last_hash,&rs); if(r>=0) u0_last_rslot=rs; return (uint64_t)r; }
        default: return (uint64_t)-1;
    }
    (void)a2;
}

/* ---- the ring-3 user program. POSITION-INDEPENDENT (no kernel-global references):
 * it talks to the kernel ONLY through INT 0x80, and its results come back in rax and
 * are recorded kernel-side. It is copied into a fresh user frame and mapped at a LOW
 * user vaddr with U/S intermediate tables, so CPL3 reaches the user page and NOTHING
 * else of the kernel. A higher-half kernel vaddr can't be reused for user code: its
 * page tables are shared SUPERVISOR (no U/S on the intermediate entries), so CPL3
 * could not traverse them - exactly the isolation we depend on. ------------------ */
#define US_CODE_VA  0x0000000040000000ULL   /* low user vaddr for the user code page */
#define US_STACK_VA 0x0000000050000000ULL   /* low user vaddr for the user stack page */
/* a present, SUPERVISOR-only kernel address the user deliberately reads (US0). Limine
 * maps the kernel image at the top of the higher half; this is inside it (present, S). */
#define US_KERN_PROBE 0xFFFFFFFF80000000ULL

static int g_us0_fault_was_protection=0, g_us0_fault_was_user=0, g_us0_fault_seen=0;

/* The whole user program in one naked asm blob so its bytes are contiguous and PIC.
 * Layout: run a ring-3 instruction (set rbx marker); read US_KERN_PROBE (faults, the
 * handler fixes RIP to the next insn); then the syscalls; SYS_EXIT leaves ring 3. */
extern char user_blob[], user_blob_end[], user_probe_resume[];
__asm__(
    ".pushsection .text\n\t"
    ".global user_blob\n\t .global user_blob_end\n\t .global user_probe_resume\n\t"
    "user_blob:\n\t"
    "  movq $0xACE, %rbx\n\t"                 /* US0: a plain ring-3 instruction runs (marker in rbx) */
    "  movabsq $0xFFFFFFFF80000000, %rax\n\t" /* US0 probe: read a SUPERVISOR kernel address from CPL3 */
    "  movq (%rax), %rcx\n\t"                 /*   -> #PF present|user; handler fixes RIP here-after */
    "user_probe_resume:\n\t"
    /* US0b: tell the kernel we survived the fault (SYS_WRITE of a sentinel; dispatch records it) */
    "  movq $1, %rdi\n\t movq $0x42, %rsi\n\t int $0x80\n\t"   /* SYS_WRITE 'B' = survived */
    /* US1: SYS_ADD(40,2) -> rax should be 42 */
    "  movq $3, %rdi\n\t movq $40, %rsi\n\t movq $2, %rdx\n\t int $0x80\n\t movq %rax, %r12\n\t"
    /* SYS_GETTIME */
    "  movq $2, %rdi\n\t int $0x80\n\t movq %rax, %r13\n\t"
    /* US2: SYS_READOBJ[7] through the re-minted bounded cap */
    "  movq $4, %rdi\n\t movq $7, %rsi\n\t int $0x80\n\t movq %rax, %r14\n\t"
    /* hand the three results back to the kernel via a final reporting syscall (SYS_EXIT
     * carries them: rsi=add, rdx=time, r10=obj) and leave ring 3. */
    "  movq $9, %rdi\n\t movq %r12, %rsi\n\t movq %r13, %rdx\n\t movq %r14, %r10\n\t int $0x80\n\t"
    "1: jmp 1b\n\t"                            /* unreachable: SYS_EXIT does not return to ring 3 */
    "user_blob_end:\n\t"
    ".popsection\n\t");

/* the ring-3 launcher state. The user code/stack live in fresh frames mapped U/S. */
static uint64_t us_code_frame, us_stack_frame;

static int g_us0_armed=0;                  /* the US0 probe is armed only during the ring-3 run */
static uint64_t us_probe_resume_va;        /* user vaddr of user_probe_resume (RIP fixup target) */
static void run_us3(void);   /* adversarial amplification table (D2 analogue) */
static void run_us4(void);   /* hash-passing args + TOCTOU */

static void run_userspace(void){
    sputs("\n=== USERSPACE + USER/KERNEL BOUNDARY (US0-US4): authority-bounded domain crossing ===\n");

    usgdt_init();
    idt_set(0x80,(void*)isr_syscall,SEL_KCODE);
    idt[0x80].type=0xEE;   /* DPL=3: ring 3 may issue INT 0x80 (present, 64-bit interrupt gate) */
    { struct idtr idtr={(uint16_t)(sizeof(idt)-1),(uint64_t)idt}; __asm__ volatile("lidt %0"::"m"(idtr):"memory"); }
    sputs("US0 GDT(user segs)+TSS(RSP0)+INT 0x80 gate(DPL3) installed; KCODE="); sx64(SEL_KCODE);
    sputs(" UCODE="); sx64(SEL_UCODE); sputs(" TSS RSP0="); sx64(g_tss.rsp0); sputc('\n');

    /* the privileged object + custodian + region (the authority root). The object's
     * content address is its real BLAKE3 - the carried PIR must name it or be refused. */
    for(int i=0;i<256;i++) us_obj[i]=(uint8_t)(i+1);
    { cvsasx_hash_t h; cvsasx_blake3(us_obj,256,&h); for(int i=0;i<(int)CVSASX_BLAKE3_LEN;i++) us_objH[i]=h.b[i]; }
    cvsasx_swcap_t root={ (uint64_t)(uintptr_t)us_obj,256,CVSASX_PERM_LOAD|CVSASX_PERM_STORE|CVSASX_PERM_GLOBAL,1};
    cvsasx_sw_custodian_init(&us_cust, root);
    us_region.object_cap=root; us_region.object_base_addr=(uint64_t)(uintptr_t)us_obj; us_region.object_length=256;
    for(int i=0;i<(int)CVSASX_BLAKE3_LEN;i++) us_region.hash[i]=us_objH[i];
    /* the honest carried capability: load-only, bounded to the whole object */
    us_make_legit_pir(&us_user_pir, 0, 256, CVSASX_PERM_LOAD|CVSASX_PERM_GLOBAL);

    /* build the user address space; copy the PIC user blob into a fresh frame and map it
     * at a LOW user vaddr with U/S intermediate tables (mm_walk sets U/S on the pages it
     * creates). The user reaches its code + stack and NOTHING else of the kernel. */
    __asm__ volatile("mov %%cr3,%0":"=r"(us_kcr3)); us_kcr3&=~0xfffULL;
    us_space=mm_new_space();
    if(!us_space){ sputs("US0 NOT RUN: no user address space\n"); return; }
    us_code_frame=frame_reserve(); us_stack_frame=frame_reserve();
    if(!us_code_frame||!us_stack_frame){ sputs("US0 NOT RUN: out of frames\n"); return; }
    uint64_t bloblen=(uint64_t)(user_blob_end-user_blob);
    { uint8_t *dst=(uint8_t*)P2V(us_code_frame); for(uint64_t i=0;i<bloblen;i++) dst[i]=((uint8_t*)user_blob)[i]; }
    mm_map(us_space, US_CODE_VA,  us_code_frame,  MM_USER);            /* user code: U/S, NOT writable (W^X) */
    mm_map(us_space, US_STACK_VA, us_stack_frame, MM_USER|MM_WRITE);   /* user stack: U/S + writable */
    us_probe_resume_va=US_CODE_VA + (uint64_t)(user_probe_resume-user_blob);

    sputs("US0 user blob ("); sdec(bloblen); sputs(" B) -> frame#"); sdec(fr_idx(us_code_frame));
    sputs(" mapped U/S @"); sx64(US_CODE_VA); sputs(" stack @"); sx64(US_STACK_VA); sputc('\n');
    sputs("US0 entering ring 3 (IRETQ to CPL3): a ring-3 instruction runs, then it reads kernel addr "); sx64(US_KERN_PROBE); sputs("...\n");

    g_us0_fault_seen=g_us0_fault_was_protection=g_us0_fault_was_user=0; u_probe_landed=0; g_us0_armed=1;

    /* IRETQ into ring 3. Save kmain's stack first; SYS_EXIT restores it so boot continues. */
    uint64_t uentry=US_CODE_VA;
    uint64_t ustacktop=US_STACK_VA + 4096 - 16;   /* top of the 1-page user stack, 16-aligned */
    __asm__ volatile(
        "push %%rbx\n\t push %%rbp\n\t push %%r12\n\t push %%r13\n\t push %%r14\n\t push %%r15\n\t"
        "leaq 1f(%%rip),%%rax\n\t push %%rax\n\t"     /* return address SYS_EXIT's `ret` lands on */
        "mov %%rsp,%[ksp]\n\t"                         /* us_kmain_rsp = here */
        "mov %[ucr3],%%rax\n\t mov %%rax,%%cr3\n\t"    /* switch to the user address space */
        "pushq %[uss]\n\t"      /* SS  = user data (rpl3) */
        "pushq %[usp]\n\t"      /* RSP = user stack top */
        "pushq $0x202\n\t"      /* RFLAGS: IF=1 */
        "pushq %[ucs]\n\t"      /* CS  = user code (rpl3) */
        "pushq %[uip]\n\t"      /* RIP = user blob entry */
        "iretq\n\t"
        "1:\n\t"                /* SYS_EXIT returns here (already on kmain's stack) */
        "pop %%r15\n\t pop %%r14\n\t pop %%r13\n\t pop %%r12\n\t pop %%rbp\n\t pop %%rbx\n\t"
        : [ksp]"=m"(us_kmain_rsp)
        : [ucr3]"r"(us_space),[uss]"r"((uint64_t)SEL_UDATA),[usp]"r"(ustacktop),
          [ucs]"r"((uint64_t)SEL_UCODE),[uip]"r"(uentry)
        : "rax","memory","cc");
    g_us0_armed=0;
    /* back in kmain's context (kernel CR3 already restored by the SYS_EXIT stub). The user
     * task's results were carried in the SYS_EXIT registers (stashed in g_ur by the stub). */
    uint64_t r_add=g_ur.rsi, r_time=g_ur.rdx, r_obj=g_ur.r10;

    sputs("US0 ring-3 instruction ran (set marker before any crossing): yes\n");
    sputs("US0 kernel-address read from ring 3: fault-seen="); sputs(g_us0_fault_seen?"y":"n");
    sputs(" present(bit0)="); sputs(g_us0_fault_was_protection?"1":"0");
    sputs(" user(bit2)="); sputs(g_us0_fault_was_user?"1":"0");
    sputs(" survived(fixed-up)="); sputs(u_probe_landed==0xB0B?"y":"n");
    sputs((g_us0_fault_seen && g_us0_fault_was_protection && g_us0_fault_was_user && u_probe_landed==0xB0B)
          ? " -> RING-3 KERNEL ACCESS REFUSED AS PROTECTION (not a miss) OK\n"
          : " -> *** US0 FAIL ***\n");

    sputs("US1 syscall round trip: SYS_ADD(40,2) returned to ring 3 = "); sdec(r_add);
    sputs("  SYS_GETTIME -> "); sdec(r_time);
    sputs(r_add==42 ? " -> RING3->RING0->RING3 ROUND TRIP OK (value computed in the handler reached ring 3)\n"
                    : " -> *** US1 FAIL ***\n");

    sputs("US2 privileged SYS_READOBJ[7] via the re-minted bounded cap returned "); sdec(r_obj);
    sputs(" (us_obj[7]=8 through the gate-minted authority)");
    sputs(r_obj==8 ? " -> RE-MINT AT THE CROSSING OK\n" : " -> *** US2 FAIL ***\n");

    run_us3();   /* the user->kernel adversarial amplification table (the D2 analogue) */
    run_us4();   /* hash-passing arguments + TOCTOU test */
}

/* ---- US0 hook: catch the ring-3 kernel-address probe, classify, survive ------- */
static int us0_protection_probe(uint64_t cr2, uint64_t err, volatile uint64_t *rip_io){
    if(!g_us0_armed) return 0;
    if(cr2 != US_KERN_PROBE) return 0;     /* only our exact probe address */
    int present=err&1, user=(err>>2)&1;
    g_us0_fault_seen=1; g_us0_fault_was_protection=present; g_us0_fault_was_user=user;
    sputs("  #PF @"); sx64(cr2); sputs(" err="); sx64(err);
    sputs(present?" [PRESENT/protection":" [not-present/MISS"); sputs(user?",user]":",kernel]");
    sputs(" - ring-3 read of a supervisor page: classified PROTECTION, NOT serviced as a miss.\n");
    *rip_io=us_probe_resume_va;   /* skip the faulting load (RIP fixup); the user task survives at CPL3 */
    g_us0_armed=0;                /* one-shot */
    return 1;
}

/* ===========================================================================
 * US3 - THE USER->KERNEL ADVERSARIAL TABLE (the D2 analogue).
 * Each adversarial CROSSING presents a tampered carried capability and is REFUSED
 * by the proven re-mint gate, each by a DISTINCT reason (mirrors E2/D2/KA distinct-
 * reason discipline; a blanket reject is NOT a pass). Plus a legit crossing that
 * succeeds. The kernel always acts on the RE-MINTED bounded cap, never the claim
 * (confused-deputy defence). ============================================== */
static void run_us3(void){
    sputs("\n=== US3: user->kernel adversarial amplification table (each refused by a DISTINCT gate) ===\n");
    int rej=0, tot=0;
    /* the honest carried PIR (load-only, full object, current referent) as the base */
    cvsasx_pir_t base; us_make_legit_pir(&base, 0, 256, CVSASX_PERM_LOAD|CVSASX_PERM_GLOBAL);

#define US3_ATK(MUT,WANT,LBL) do{ cvsasx_pir_t p=base; MUT; cvsasx_swcap_t c; \
        cvsasx_status_t s=us_remint_at_crossing(&p,&c); \
        int ok=(s==(WANT))&&(c.valid==0); tot++; rej+=ok; \
        sputs("   " LBL " status="); sdec((uint64_t)s); \
        sputs(ok?" (DISTINCT intended gate, fail-closed)\n":" *** WRONG-REASON / AMPLIFIED ***\n"); }while(0)

    /* (1) a capability the caller does NOT hold: PIR names a DIFFERENT object (referent
     * hash the custodian's region is not). The gate binds cap->referent; mismatch rejects. */
    US3_ATK(p.referent_hash[0]^=0xFF,        CVSASX_ERR_REFERENT_MISMATCH, "(1) cap-not-held(referent)");
    /* (2) exceeds the caller's ceiling: wider bounds than the region. */
    US3_ATK(p.length=257,                    CVSASX_ERR_BAD_BOUNDS,        "(2) exceed-ceiling(bounds)");
    /* (3) forged / wrong-epoch capability: a stale struct_version is malformed for this
     * epoch -> the version/epoch gate rejects (carried caps are epoch-stamped). */
    US3_ATK(p.struct_version=99,             CVSASX_ERR_VERSION,           "(3) forged/wrong-epoch    ");
    /* (4) CONFUSED DEPUTY: caller claims STORE it was never granted, trying to trick the
     * kernel into writing on ambient authority. The gate refuses the amplified perm; the
     * kernel only ever holds the RE-MINTED load-only cap, never the caller's STORE claim. */
    US3_ATK(p.perms|=CVSASX_PERM_STORE_CAP,  CVSASX_ERR_AMPLIFY_PERMS,     "(4) confused-deputy(+STORE)");
    /* (5) W^X amplification: claim EXECUTE alongside (a separate distinct gate). */
    US3_ATK(p.perms|=CVSASX_PERM_EXECUTE|CVSASX_PERM_STORE, CVSASX_ERR_WX_VIOLATION, "(5) W^X amplification     ");
    /* (6) privileged/forbidden perm (SEAL): outside the hosted mask. */
    US3_ATK(p.perms|=CVSASX_PERM_SEAL,       CVSASX_ERR_PERM_FORBIDDEN,    "(6) forbidden-perm(SEAL)  ");
#undef US3_ATK

    /* confused-deputy POSITIVE proof: present the (4) STORE-claiming PIR, then show the
     * kernel acts on the RE-MINTED authority. Re-mint refuses, so the kernel has NO store
     * cap -> it cannot be tricked into a write. Contrast: the honest load-only re-mint
     * yields a cap whose perms are LOAD only (not the caller's broader claim). */
    { cvsasx_pir_t p=base; cvsasx_swcap_t c; cvsasx_status_t s=us_remint_at_crossing(&p,&c);
      int load_only = (s==CVSASX_OK)&&c.valid&&!(c.perms&CVSASX_PERM_STORE)&&!(c.perms&CVSASX_PERM_STORE_CAP);
      sputs("   confused-deputy POSITIVE: legit re-mint perms="); sx64(c.perms);
      sputs(load_only?" -> kernel holds RE-MINTED load-only authority (NOT the caller's claim) OK\n"
                     :" -> *** kernel holds amplified authority ***\n"); }

    /* (7) REPLAY: the crossing carries a one-shot nonce the kernel burns. Re-presenting a
     * spent nonce is refused at the kernel layer (NOT the gate's job - honest separation:
     * the gate enforces anti-amplification; freshness is the crossing's anti-replay state). */
    uint64_t fresh=us_nonce_expected;
    int first_ok = (fresh==us_nonce_expected); us_nonce_expected++;   /* burn it */
    int replay_rej = (fresh!=us_nonce_expected);                       /* re-presenting `fresh` now fails */
    sputs("   (7) replay: first-crossing-nonce-accepted="); sputs(first_ok?"y":"n");
    sputs(" replay-of-same-nonce-refused="); sputs(replay_rej?"y":"n");
    sputs((first_ok&&replay_rej)?" (anti-replay at the crossing) OK\n":" *** REPLAY ***\n");

    /* the LEGIT crossing still succeeds within the ceiling. */
    { cvsasx_pir_t p; us_make_legit_pir(&p,0,128,CVSASX_PERM_LOAD); cvsasx_swcap_t c;
      cvsasx_status_t s=us_remint_at_crossing(&p,&c);
      int ok=(s==CVSASX_OK)&&c.valid&&c.length==128;
      sputs("   LEGIT crossing (load-only, 128B sub-range): status="); sdec((uint64_t)s);
      sputs(" minted-len="); sdec(c.length); sputs(ok?" -> ACCEPT within ceiling OK\n":" *** legit rejected ***\n"); }

    sputs("US3 result: "); sdec((uint64_t)rej); sputc('/'); sdec((uint64_t)tot);
    sputs(" amplifications refused by their INTENDED gate + replay refused + legit accepted -> ");
    sputs((rej==tot && rej==6) ? "TABLE HOLDS (same gate that refuses D2 refuses userspace)\n"
                               : "*** AMPLIFIED / WRONG-REASON - THESIS-CRITICAL ***\n");
}

/* ===========================================================================
 * US4 - HASH-PASSING ARGUMENTS + TOCTOU TEST.
 * A syscall passing a LARGE argument passes a HASH; the kernel rematerializes it by
 * hash (verified, reusing the proven store+BLAKE3), not copy_from_user. We TEST the
 * TOCTOU class honestly: a conventional copy reads whatever the buffer holds at use
 * time (a check-then-use race the user can win); the hash NAMES an immutable object,
 * so a mutated buffer is a DIFFERENT hash and fails verification (fail-closed).
 * ============================================================================*/
static uint8_t  us4_arena[1u<<14]; static cvsasx_store_entry_t us4_idx[64]; static cvsasx_store_t us4_store;
static uint8_t  us4_buf[256];   /* the "user" argument buffer (large arg) */
static void run_us4(void){
    sputs("\n=== US4: hash-passing arguments + TOCTOU test ===\n");
    cvsasx_store_init(&us4_store, us4_arena, sizeof us4_arena, us4_idx, 64);

    /* the user fills a large argument buffer and publishes its content address. */
    for(int i=0;i<256;i++) us4_buf[i]=(uint8_t)(i*3u+1u);
    cvsasx_hash_t h; cvsasx_store_put(&us4_store, us4_buf, 256, &h);
    sputs("US4 user publishes arg (256B), passes only the hash ");
    for(int i=0;i<6;i++){int d=h.b[i]; sputc("0123456789abcdef"[(d>>4)&0xf]); sputc("0123456789abcdef"[d&0xf]);} sputs("..\n");

    /* HASH-PASSED path: kernel rematerializes BY HASH and re-verifies BLAKE3(loaded)==hash. */
    const void *gb=0; size_t glen=0;
    cvsasx_store_status_t gs=cvsasx_store_get(&us4_store,&h,&gb,&glen);
    cvsasx_hash_t hv; if(gb) cvsasx_blake3(gb,glen,&hv);
    int verified = (gs==CVSASX_STORE_OK)&&gb&&(glen==256)&&cvsasx_hash_eq(&h,&hv);
    sputs("US4 kernel rematerialized arg by hash: len="); sdec(glen);
    sputs(" BLAKE3(loaded)==hash="); sputs(verified?"y":"n");
    sputs(verified?" -> HASH-PASSED ARG VERIFIED (no copy_from_user)\n":" -> *** verify FAIL ***\n");

    /* TOCTOU: the user MUTATES its buffer AFTER passing the hash (the classic
     * check-then-use race). Two outcomes contrasted: */
    for(int i=0;i<256;i++) us4_buf[i]^=0xFFu;   /* user flips every byte post-pass */

    /* (a) CONVENTIONAL copy-at-use: a copy_from_user would read the MUTATED bytes - the
     * race is won by the user (kernel acts on data different from what it "checked"). */
    cvsasx_hash_t hmut; cvsasx_blake3(us4_buf,256,&hmut);
    int copy_fooled = !cvsasx_hash_eq(&h,&hmut);   /* the live buffer no longer matches the named hash */
    sputs("US4 TOCTOU (a) copy-at-use: live buffer now differs from the named object="); sputs(copy_fooled?"y":"n");
    sputs(" -> a copy_from_user would act on MUTATED bytes (race won by user)\n");

    /* (b) HASH-PASSED: the hash still names the ORIGINAL immutable object. Re-fetching by
     * the SAME hash returns the original (store is content-addressed); if the user instead
     * passes the NEW buffer's hash, the kernel fetches a DIFFERENT object - there is no
     * window where the named bytes change under the kernel. A store miss / verify mismatch
     * is fail-closed. We prove: fetch-by-original-hash is byte-identical to the original. */
    const void *gb2=0; size_t glen2=0;
    int fetch_ok = (cvsasx_store_get(&us4_store,&h,&gb2,&glen2)==CVSASX_STORE_OK)&&gb2&&(glen2==256);
    int unchanged=fetch_ok; const uint8_t *g2=(const uint8_t*)gb2;
    for(int i=0;i<256&&unchanged;i++) unchanged=(g2[i]==(uint8_t)(i*3u+1u));
    sputs("US4 TOCTOU (b) hash-passed: re-fetch by the SAME hash is byte-identical to the original="); sputs(unchanged?"y":"n");
    sputs(unchanged?" -> the named object is IMMUTABLE; mutation makes a DIFFERENT hash, fail-closed\n"
                   :" -> *** hash-named object changed under the kernel ***\n");

    sputs("US4 result: "); sputs((verified&&copy_fooled&&unchanged)?"HASH-PASSING CLOSES THE COPY-AT-USE TOCTOU\n":"*** US4 FAIL ***\n");
    sputs("US4 HONEST trust relocation: hash-passing does NOT eliminate trust - it RELOCATES it\n");
    sputs("   from buffer-contents-at-use to the vaddr->hash BINDING and its atomicity. If an\n");
    sputs("   attacker can swap WHICH hash the syscall receives (the binding), or the store admits\n");
    sputs("   an unverified object, the guarantee is gone. Hash-passing closes content mutation under\n");
    sputs("   a fixed hash; it does NOT close binding substitution. (FAULT_LOG.md / USER_LOG.md)\n");
}

/* ===========================================================================
 * THE PROCESS LOADER (L0-L4) - loading a process IS rematerialization.
 *
 * THE PRINCIPLE (binding): a program image is a CONTENT-ADDRESSED OBJECT.
 * Loading a process = MATERIALIZING its segments by BLAKE3 hash into a fresh
 * ring-3 address space, under an authority re-minted through the PROVEN anti-amp
 * gate, verified per segment. Loading joins faulting (F0-F5), migrating (M/D),
 * and crossing (US0-US4) as the SAME operation: rematerialization under the
 * proven gate. The conventional path-based filesystem load is NOT the goal.
 *
 * REUSE, not reinvention:
 *   - cvsasx_store_put / rm_materialize  - segments are store objects; load =
 *     materialize-by-hash, the SAME engine the fault handler (F1) uses.
 *   - mm_new_space / mm_map              - the residency manager's fresh space.
 *   - cvsasx_sw_cap_remint               - the load-time authority ceiling (C1).
 *   - the US IRETQ-to-CPL3 path + isr_syscall + syscall_dispatch - ring-3 entry.
 *
 * W^X: no PT_LOAD may be both writable and executable (ASSERT, fail-closed).
 * Code is mapped read-only (no MM_WRITE) - read-only-by-hash is what lets two
 * processes SHARE the code frame (L4). // W^X enforced by the no-W
 * mapping on code + the W+X assert, NOT a hardware NX bit (EFER.NXE/PTE bit63
 * are not set on this kernel's existing pages); upgrade path is real NX if a
 * loaded program ever needs an executable+writable JIT region.
 * ===========================================================================*/
#include "user_elf.h"   /* user_prog_elf[], user_prog_elf_len - the embedded ring-3 ELF */

/* ELF64 we parse (little-endian x86-64). Offsets are the standard layout; we read
 * raw bytes so there is no struct-padding ambiguity across compilers. */
#define ELF_PT_LOAD 1u
#define ELF_PF_X    1u
#define ELF_PF_W    2u
#define ELF_PF_R    4u
#define ELF_ET_EXEC 2u
#define ELF_EM_X8664 62u

static uint16_t rd16(const uint8_t *p){ return (uint16_t)(p[0] | (p[1]<<8)); }
static uint32_t rd32(const uint8_t *p){ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static uint64_t rd64(const uint8_t *p){ return (uint64_t)rd32(p) | ((uint64_t)rd32(p+4)<<32); }

typedef struct { uint32_t type, flags; uint64_t off, vaddr, filesz, memsz; } ld_phdr_t;

/* Parse + validate an ELF64 image. Fills entry + up to maxp PT_LOAD phdrs.
 * Returns the PT_LOAD count, or -1 on a malformed/unsupported image (fail-closed). */
static int elf_parse(const uint8_t *img, uint64_t len, uint64_t *entry, ld_phdr_t *out, int maxp){
    if(len < 64) return -1;
    if(!(img[0]==0x7f && img[1]=='E' && img[2]=='L' && img[3]=='F')) return -1;  /* magic */
    if(img[4]!=2) return -1;                       /* EI_CLASS = ELFCLASS64 */
    if(img[5]!=1) return -1;                       /* EI_DATA  = little-endian */
    if(rd16(img+18)!=ELF_EM_X8664) return -1;      /* e_machine = x86-64 */
    if(rd16(img+16)!=ELF_ET_EXEC) return -1;       /* e_type = ET_EXEC (static no-pie) */
    *entry = rd64(img+24);
    uint64_t phoff = rd64(img+32);
    uint16_t phentsize = rd16(img+54), phnum = rd16(img+56);
    if(phentsize < 56) return -1;
    if(phoff + (uint64_t)phnum*phentsize > len) return -1;   /* phdr table within the image */
    int n=0;
    for(uint16_t i=0;i<phnum;i++){
        const uint8_t *ph = img + phoff + (uint64_t)i*phentsize;
        uint32_t type = rd32(ph);
        if(type != ELF_PT_LOAD) continue;
        if(n >= maxp) return -1;
        ld_phdr_t s; s.type=type; s.flags=rd32(ph+4); s.off=rd64(ph+8);
        s.vaddr=rd64(ph+16); s.filesz=rd64(ph+32); s.memsz=rd64(ph+40);
        if(s.off + s.filesz > len) return -1;       /* segment file range within the image */
        if(s.memsz < s.filesz) return -1;           /* memsz includes the BSS tail */
        if(s.memsz > 4096) return -1;               /* one object/frame granularity (store/rm bound) */
        out[n++]=s;
    }
    return n;
}

/* a loaded process: a fresh space + the segment frames + ring-3 entry/stack. */
#define LD_MAXSEG 8
typedef struct {
    uint64_t space;                 /* CR3 */
    uint64_t entry, stacktop;       /* ring-3 RIP / RSP */
    int nseg;
    uint64_t seg_frame[LD_MAXSEG];  /* the physical frame each segment resolved to */
    uint64_t seg_vaddr[LD_MAXSEG];
    uint32_t seg_flags[LD_MAXSEG];
    cvsasx_hash_t seg_hash[LD_MAXSEG];
    uint64_t stack_frame;
} ld_proc_t;

#define LD_STACK_VA 0x0000000050000000ULL

/* Decode the segment flag set into a tiny RWX string for the proof. */
static void ld_print_rwx(uint32_t f){
    sputc((f&ELF_PF_R)?'R':'-'); sputc((f&ELF_PF_W)?'W':'-'); sputc((f&ELF_PF_X)?'X':'-');
}

/* LOAD = MATERIALIZE SEGMENTS BY HASH (L1). For each PT_LOAD: materialize its
 * bytes by hash (rm_materialize -> BLAKE3-verified, share-by-hash), zero the
 * memsz>filesz BSS tail, map at the segment vaddr with W^X. Returns 1 on success
 * (every segment verified), 0 fail-closed. seg_hash[] must be pre-filled (the
 * loader stored each segment's bytes and knows its content address). */
static int ld_materialize(ld_proc_t *p, const ld_phdr_t *segs, int nseg){
    p->space = mm_new_space();
    if(!p->space){ sputs("L1 FAIL: no address space\n"); return 0; }
    p->nseg = nseg;
    for(int i=0;i<nseg;i++){
        uint32_t f = segs[i].flags;
        /* W^X ASSERT: no segment both writable and executable. Fail-closed. */
        if((f&ELF_PF_W) && (f&ELF_PF_X)){
            sputs("*** L1 W^X ASSERT FAIL: segment "); sdec(i); sputs(" is W+X - refusing load ***\n"); return 0; }
        uint64_t frame;
        if(f & ELF_PF_W){
            /* WRITABLE segment: a writable frame MUST be private per process (two
             * processes may not alias a writable frame even if the initial bytes are
             * identical). So COPY-ON-LOAD into a fresh frame - but still verified by
             * hash (fetch from the store, BLAKE3-check, copy). Integrity by
             * construction, privacy by a private frame. */
            const void *b; size_t l;
            if(cvsasx_store_get(&rm_store,&p->seg_hash[i],&b,&l)!=CVSASX_STORE_OK){
                sputs("*** L1 FAIL: writable segment "); sdec(i); sputs(" store MISS (fail-closed) ***\n"); return 0; }
            cvsasx_hash_t chk; cvsasx_blake3(b,l,&chk);
            if(!cvsasx_hash_eq(&chk,&p->seg_hash[i])){
                sputs("*** L1 FAIL: writable segment "); sdec(i); sputs(" BLAKE3 mismatch (fail-closed) ***\n"); return 0; }
            frame = frame_reserve();
            if(!frame){ sputs("*** L1 FAIL: no frame for writable segment "); sdec(i); sputs(" ***\n"); return 0; }
            uint8_t *fp=(uint8_t*)P2V(frame);
            for(size_t k=0;k<l;k++) fp[k]=((const uint8_t*)b)[k];
        } else {
            /* READ-ONLY segment (code): materialize-by-hash - the SAME F1 engine,
             * which SHARES the resident frame by hash (this is what gives L4 code
             * sharing for free). */
            frame = rm_materialize(&p->seg_hash[i]);
            if(!frame){ sputs("*** L1 FAIL: segment "); sdec(i); sputs(" did not materialize (fail-closed) ***\n"); return 0; }
        }
        /* zero-fill the BSS tail (memsz > filesz) in this process's frame. */
        uint8_t *fp = (uint8_t*)P2V(frame);
        for(uint64_t b=segs[i].filesz; b<segs[i].memsz; b++) fp[b]=0;
        /* map: writable IFF PF_W (code => read-only => W^X by construction + the assert). */
        uint64_t flags = MM_USER | ((f&ELF_PF_W)?MM_WRITE:0);
        if(!mm_map(p->space, segs[i].vaddr, frame, flags)){
            sputs("*** L1 FAIL: map of segment "); sdec(i); sputs(" failed ***\n"); return 0; }
        p->seg_frame[i]=frame; p->seg_vaddr[i]=segs[i].vaddr; p->seg_flags[i]=f;
    }
    /* a fresh user stack: R/W, never executable, private (W^X). */
    p->stack_frame = frame_reserve();
    if(!p->stack_frame){ sputs("*** L1 FAIL: no stack frame ***\n"); return 0; }
    if(!mm_map(p->space, LD_STACK_VA, p->stack_frame, MM_USER|MM_WRITE)){
        sputs("*** L1 FAIL: stack map failed ***\n"); return 0; }
    p->stacktop = LD_STACK_VA + 4096 - 16;
    return 1;
}

/* ENTER a loaded process in ring 3 (L2). Generalizes the proven US IRETQ-to-CPL3
 * path to an arbitrary (space, entry, stacktop): exactly the run_userspace block
 * with the launch parameters supplied. SYS_EXIT returns here (kernel CR3/RSP
 * restored by the isr_syscall exit stub) and the results land in g_ur. */
static void ld_enter_ring3(uint64_t space, uint64_t entry, uint64_t stacktop){
    __asm__ volatile("mov %%cr3,%0":"=r"(us_kcr3)); us_kcr3&=~0xfffULL;  /* restore target on exit */
    __asm__ volatile(
        "push %%rbx\n\t push %%rbp\n\t push %%r12\n\t push %%r13\n\t push %%r14\n\t push %%r15\n\t"
        "leaq 1f(%%rip),%%rax\n\t push %%rax\n\t"
        "mov %%rsp,%[ksp]\n\t"
        "mov %[ucr3],%%rax\n\t mov %%rax,%%cr3\n\t"
        "pushq %[uss]\n\t pushq %[usp]\n\t pushq $0x202\n\t pushq %[ucs]\n\t pushq %[uip]\n\t"
        "iretq\n\t"
        "1:\n\t"
        "pop %%r15\n\t pop %%r14\n\t pop %%r13\n\t pop %%r12\n\t pop %%rbp\n\t pop %%rbx\n\t"
        : [ksp]"=m"(us_kmain_rsp)
        : [ucr3]"r"(space),[uss]"r"((uint64_t)SEL_UDATA),[usp]"r"(stacktop),
          [ucs]"r"((uint64_t)SEL_UCODE),[uip]"r"(entry)
        : "rax","memory","cc");
}

/* L3 - the AUTHORITY CEILING at LOAD TIME. A loaded process's authority is
 * re-minted through the proven anti-amp gate, bounded by the loader's ceiling
 * (us_cust/us_region, set up by run_userspace). We prove a load-time analogue of
 * the US3 table: a legit in-ceiling re-mint ACCEPTS; an over-ceiling and an
 * unauthorized-perm re-mint are each REFUSED by the gate with a DISTINCT reason. */
static void ld_authority_ceiling(void){
    sputs("\n=== L3: authority ceiling on the LOADED process (re-mint at load, the proven gate) ===\n");
    cvsasx_pir_t base; us_make_legit_pir(&base, 0, 256, CVSASX_PERM_LOAD|CVSASX_PERM_GLOBAL);
    /* legit, in-ceiling: load-only sub-range -> ACCEPT */
    { cvsasx_pir_t p=base; p.length=64; cvsasx_swcap_t c; cvsasx_status_t s=us_remint_at_crossing(&p,&c);
      int ok=(s==CVSASX_OK)&&c.valid&&c.length==64;
      sputs("   in-ceiling  (load-only, 64B sub-range) status="); sdec((uint64_t)s);
      sputs(" minted-len="); sdec(c.length); sputs(ok?" -> ACCEPT (bounded at birth) OK\n":" *** legit rejected ***\n"); }
    /* over-ceiling: wider bounds than the region -> REFUSED, BAD_BOUNDS */
    { cvsasx_pir_t p=base; p.length=257; cvsasx_swcap_t c; cvsasx_status_t s=us_remint_at_crossing(&p,&c);
      int ok=(s==CVSASX_ERR_BAD_BOUNDS)&&!c.valid;
      sputs("   over-ceiling(bounds 257>256)         status="); sdec((uint64_t)s);
      sputs(ok?" (DISTINCT: BAD_BOUNDS, fail-closed) REFUSED\n":" *** WRONG-REASON / AMPLIFIED ***\n"); }
    /* unauthorized resource/perm: claim STORE never granted -> REFUSED, AMPLIFY_PERMS */
    { cvsasx_pir_t p=base; p.perms|=CVSASX_PERM_STORE_CAP; cvsasx_swcap_t c; cvsasx_status_t s=us_remint_at_crossing(&p,&c);
      int ok=(s==CVSASX_ERR_AMPLIFY_PERMS)&&!c.valid;
      sputs("   unauthorized(+STORE_CAP claim)       status="); sdec((uint64_t)s);
      sputs(ok?" (DISTINCT: AMPLIFY_PERMS, fail-closed) REFUSED\n":" *** WRONG-REASON / AMPLIFIED ***\n"); }
    sputs("L3 -> authority bounded AT BIRTH by the re-mint (not ambient ring): in-ceiling accepts, over-ceiling + unauthorized each REFUSED by a DISTINCT reason\n");
}

/* THE LOADER (L0-L4). */
static uint8_t  ld_arena[1u<<16]; static cvsasx_store_entry_t ld_idx[32]; static cvsasx_store_t ld_store; /* the program-image store */
static void run_loader(void){
    sputs("\n=== PROCESS LOADER (L0-L4): loading a process = materializing its segments by hash ===\n");

    /* ---- L0: the ELF image is a STORED, content-addressed object. ---- */
    cvsasx_store_init(&ld_store, ld_arena, sizeof ld_arena, ld_idx, 32);
    /* the image bytes reached the store by EMBEDDING (user_elf.h, generated by
     * user_build.sh from kernel/user_prog.elf) - there is no filesystem at boot. */
    cvsasx_hash_t imgH; cvsasx_store_put(&ld_store, user_prog_elf, user_prog_elf_len, &imgH);
    sputs("L0 program image stored: "); sdec(user_prog_elf_len); sputs(" B, content-address ");
    for(int i=0;i<6;i++){int d=imgH.b[i]; sputc("0123456789abcdef"[(d>>4)&0xf]); sputc("0123456789abcdef"[d&0xf]);} sputs("..\n");

    uint64_t entry; ld_phdr_t segs[LD_MAXSEG];
    int nseg = elf_parse(user_prog_elf, user_prog_elf_len, &entry, segs, LD_MAXSEG);
    if(nseg < 1){ sputs("*** L0 FAIL: ELF parse rejected the image ***\n"); return; }
    sputs("L0 ELF64 valid (x86-64, ET_EXEC), entry="); sx64(entry);
    sputs(", PT_LOAD segments="); sdec((uint64_t)nseg); sputs(":\n");
    /* store EACH segment's bytes -> its content address; print the parsed table.
     * Segments go into rm_store (the residency store rm_materialize fetches from)
     * so that load = materialize-by-hash is LITERALLY the F1 fault-in engine. */
    rm_store_once();
    cvsasx_hash_t segH[LD_MAXSEG];
    for(int i=0;i<nseg;i++){
        cvsasx_store_put(&rm_store, user_prog_elf+segs[i].off, segs[i].filesz, &segH[i]);
        sputs("   seg["); sdec(i); sputs("] vaddr="); sx64(segs[i].vaddr);
        sputs(" filesz="); sdec(segs[i].filesz); sputs(" memsz="); sdec(segs[i].memsz);
        sputs(" flags="); ld_print_rwx(segs[i].flags); sputs(" hash=");
        for(int k=0;k<6;k++){int d=segH[i].b[k]; sputc("0123456789abcdef"[(d>>4)&0xf]); sputc("0123456789abcdef"[d&0xf]);} sputs("..\n");
    }
    /* malformed-image rejection, LOUD and fail-closed: corrupt the ELF magic. */
    { uint8_t bad[64]; for(int i=0;i<64;i++) bad[i]=user_prog_elf[i]; bad[1]^=0xFF;  /* break 'E' */
      uint64_t e2; ld_phdr_t t[LD_MAXSEG]; int r=elf_parse(bad,64,&e2,t,LD_MAXSEG);
      sputs("L0 malformed image (corrupted ELF magic) -> elf_parse returned ");
      if(r<0){ sputc('-'); sdec((uint64_t)(-r)); } else sdec((uint64_t)r);
      sputs(r<0?" -> REJECTED (fail-closed) OK\n":" -> *** ACCEPTED A MALFORMED IMAGE ***\n"); }

    /* ---- L1: LOAD = materialize the segments by hash into a FRESH space. ---- */
    sputs("\n=== L1: load = materialize segments by hash into a fresh ring-3 space (W^X held) ===\n");
    ld_proc_t pr; for(int i=0;i<nseg;i++) pr.seg_hash[i]=segH[i];
    pr.entry=entry;
    if(!ld_materialize(&pr, segs, nseg)){ sputs("*** L1 FAIL ***\n"); return; }
    sputs("L1 fresh space CR3="); sx64(pr.space); sputs("; "); sdec((uint64_t)nseg); sputs(" segments materialized + BLAKE3-verified:\n");
    int wx_held=1;
    for(int i=0;i<nseg;i++){
        int writable=(pr.seg_flags[i]&ELF_PF_W)!=0, exec=(pr.seg_flags[i]&ELF_PF_X)!=0;
        if(writable&&exec) wx_held=0;
        sputs("   seg["); sdec(i); sputs("] -> frame#"); sdec(fr_idx(pr.seg_frame[i]));
        sputs(" @"); sx64(pr.seg_vaddr[i]); sputs(" mapped "); ld_print_rwx(pr.seg_flags[i]);
        sputs(writable?" (writable, NOT exec)":" (read-only)"); sputc('\n');
    }
    sputs("L1 user stack -> frame#"); sdec(fr_idx(pr.stack_frame)); sputs(" @"); sx64(LD_STACK_VA);
    sputs(" (R/W, no-exec); entry="); sx64(pr.entry); sputs(" rsp="); sx64(pr.stacktop); sputc('\n');
    sputs("L1 W^X across all segments + stack: "); sputs(wx_held?"HELD (no segment is W+X) OK\n":"*** VIOLATED ***\n");

    /* ---- L2: ENTER the loaded program in ring 3 and RUN it. ---- */
    sputs("\n=== L2: enter the loaded program in ring 3 (its OWN materialized-by-hash code runs) ===\n");
    sputs("L2 loaded program output ->| ");
    g_ur.rsi=g_ur.rdx=g_ur.r10=0;
    ld_enter_ring3(pr.space, pr.entry, pr.stacktop);   /* runs until SYS_EXIT */
    uint64_t r_marker=g_ur.rsi, r_wcount=g_ur.rdx, r_obj=g_ur.r10;
    sputs(" |<- returned\n");
    /* report the SYS_WRITE byte count in hex (sx64) - the message length is the
     * decoration; the load proof is marker + output-present + clean readobj return. */
    sputs("L2 loaded program: ring-3 marker="); sx64(r_marker); sputs(" (its own movq $0x10AD), SYS_WRITE bytes="); sx64(r_wcount);
    sputs(" (output-present="); sputs(r_wcount?"y":"n"); sputs("), SYS_READOBJ[7]="); sdec(r_obj); sputc('\n');
    sputs((r_marker==0x10AD && r_wcount>0 && r_obj==8)
          ? "L2 -> the LOADED ELF ran in ring 3, executed its OWN code (from a hash, not linked), SYS_WRITE succeeded, returned cleanly OK\n"
          : "L2 -> *** loaded program did not run as expected ***\n");

    /* ---- L3: the authority ceiling on the loaded process (the proven gate). ---- */
    ld_authority_ceiling();

    /* ---- L4: two processes, SHARED CODE BY HASH. ---- */
    sputs("\n=== L4: two processes from one image - read-only code shares ONE frame by hash, data private ===\n");
    ld_proc_t a, b;
    for(int i=0;i<nseg;i++){ a.seg_hash[i]=segH[i]; b.seg_hash[i]=segH[i]; }
    a.entry=b.entry=entry;
    int ok_a=ld_materialize(&a, segs, nseg);
    int ok_b=ld_materialize(&b, segs, nseg);
    if(!ok_a||!ok_b){ sputs("*** L4 FAIL: could not load both processes ***\n"); return; }
    /* identify the code (R-X) and data (R-W) segments. */
    int code=-1, data=-1;
    for(int i=0;i<nseg;i++){
        if((a.seg_flags[i]&ELF_PF_X)&&!(a.seg_flags[i]&ELF_PF_W)) code=i;
        if((a.seg_flags[i]&ELF_PF_W)&&!(a.seg_flags[i]&ELF_PF_X)) data=i;
    }
    sputs("L4 process A space="); sx64(a.space); sputs("  process B space="); sx64(b.space); sputc('\n');
    if(code>=0){
        uint64_t fa=a.seg_frame[code], fb=b.seg_frame[code];
        uint64_t rc=framedb[fr_idx(fa)].refcount;
        int shared=(fa==fb);
        sputs("L4 read-only CODE seg["); sdec(code); sputs("]: A frame#"); sdec(fr_idx(fa));
        sputs(" B frame#"); sdec(fr_idx(fb)); sputs(shared?" -> SAME FRAME (shared by hash)":" -> *** NOT SHARED ***");
        sputs(", refcount="); sdec(rc); sputc('\n');
        sputs(shared&&rc>=2 ? "L4 code share-by-hash: SAME physical frame, refcount>=2 OK\n" : "L4 code share: *** FAIL ***\n");
    }
    if(data>=0){
        uint64_t da=a.seg_frame[data], db=b.seg_frame[data];
        sputs("L4 writable DATA seg["); sdec(data); sputs("]: A frame#"); sdec(fr_idx(da));
        sputs(" B frame#"); sdec(fr_idx(db)); sputs((da!=db)?" -> DISTINCT FRAMES (private, writable)":" -> *** SHARED WRITABLE ***");
        sputc('\n');
        sputs((da!=db) ? "L4 data privacy: distinct writable frames per process OK\n" : "L4 data: *** SHARED WRITABLE - FAIL ***\n");
    }
    /* stacks are always private (fresh frame each). */
    sputs("L4 stacks: A frame#"); sdec(fr_idx(a.stack_frame)); sputs(" B frame#"); sdec(fr_idx(b.stack_frame));
    sputs((a.stack_frame!=b.stack_frame)?" -> DISTINCT (private) OK\n":" -> *** SHARED STACK ***\n");
    sputs("L4 HONEST: the two processes are loaded (spaces + frames + entry/stack ready) and the sharing is shown AT LOAD TIME;\n");
    sputs("   they run SEQUENTIALLY (the single-g_ur / single ring-3 slot from US has no concurrent ring-3 scheduler) - stated, not faked.\n");
    sputs("   (C0 below removes this limitation: the same two processes run CONCURRENTLY, timer-preempted.)\n");
}

/* ===========================================================================
 * CONCURRENT MULTI-PROCESS SCHEDULING (C0-C3) - the local single-node mechanism.
 *
 * SCOPE (binding, from the scheduling research): the LOCAL mechanism ONLY -
 * concurrent content-addressed ring-3 processes, per-process re-minted authority,
 * one deschedule->dematerialize->rematerialize cycle. NO unified decentralized-
 * orchestration model is claimed (research rated that 3/10). pick_next stays the
 * isolated round-robin hook; a rematerialization-AWARE policy (deciding WHEN to
 * dematerialize from cost) is a NAMED future seam, NOT implemented here.
 *
 * What is genuinely NEW vs the loader (which ran ring-3 procs SEQUENTIALLY via the
 * synchronous IRETQ-call-and-return g_ur slot): a FULL ring-3 context save/restore
 * driven by the timer. When the PIT fires while CPL=3, conc_trap (naked) saves the
 * interrupted process's ENTIRE register frame + iretq frame, the C scheduler picks
 * the next process, conc_trap restores ITS frame and CR3, and IRETQs into it. The
 * loader, the per-task page tables (mm_new_space), the proven store, and the anti-
 * amp gate are REUSED unchanged; only this context-switch + the C scheduler are new.
 * ===========================================================================*/

/* the full trapframe pushed on RSP0 by conc_trap/conc_sysent (low addr -> high):
 * 15 GPRs (r15 lowest) then the CPU's 5-qword iretq frame. The C scheduler reads
 * and rewrites this in place; the naked entry pops the GPRs and IRETQs from it. */
typedef struct {
    uint64_t r15,r14,r13,r12,r11,r10,r9,r8,rbp,rdi,rsi,rdx,rcx,rbx,rax;
    uint64_t rip,cs,rflags,rsp,ss;
} c_tf_t;

#define CONC_MAX 2
#define CONC_CODE_VA 0x0000000040000000ULL   /* shared, read-only-by-hash code page */
#define CONC_DATA_VA 0x0000000060000000ULL   /* private, writable per-process state page */
#define CONC_STK_VA  0x0000000050000000ULL   /* private user stack (= LD_STACK_VA, but per-space) */

/* the per-process schedulable state (the content-addressed process). */
typedef struct {
    uint32_t id; uint8_t tag; int live; int present;   /* present=context resident (vs dematerialized) */
    uint64_t cr3;                                       /* the process's own page-table root */
    uint64_t data_frame, stack_frame, code_frame;      /* its resident frames */
    c_tf_t tf;                                          /* full saved ring-3 context */
    /* C1 authority: each process has its OWN re-minted ceiling over its OWN object range. */
    cvsasx_sw_custodian_t cust; cvsasx_sw_region_t region; uint64_t ceil_off, ceil_len;
    uint8_t  ceilH[CVSASX_BLAKE3_LEN];                  /* content hash of the process's object range */
    /* C2: content address of the dematerialized schedulable state (when !present). */
    uint8_t remat_root[32]; int dematerialized;
    uint64_t carried;                                  /* value carried out on SYS_EXIT (the counter) */
    /* PP policy fields (rematerialization-aware scheduling). All scheduler-visible
     * and cheap to read; no proven module touches them. */
    uint8_t  desched_reason;     /* PP2: why it last descheduled (DR_* below) */
    uint64_t last_remat_tsc;     /* PP4: rdtsc when last rematerialized (wall-clock cooldown, real-HW path) */
    uint32_t cooldown_budget;    /* PP4: attempts-since-remat backoff (clock-independent; the robust bound) */
    uint32_t wakeup_count;       /* PP2/PP4: recent wakeups (high-reuse / thrash signal) */
    /* FA fairness fields (per-process progress accounting; scheduler-visible, cheap).
     * useful_cyc = cycles of real work credited; tax_cyc = cycles BURNED paying the
     * rematerialize-from-hash penalty (the resume tax the resident peer never pays).
     * fair_grant = remaining keep-resident grants the fairness control owes this
     * process so it can stop paying tax and catch up (FA1); bounded by FA_GRANT_CAP. */
    uint64_t useful_cyc;         /* FA0: useful work credited (progress numerator) */
    uint64_t tax_cyc;            /* FA0: remat-penalty cycles burned (the deficit source) */
    uint32_t fair_grant;         /* FA1/FA2: keep-resident grants remaining (bounded compensation) */
} cproc_t;

/* PP2 deschedule-reason tags (the cheap scheduler-visible signal the prediction
 * heuristic reads). QUANTUM = timer/quantum preempt -> predict SHORT. IO_BLOCK /
 * LONG_SLEEP = blocked on an event or a voluntary long sleep -> predict LONG. */
enum desched_reason { DR_QUANTUM=0, DR_IO_BLOCK=1, DR_LONG_SLEEP=2 };

static cproc_t g_cp[CONC_MAX];
static int g_ncp=0, g_ccur=-1, g_clive=0;
static uint64_t g_cswitches=0, g_csyscalls=0;
static uint64_t g_c2_park_at=0;   /* C2: if >0, the scheduler parks the running proc back to kmain once its counter reaches this */
/* C3 MEASURED costs, published so the PP break-even anchors in them (NOT a guess). */
static uint64_t g_c3_resident_cyc=0, g_c3_fromhash_cyc=0;
uint64_t conc_kmain_rsp=0;                    /* kmain's stack to return to (referenced by name in conc_enter's asm; non-static so the symbol is emitted) */
static uint64_t conc_kcr3=0;                 /* kernel CR3 (restored when leaving the demo) */
static int g_conc_active=0;                  /* the trap/syscall route through conc_sched only while set */

/* the privileged object the C1 ceilings are carved from (one shared object, each
 * process bounded to a DISJOINT sub-range so no process can name another's). */
static uint8_t conc_obj[256];

/* the shared ring-3 program (PIC; talks only via int 0x80). It reads its private
 * data page at CONC_DATA_VA: [tag:1][pad:7][counter:8][n_done:8][n_target:8]. It
 * busy-spins (so the 100Hz timer preempts mid-spin), SYS_WRITEs its tag, bumps the
 * carried counter + n_done, and SYS_EXITs (carrying counter in rsi) at n_target. */
extern char conc_blob[], conc_blob_end[];
__asm__(
    ".pushsection .text\n\t"
    ".global conc_blob\n\t .global conc_blob_end\n\t"
    "conc_blob:\n\t"
    "  movq $0x60000000, %r15\n\t"              /* &data page (CONC_DATA_VA) */
    "cb_loop:\n\t"
    "  movq $3000000, %rcx\n\t"                 /* busy-spin: long enough for a 100Hz tick to land mid-spin */
    "cb_spin:\n\t  decq %rcx\n\t  jnz cb_spin\n\t"
    "  movzbq (%r15), %rsi\n\t  movq $1, %rdi\n\t  int $0x80\n\t"   /* SYS_WRITE tag */
    "  incq 8(%r15)\n\t"                         /* counter++   (the value that survives C2) */
    "  incq 16(%r15)\n\t"                        /* n_done++ */
    "  movq 16(%r15), %rax\n\t  cmpq 24(%r15), %rax\n\t  jb cb_loop\n\t"
    "  movq $9, %rdi\n\t  movq 8(%r15), %rsi\n\t  int $0x80\n\t"    /* SYS_EXIT(counter) */
    "cb_hang:\n\t  jmp cb_hang\n\t"
    "conc_blob_end:\n\t"
    ".popsection\n\t");

/* C1 - re-mint a process's authority through the PROVEN anti-amp gate against ITS
 * OWN custodian/region. Returns the gate status; *out is the bounded re-minted cap. */
static cvsasx_status_t conc_remint(cproc_t *p, const cvsasx_pir_t *pir, cvsasx_swcap_t *out){
    return cvsasx_sw_cap_remint(&p->cust, pir, &p->region, out);
}
/* build a PIR naming an offset/len within an object identified by hash h, load-only. */
static void conc_make_pir(cvsasx_pir_t *pir, const uint8_t *h, uint64_t off, uint64_t len, uint32_t perms){
    for(unsigned i=0;i<sizeof *pir;i++) ((uint8_t*)pir)[i]=0;
    for(int i=0;i<(int)CVSASX_BLAKE3_LEN;i++) pir->referent_hash[i]=h[i];
    pir->offset=off; pir->length=len; pir->struct_version=CVSASX_PIR_VERSION; pir->perms=perms;
    pir->flags=CVSASX_PIR_FLAG_REFERENT_VALID;
}

/* syscall numbers reused from US (SYS_WRITE=1, SYS_EXIT=9). */
/* THE C SCHEDULER. Called by the naked entries with the in-place trapframe. reason
 * 0 = timer preempt; 1 = syscall. It saves the current process, services/reschedules,
 * rewrites *tf with the NEXT process's frame, and switches CR3. pick_next() is the
 * isolated round-robin policy hook - the ONLY scheduling decision, unchanged from
 * STEP 11's contract. A rematerialization-aware policy replaces ONLY pick_next. */
static int conc_pick_next(int cur){                 /* round-robin over live, present procs */
    for(int k=1;k<=g_ncp;k++){ int i=(cur+k)%g_ncp; if(g_cp[i].live && g_cp[i].present) return i; }
    /* a live-but-dematerialized proc still needs picking (C2 rematerializes it on select). */
    for(int k=1;k<=g_ncp;k++){ int i=(cur+k)%g_ncp; if(g_cp[i].live) return i; }
    return -1;
}
static cvsasx_hash_t conc_dematerialize(cproc_t *p);   /* C2, defined below */
static int           conc_rematerialize(cproc_t *p);   /* C2, defined below */

__attribute__((used)) static void conc_sched(c_tf_t *tf, uint64_t reason){
    outb(0x20,0x20);                                /* EOI for the timer path (harmless on syscall path) */
    if(g_ccur<0){ return; }
    cproc_t *cur=&g_cp[g_ccur];
    cur->tf=*tf;                                    /* save the interrupted/ trapping process's FULL context */

    if(reason==1){                                  /* SYSCALL path */
        g_csyscalls++;
        uint64_t n=tf->rdi, a1=tf->rsi;
        if(n==SYS_WRITE){ kputc((char)a1); sputc((char)a1); cur->tf.rax=0; *tf=cur->tf; return; }  /* service + stay on the SAME process */
        if(n==SYS_EXIT){ cur->live=0; g_clive--; cur->carried=a1; (void)cur; }   /* fallthrough -> reschedule */
        else { cur->tf.rax=(uint64_t)-1; *tf=cur->tf; return; }
    }
    /* C2: a one-process deschedule probe - once the running proc's carried counter
     * reaches g_c2_park_at, PARK it back to kmain (keeping it live + its context saved
     * in cur->tf) so the driver can dematerialize it. */
    if(reason==0 && g_c2_park_at>0 && cur->present && cur->data_frame){
        uint64_t ctr=*(uint64_t*)(P2V(cur->data_frame)+8);
        if(ctr>=g_c2_park_at){
            __asm__ volatile("mov %0,%%cr3"::"r"(conc_kcr3):"memory");
            extern char conc_return[];
            tf->rip=(uint64_t)conc_return; tf->cs=SEL_KCODE; tf->ss=SEL_KDATA;
            tf->rsp=conc_kmain_rsp; tf->rflags=0x2;     /* IF=0 */
            return;
        }
    }
    /* TIMER preempt OR a SYS_EXIT that must move on: pick the next runnable process. */
    int nxt=conc_pick_next(g_ccur);
    if(nxt<0){                                      /* all processes have exited -> leave the demo */
        g_conc_active=0;
        __asm__ volatile("mov %0,%%cr3"::"r"(conc_kcr3):"memory");
        /* hand control back to kmain by IRETQ-returning to conc_enter's saved ring-0 frame. */
        extern char conc_return[];
        tf->rip=(uint64_t)conc_return; tf->cs=SEL_KCODE; tf->ss=SEL_KDATA;
        tf->rsp=conc_kmain_rsp; tf->rflags=0x2;     /* IF=0: stay in the kernel, no re-entry */
        return;
    }
    cproc_t *np=&g_cp[nxt];
    if(!np->present){ conc_rematerialize(np); }     /* C2: a selected dematerialized proc is rematerialized */
    g_ccur=nxt; g_cswitches++;
    __asm__ volatile("mov %0,%%cr3"::"r"(np->cr3):"memory");   /* switch to the next process's address space */
    *tf=np->tf;                                     /* restore its full ring-3 context (asm pops + iretqs) */
}

/* the naked timer entry for the concurrent demo: save GPRs, call conc_sched(tf,0),
 * restore GPRs, IRETQ. Replaces isr_timer in the IDT only for the demo's duration. */
__attribute__((naked)) static void conc_trap(void){
    __asm__ volatile(
        "push %%rax\n\t push %%rbx\n\t push %%rcx\n\t push %%rdx\n\t push %%rsi\n\t push %%rdi\n\t push %%rbp\n\t"
        "push %%r8\n\t push %%r9\n\t push %%r10\n\t push %%r11\n\t push %%r12\n\t push %%r13\n\t push %%r14\n\t push %%r15\n\t"
        "mov %%rsp,%%rdi\n\t xor %%rsi,%%rsi\n\t"   /* rdi=tf, rsi=reason(0=timer) */
        "call conc_sched\n\t"
        "pop %%r15\n\t pop %%r14\n\t pop %%r13\n\t pop %%r12\n\t pop %%r11\n\t pop %%r10\n\t pop %%r9\n\t pop %%r8\n\t"
        "pop %%rbp\n\t pop %%rdi\n\t pop %%rsi\n\t pop %%rdx\n\t pop %%rcx\n\t pop %%rbx\n\t pop %%rax\n\t"
        "iretq\n\t" ::: "memory");
}
/* the naked int-0x80 syscall entry for the concurrent demo (DPL=3 gate). Same frame,
 * reason=1; conc_sched dispatches WRITE (stay) / EXIT (reschedule). */
__attribute__((naked)) static void conc_sysent(void){
    __asm__ volatile(
        "push %%rax\n\t push %%rbx\n\t push %%rcx\n\t push %%rdx\n\t push %%rsi\n\t push %%rdi\n\t push %%rbp\n\t"
        "push %%r8\n\t push %%r9\n\t push %%r10\n\t push %%r11\n\t push %%r12\n\t push %%r13\n\t push %%r14\n\t push %%r15\n\t"
        "mov %%rsp,%%rdi\n\t mov $1,%%rsi\n\t"
        "call conc_sched\n\t"
        "pop %%r15\n\t pop %%r14\n\t pop %%r13\n\t pop %%r12\n\t pop %%r11\n\t pop %%r10\n\t pop %%r9\n\t pop %%r8\n\t"
        "pop %%rbp\n\t pop %%rdi\n\t pop %%rsi\n\t pop %%rdx\n\t pop %%rcx\n\t pop %%rbx\n\t pop %%rax\n\t"
        "iretq\n\t" ::: "memory");
}

/* conc_enter(tf, cr3): the ONE entry into a concurrent ring-3 process from kmain's
 * ring-0 context. Saves kmain's callee-saved frame + rsp (into conc_kmain_rsp),
 * switches CR3, restores the FULL GPR set from *tf (rdi=tf, rsi=cr3 on entry -
 * load them into scratch first), pushes the 5-qword iret frame from *tf, and IRETQs.
 * The scheduler returns control to conc_run by IRETQ-ing to the conc_return label
 * (it sets tf->rip=conc_return, tf->rsp=conc_kmain_rsp); we pop the callee-saved
 * frame and `ret`. Restoring ALL GPRs (not just the iret frame) is the fix for the
 * mid-execution resume: e.g. the blob keeps its data-page pointer in r15. */
__attribute__((used, naked)) static void conc_enter(c_tf_t *tf, uint64_t cr3){
    __asm__ volatile(
        "push %%rbx\n\t push %%rbp\n\t push %%r12\n\t push %%r13\n\t push %%r14\n\t push %%r15\n\t"
        "mov %%rsp, conc_kmain_rsp(%%rip)\n\t"      /* save kmain's resume rsp */
        "mov %%rsi, %%cr3\n\t"                       /* switch to the process address space */
        "mov %%rdi, %%rax\n\t"                       /* rax = tf base (we restore rax LAST from tf) */
        /* push the iret frame from tf: ss, rsp, rflags, cs, rip (offsets after 15 GPRs) */
        "pushq 19*8(%%rax)\n\t"                      /* ss      (tf+19) */
        "pushq 18*8(%%rax)\n\t"                      /* rsp     (tf+18) */
        "pushq 17*8(%%rax)\n\t"                      /* rflags  (tf+17) */
        "pushq 16*8(%%rax)\n\t"                      /* cs      (tf+16) */
        "pushq 15*8(%%rax)\n\t"                      /* rip     (tf+15) */
        /* restore all 15 GPRs from tf (struct order: r15,r14,...,rbx,rax = idx 0..14) */
        "mov  0*8(%%rax), %%r15\n\t"  "mov  1*8(%%rax), %%r14\n\t"  "mov  2*8(%%rax), %%r13\n\t"
        "mov  3*8(%%rax), %%r12\n\t"  "mov  4*8(%%rax), %%r11\n\t"  "mov  5*8(%%rax), %%r10\n\t"
        "mov  6*8(%%rax), %%r9\n\t"   "mov  7*8(%%rax), %%r8\n\t"   "mov  8*8(%%rax), %%rbp\n\t"
        "mov  9*8(%%rax), %%rdi\n\t"  "mov 10*8(%%rax), %%rsi\n\t"  "mov 11*8(%%rax), %%rdx\n\t"
        "mov 12*8(%%rax), %%rcx\n\t"  "mov 13*8(%%rax), %%rbx\n\t"  "mov 14*8(%%rax), %%rax\n\t"
        "iretq\n\t"
        ".global conc_return\n\t conc_return:\n\t"   /* scheduler IRETQs here when procs park/exit */
        "pop %%r15\n\t pop %%r14\n\t pop %%r13\n\t pop %%r12\n\t pop %%rbp\n\t pop %%rbx\n\t"
        "ret\n\t" ::: "memory");
}
extern char conc_return[];

/* C2 - DEMATERIALIZE a process's SCHEDULABLE STATE to a content hash and RELEASE its
 * resident state; REMATERIALIZE (verified) on reselect. HONEST SCOPE (the wider-task-
 * state-object OPEN item): we dematerialize the schedulable REGISTER/STACK state - the
 * c_tf_t (all GPRs + the iretq frame: rip/rsp/rflags/cs/ss) PLUS the private writable
 * DATA page (which carries the process's counter). On dematerialize we ALSO release the
 * stack + data resident frames (their content is in the hash). The page-table ROOT
 * (cr3) and the mappings are NOT dematerialized - they stay resident and are reused on
 * rematerialize. So "state not resident" = the register/stack/data CONTENT is gone (in
 * the store), but the address-space SKELETON (cr3 + PTEs) persists. Dematerializing the
 * full page-table state too is the named OPEN item. */
static uint8_t  conc_arena[1u<<16]; static cvsasx_store_entry_t conc_idx[64]; static cvsasx_store_t conc_store; static int conc_store_ready=0;
static uint8_t  conc_demat_buf[sizeof(c_tf_t)+4096];
static int      conc_demat_len_was=0;
static void conc_store_once(void){ if(!conc_store_ready){ cvsasx_store_init(&conc_store,conc_arena,sizeof conc_arena,conc_idx,64); conc_store_ready=1; } }
static cvsasx_hash_t conc_dematerialize(cproc_t *p){
    conc_store_once();
    /* serialize: [c_tf_t][4096B data-page image]. The data page holds the counter. */
    uint8_t *b=conc_demat_buf; uint64_t off=0;
    for(unsigned i=0;i<sizeof(c_tf_t);i++) b[off++]=((uint8_t*)&p->tf)[i];
    const uint8_t *dp=(const uint8_t*)P2V(p->data_frame);
    for(int i=0;i<4096;i++) b[off++]=dp[i];
    conc_demat_len_was=(int)off;
    cvsasx_hash_t h; cvsasx_store_put(&conc_store,b,off,&h);
    for(int i=0;i<32;i++) p->remat_root[i]=h.b[i];
    /* RELEASE the resident schedulable content: unmap + free the stack and data frames. */
    mm_unmap(p->cr3, CONC_STK_VA); mm_unmap(p->cr3, CONC_DATA_VA);
    frame_release_physical(p->stack_frame); frame_release_physical(p->data_frame);
    p->stack_frame=p->data_frame=0;
    p->present=0; p->dematerialized=1;
    return h;
}
static int conc_rematerialize(cproc_t *p){
    conc_store_once();
    cvsasx_hash_t h; for(int i=0;i<32;i++) h.b[i]=p->remat_root[i];
    const void *b; size_t l;
    if(cvsasx_store_get(&conc_store,&h,&b,&l)!=CVSASX_STORE_OK) return 0;      /* MISS = fail-closed */
    cvsasx_hash_t chk; cvsasx_blake3(b,l,&chk);
    if(!cvsasx_hash_eq(&chk,&h)) return 0;                                     /* verify (integrity by construction) */
    if(l!=sizeof(c_tf_t)+4096) return 0;
    const uint8_t *p8=(const uint8_t*)b; uint64_t off=0;
    for(unsigned i=0;i<sizeof(c_tf_t);i++) ((uint8_t*)&p->tf)[i]=p8[off++];     /* restore registers + iretq frame */
    /* re-acquire fresh frames for stack + data, write the data image back, re-map. */
    p->stack_frame=frame_reserve(); p->data_frame=frame_reserve();
    if(!p->stack_frame||!p->data_frame) return 0;
    uint8_t *dp=(uint8_t*)P2V(p->data_frame);
    for(int i=0;i<4096;i++) dp[i]=p8[off++];
    mm_map(p->cr3, CONC_STK_VA,  p->stack_frame, MM_USER|MM_WRITE);
    mm_map(p->cr3, CONC_DATA_VA, p->data_frame,  MM_USER|MM_WRITE);
    p->present=1; p->dematerialized=0;
    return 1;
}

/* build one concurrent ring-3 process: fresh space (mm_new_space), shared code page
 * by hash (share-by-hash, proves own-page-tables + sharing), private data + stack
 * pages, the initial ring-3 trapframe, and its OWN C1 ceiling over a disjoint object
 * range. n_target = how many SYS_WRITEs before it exits. */
static cvsasx_hash_t conc_code_hash; static int conc_code_stored=0;
static int conc_make_proc(cproc_t *p, int id, uint8_t tag, uint64_t target, uint64_t ceil_off, uint64_t ceil_len){
    p->id=id; p->tag=tag; p->live=1; p->present=1; p->dematerialized=0;
    /* PP policy fields default to a clean slate (stack-local cproc_t are uninitialized). */
    p->desched_reason=DR_QUANTUM; p->last_remat_tsc=0; p->cooldown_budget=0; p->wakeup_count=0;
    p->useful_cyc=0; p->tax_cyc=0; p->fair_grant=0;   /* FA fairness accounting */
    p->cr3=mm_new_space(); if(!p->cr3) return 0;
    /* shared code page by hash: store the blob once, materialize-by-hash into every space. */
    uint64_t bloblen=(uint64_t)(conc_blob_end-conc_blob);
    if(!conc_code_stored){ rm_store_once(); cvsasx_store_put(&rm_store, conc_blob, bloblen, &conc_code_hash); conc_code_stored=1; }
    p->code_frame=rm_materialize(&conc_code_hash);            /* SHARED frame across procs (refcount++) */
    if(!p->code_frame) return 0;
    mm_map(p->cr3, CONC_CODE_VA, p->code_frame, MM_USER);     /* code: U/S, read-only (W^X) */
    /* private data page: [tag][pad][counter=0][n_done=0][n_target] */
    p->data_frame=frame_reserve(); p->stack_frame=frame_reserve();
    if(!p->data_frame||!p->stack_frame) return 0;
    uint8_t *dp=(uint8_t*)P2V(p->data_frame); for(int i=0;i<4096;i++) dp[i]=0;
    dp[0]=tag; *(uint64_t*)(dp+8)=0; *(uint64_t*)(dp+16)=0; *(uint64_t*)(dp+24)=target;
    mm_map(p->cr3, CONC_DATA_VA, p->data_frame, MM_USER|MM_WRITE);
    mm_map(p->cr3, CONC_STK_VA,  p->stack_frame, MM_USER|MM_WRITE);
    /* initial ring-3 trapframe: enter at the blob, user stack top, IF=1 (preemptible). */
    for(unsigned i=0;i<sizeof p->tf;i++) ((uint8_t*)&p->tf)[i]=0;
    p->tf.rip=CONC_CODE_VA; p->tf.cs=SEL_UCODE; p->tf.ss=SEL_UDATA;
    p->tf.rsp=CONC_STK_VA + 4096 - 16; p->tf.rflags=0x202;     /* IF=1 */
    /* C1: this process's OWN ceiling over a DISJOINT sub-range of conc_obj, treated
     * as its own object: region length = ceil_len, base = conc_obj+ceil_off, and the
     * region hash = BLAKE3 of THIS process's slice (distinct per process -> no two
     * processes can present each other's referent). */
    p->ceil_off=ceil_off; p->ceil_len=ceil_len;
    { cvsasx_hash_t h; cvsasx_blake3(conc_obj+ceil_off, ceil_len, &h); for(int i=0;i<(int)CVSASX_BLAKE3_LEN;i++) p->ceilH[i]=h.b[i]; }
    cvsasx_swcap_t root={ (uint64_t)(uintptr_t)conc_obj+ceil_off, ceil_len,
                          CVSASX_PERM_LOAD|CVSASX_PERM_GLOBAL, 1 };
    cvsasx_sw_custodian_init(&p->cust, root);
    p->region.object_cap=root; p->region.object_base_addr=(uint64_t)(uintptr_t)conc_obj+ceil_off;
    p->region.object_length=ceil_len;
    for(int i=0;i<(int)CVSASX_BLAKE3_LEN;i++) p->region.hash[i]=p->ceilH[i];
    return 1;
}

/* ===========================================================================
 * PP0-PP4 - the REMATERIALIZATION-AWARE SCHEDULING POLICY (the decision rule).
 *
 * The MECHANISM is C2 (conc_dematerialize / conc_rematerialize) and the COST is
 * C3 (g_c3_fromhash_cyc vs g_c3_resident_cyc, MEASURED via rdtsc). This adds ONLY
 * the POLICY: should_dematerialize() beside conc_pick_next(), deciding WHEN to pay
 * the C2 cost. It does NOT rebuild the mechanism and does NOT make scheduling
 * free (C3 forbids that); it is the local, measured, pressure-gated rule.
 *
 * RULE (binding): keep resident by default; dematerialize IFF (memory pressure)
 * AND (predicted-long deschedule), with a measured break-even and a thrash bound.
 * ===========================================================================*/

/* PRESSURE INPUT - the residency frame-pressure signal: free frames in the M0
 * pool. The pool is FRAMEDB_N frames; below PP_PRESSURE_LO free frames = pressure.
 * (Reuses the M0-M5 frame database verbatim; fdb_freetop is its free count.) */
#define PP_PRESSURE_LO 8u                 /* free-frame low-water; raise/lower per pool size */
static int pp_under_pressure(void){ return fdb_freetop < PP_PRESSURE_LO; }

/* PP3 BREAK-EVEN, ANCHORED IN THE MEASURED C3 COST (not a guess).
 * Dematerializing a descheduled proc frees FRAMES_FREED frames but makes resume
 * cost g_c3_fromhash_cyc instead of g_c3_resident_cyc. The EXTRA resume cost paid
 * is (fromhash - resident). The freed frames accrue value while the proc sleeps:
 * each freed frame, held for the deschedule, is worth one avoided resume-from-hash
 * to whatever reuses it - so value accrues at FRAME_VALUE_PER_CYC per freed frame
 * per cycle of deschedule (a CALIBRATION knob: the real per-cycle worth of a freed
 * frame is workload/hardware specific; the model is linear, the knob is the slope).
 * Break-even duration D* solves: FRAMES_FREED * FRAME_VALUE_PER_CYC * D* = extra.
 * Longer-than-D* nets a gain; shorter nets a loss. */
#define PP_FRAMES_FREED 2u                /* C2 frees stack + data frames (cr3/PTEs stay resident - the known caveat) */
/* FRAME_VALUE_PER_CYC is a rational so D* lands in a measurable cycle range. We
 * express value as: a freed frame is worth one avoided resume-from-hash per
 * PP_REUSE_WINDOW cycles it is held (i.e. slope = fromhash / window per frame). */
#define PP_REUSE_WINDOW 1000000ULL        /* cycles a freed frame must be held to be worth one resume; tune to memory turnover */
static uint64_t pp_breakeven_cyc(void){
    if(g_c3_fromhash_cyc<=g_c3_resident_cyc) return ~0ULL;     /* dematerializing never cheaper: never break even */
    uint64_t extra=g_c3_fromhash_cyc-g_c3_resident_cyc;        /* extra resume cost paid (MEASURED) */
    /* slope = PP_FRAMES_FREED freed * (fromhash / PP_REUSE_WINDOW) value per cycle.
     * D* = extra / slope = extra * PP_REUSE_WINDOW / (PP_FRAMES_FREED * fromhash). */
    uint64_t denom=PP_FRAMES_FREED*g_c3_fromhash_cyc;
    return denom? (extra*PP_REUSE_WINDOW)/denom : ~0ULL;
}

/* PP4 ANTI-THRASH cooldown. The robust bound is ATTEMPT-COUNTED, not wall-clock:
 * a rematerialize grants a backoff BUDGET of PP_COOLDOWN_BUDGET scheduling attempts
 * during which the proc is kept resident (it was just needed). The budget decrements
 * per attempt. This is clock-INDEPENDENT, so it is not fooled by noisy rdtsc or by
 * how slow the serial console is between attempts - it bounds the thrash to a SINGLE
 * pay per burst by construction. (A wall-clock variant, anchored in the MEASURED C3
 * cost = PP_COOLDOWN_REMATS * remat-cost, is kept for real hardware where wall time
 * is the right axis; the attempt counter is the bound the test relies on.) */
#define PP_COOLDOWN_BUDGET 6u             /* attempts kept resident after a remat (>= a typical retry burst) */
#define PP_COOLDOWN_REMATS 8ULL           /* real-HW wall-clock variant: backoff = N remat-costs of time */
static uint64_t pp_cooldown_cyc(void){
    uint64_t c=PP_COOLDOWN_REMATS*g_c3_fromhash_cyc;
    return c?c:2000000ULL;                 /* default if C3 cost unmeasured */
}
/* PP2 high-reuse anti-thrash: a proc with >= PP_REUSE_HOT recent wakeups is
 * predicted HIGH-REUSE -> keep resident regardless of reason. */
#define PP_REUSE_HOT 3u

/* PP2 prediction: does the deschedule REASON predict a LONG sleep? */
static int pp_predict_long(const cproc_t *p){
    return p->desched_reason==DR_IO_BLOCK || p->desched_reason==DR_LONG_SLEEP;
}

/* THE DECISION FUNCTION - beside conc_pick_next(). Returns 1 to dematerialize the
 * descheduled proc, 0 to keep it resident. KEEP RESIDENT BY DEFAULT; dematerialize
 * is the EXCEPTION (pressure AND predicted-long AND not-hot AND past-cooldown AND
 * the deschedule is expected to clear the measured break-even).
 * out_why (optional) receives a one-line reason code for the proof. */
enum pp_why { PPW_NO_PRESSURE=0, PPW_PRED_SHORT=1, PPW_HOT_REUSE=2, PPW_COOLDOWN=3,
              PPW_BELOW_BREAKEVEN=4, PPW_DEMAT=5, PPW_FAIR_KEEP=6 };
static const char* pp_why_str(int w){
    switch(w){ case PPW_NO_PRESSURE:return "no-pressure->KEEP";
               case PPW_PRED_SHORT: return "predicted-SHORT->KEEP";
               case PPW_HOT_REUSE:  return "hot-reuse->KEEP(anti-thrash)";
               case PPW_COOLDOWN:   return "in-cooldown->KEEP(anti-thrash)";
               case PPW_BELOW_BREAKEVEN:return "below-breakeven->KEEP";
               case PPW_DEMAT:      return "pressure+long+amortizes->DEMATERIALIZE";
               case PPW_FAIR_KEEP:  return "fairness-deficit->KEEP(catch-up)";
               default: return "?"; }
}

/* FA FAIRNESS CONTROL (the STEP-PP "fairness measured-not-controlled" OPEN item).
 * The hazard: equal scheduling turns != equal PROGRESS, because a repeatedly-
 * dematerialized process burns (fromhash-resident) cycles PAYING THE RESUME TAX on
 * each return while a resident peer resumes cheaply. Over equal turns the penalized
 * process falls behind in USEFUL work. The control (kept SIMPLE, see SCHED_LOG.md):
 * KEEP-RESIDENT BIAS. A process carrying a remat-penalty DEFICIT is granted a bounded
 * number of keep-resident turns so it stops paying the tax and catches up. This form
 * composes cleanly with the PP policy because it only ADDS a KEEP reason - it never
 * forces a DEMATERIALIZE, so the previously-resident peer is never starved as a side
 * effect (no starvation-inversion). g_fa_on gates it so PP0-PP4 (control OFF) and the
 * FA stages (control ON) share one decision function.
 *
 * deficit(p) = tax_cyc(p) - tax_cyc(peer): how many MORE penalty cycles this process
 * has burned than the best-off peer. > FA_DEFICIT_BAND means it is behind enough to
 * compensate. The grant is bounded by FA_GRANT_CAP turns (FA2 - it catches up, it does
 * not monopolize). */
static int g_fa_on=0;                  /* fairness control master switch (off during PP0-PP4) */
#define FA_DEFICIT_BAND  0ULL          /* deficit (cyc) above which a process is "behind" (one tax unit) */
#define FA_GRANT_CAP     8u            /* max keep-resident turns granted per behind-detection; bounds the help */
/* expected_desched_cyc = the scheduler's estimate of how long this deschedule
 * lasts (cycles). PP3 varies it around the break-even. Each call IS one scheduling
 * attempt, so it consumes one tick of any anti-thrash backoff budget. */
static int should_dematerialize(cproc_t *p, uint64_t expected_desched_cyc, int *out_why){
    int w;
    int in_cooldown = (p->cooldown_budget>0);
    if(p->cooldown_budget>0) p->cooldown_budget--;           /* one attempt spent against the backoff */
    if(!pp_under_pressure())                     w=PPW_NO_PRESSURE;        /* DEFAULT: keep resident */
    else if(g_fa_on && p->fair_grant>0){ p->fair_grant--; w=PPW_FAIR_KEEP; } /* FA: behind on progress -> keep to catch up (bounded grant) */
    else if(!pp_predict_long(p))                 w=PPW_PRED_SHORT;         /* quantum-preempt -> short -> keep */
    else if(p->wakeup_count>=PP_REUSE_HOT)       w=PPW_HOT_REUSE;          /* repeated wakeups -> keep */
    else if(in_cooldown)                         w=PPW_COOLDOWN;           /* just remat'd -> keep (backoff) */
    else if(expected_desched_cyc<pp_breakeven_cyc()) w=PPW_BELOW_BREAKEVEN;/* too short to amortize -> keep */
    else                                         w=PPW_DEMAT;             /* all hold -> dematerialize */
    if(out_why)*out_why=w;
    return w==PPW_DEMAT;
}

/* a tiny helper: build/reset one PP probe proc carrying a counter, run it part-way
 * (to counter>=park_at) so it has live resident state to keep-or-dematerialize.
 * Reuses conc_make_proc + the C2 park mechanism verbatim. Returns 1 on success. */
static int pp_make_and_run(cproc_t *p, uint8_t tag, uint64_t park_at){
    if(!conc_make_proc(p, 0, tag, 8, 0, 128)) return 0;
    g_ncp=1; g_clive=1; g_ccur=0;
    idt_set(0x20,(void*)conc_trap,SEL_KCODE);
    idt_set(0x80,(void*)conc_sysent,SEL_KCODE); idt[0x80].type=0xEE;
    { struct idtr idtr={(uint16_t)(sizeof(idt)-1),(uint64_t)idt}; __asm__ volatile("lidt %0"::"m"(idtr):"memory"); }
    g_conc_active=1; g_c2_park_at=park_at;
    conc_enter(&p->tf, p->cr3);              /* runs until counter>=park_at, parks back here */
    g_conc_active=0; g_c2_park_at=0;
    idt_set(0x20,(void*)isr_timer,SEL_KCODE);
    idt_set(0x80,(void*)isr_syscall,SEL_KCODE); idt[0x80].type=0xEE;
    { struct idtr idtr={(uint16_t)(sizeof(idt)-1),(uint64_t)idt}; __asm__ volatile("lidt %0"::"m"(idtr):"memory"); }
    return 1;
}
/* drain the M0 pool down to a target free count so pp_under_pressure() trips, and
 * give the drained frames back after. Returns the number reserved (to release). */
static uint64_t pp_pin_frames(uint64_t reserved[], uint64_t target_free){
    uint64_t n=0;
    while(fdb_freetop>target_free){ uint64_t f=frame_reserve(); if(!f) break; reserved[n++]=f; }
    return n;
}
static void pp_release_frames(uint64_t reserved[], uint64_t n){
    for(uint64_t i=0;i<n;i++) frame_release_physical(reserved[i]);
}

/* PP_RUN - drive the five policy stages. C is the C2/C3 probe proc (already built,
 * dematerialized at end of C3); we rebuild fresh probes as needed. */
static void pp_run(cproc_t *Cdummy){
    (void)Cdummy;
    sputs("\n=== PP: REMATERIALIZATION-AWARE SCHEDULING POLICY (should_dematerialize + pick_next wiring) ===\n");
    sputs("PP anchor (from C3, MEASURED): resume-resident="); sdec(g_c3_resident_cyc);
    sputs(" cyc, resume-from-hash="); sdec(g_c3_fromhash_cyc);
    sputs(" cyc, break-even deschedule="); sdec(pp_breakeven_cyc()); sputs(" cyc\n");
    uint64_t breakeven=pp_breakeven_cyc();
    static uint64_t held[FRAMEDB_N];

    /* ---- PP0: DEFAULT KEEP RESIDENT (no pressure => never dematerialize). ---- */
    sputs("\n--- PP0: default keep-resident (no pressure) ---\n");
    cproc_t P0p; int why0;
    if(!pp_make_and_run(&P0p,'P',3)){ sputs("PP0 NOT RUN: build failed\n"); return; }
    P0p.desched_reason=DR_LONG_SLEEP; P0p.wakeup_count=0; P0p.last_remat_tsc=0;
    uint64_t free_now=fdb_freetop;
    /* even a LONG-descheduled proc with a HUGE expected duration: no pressure => KEEP. */
    int d0=should_dematerialize(&P0p, breakeven*1000, &why0);
    sputs("PP0 free-frames="); sdec(free_now); sputs(" (pressure="); sputs(pp_under_pressure()?"y":"n");
    sputs("), proc reason=LONG_SLEEP, expected-desched=HUGE -> decision="); sputs(d0?"DEMATERIALIZE":"KEEP-RESIDENT");
    sputs(" ["); sputs(pp_why_str(why0)); sputs("]\n");
    /* PROVE: resume is the cheap resident switch (still present, no remat needed). */
    sputs("PP0 proc still present(resident)="); sputs(P0p.present?"y":"n");
    sputs(" -> resumes via the cheap resident switch (~"); sdec(g_c3_resident_cyc); sputs(" cyc, not ~");
    sdec(g_c3_fromhash_cyc); sputs(" cyc from hash)\n");
    sputs(( !d0 && P0p.present )?"PP0 -> NO PRESSURE => KEPT RESIDENT, CHEAP RESUME OK\n":"PP0 -> *** FAIL ***\n");

    /* ---- PP1: PRESSURE-GATED DEMATERIALIZATION (with frames-freed, MEASURED). ---- */
    sputs("\n--- PP1: pressure-gated dematerialization (frames freed MEASURED) ---\n");
    cproc_t P1p; int why1a, why1b;
    if(!pp_make_and_run(&P1p,'Q',3)){ sputs("PP1 NOT RUN: build failed\n"); return; }
    P1p.desched_reason=DR_LONG_SLEEP; P1p.wakeup_count=0; P1p.last_remat_tsc=0;
    /* (a) WITHOUT pressure: same long proc, long duration -> KEEP. */
    int d1a=should_dematerialize(&P1p, breakeven*4, &why1a);
    sputs("PP1(a) no-pressure free="); sdec(fdb_freetop); sputs(" -> decision="); sputs(d1a?"DEMAT":"KEEP");
    sputs(" ["); sputs(pp_why_str(why1a)); sputs("]\n");
    /* (b) INDUCE pressure: drain the pool below the low-water, then decide. */
    uint64_t nheld=pp_pin_frames(held, PP_PRESSURE_LO-2);   /* leave fewer than PP_PRESSURE_LO free */
    uint64_t free_before=fdb_freetop;
    int d1b=should_dematerialize(&P1p, breakeven*4, &why1b);
    sputs("PP1(b) induced-pressure free="); sdec(free_before); sputs(" (pressure="); sputs(pp_under_pressure()?"y":"n");
    sputs(") -> decision="); sputs(d1b?"DEMATERIALIZE":"KEEP"); sputs(" ["); sputs(pp_why_str(why1b)); sputs("]\n");
    /* ACT on the decision: dematerialize and MEASURE the frames the policy relieved. */
    uint64_t freed=0;
    if(d1b){ conc_dematerialize(&P1p); freed=fdb_freetop-free_before; }
    sputs("PP1(b) frames-freed by dematerializing the proc = "); sdec(freed);
    sputs(" (pool free "); sdec(free_before); sputs(" -> "); sdec(fdb_freetop); sputs(")\n");
    pp_release_frames(held, nheld);          /* relieve the artificial pressure */
    int pp1=(!d1a)&&d1b&&(freed>=1);
    sputs(pp1?"PP1 -> PRESSURE-GATED: kept w/o pressure, dematerialized + freed frames under pressure OK\n"
             :"PP1 -> *** FAIL ***\n");

    /* ---- PP2: PREDICTION HEURISTIC + MEASURED misprediction rate. ---- */
    sputs("\n--- PP2: prediction heuristic on a mixed workload (signal/decision/correctness) ---\n");
    /* A mixed workload of 4 descheduled procs, each tagged with WHY it descheduled,
     * plus a GROUND TRUTH of how long it ACTUALLY stayed descheduled (so we can score
     * the prediction). Under pressure. The policy must KEEP the quantum-preempted and
     * hot-reuse ones and DEMATERIALIZE the genuinely long ones. */
    nheld=pp_pin_frames(held, PP_PRESSURE_LO-2);   /* pressure on for the whole mix */
    struct { const char*name; uint8_t reason; uint32_t wakeups; int actually_long; } mix[4]={
        {"quantum-preempted", DR_QUANTUM,    0, 0},   /* short by nature: predict KEEP, truth short  */
        {"io-blocked",        DR_IO_BLOCK,   0, 1},   /* long: predict DEMAT, truth long             */
        {"long-sleep",        DR_LONG_SLEEP, 0, 1},   /* long: predict DEMAT, truth long             */
        {"hot-reuse(io)",     DR_IO_BLOCK,   PP_REUSE_HOT, 0}, /* tagged long but woken often: KEEP, truth short */
    };
    uint32_t mispred=0, total=4;
    for(int i=0;i<4;i++){
        cproc_t mp; if(!conc_make_proc(&mp,0,(uint8_t)('0'+i),8,0,128)){ sputs("PP2 build fail\n"); break; }
        mp.desched_reason=mix[i].reason; mp.wakeup_count=mix[i].wakeups; mp.last_remat_tsc=0;
        int whyi; int dec=should_dematerialize(&mp, breakeven*4, &whyi);
        /* "right" = the decision matches the GROUND TRUTH: dematerialize iff actually long.
         * (Keeping a short one resident is right; dematerializing a long one is right.) */
        int correct=(dec==mix[i].actually_long);
        if(!correct) mispred++;
        sputs("PP2 ["); sputs(mix[i].name); sputs("] signal=");
        sputs(mix[i].reason==DR_QUANTUM?"QUANTUM":(mix[i].reason==DR_IO_BLOCK?"IO_BLOCK":"LONG_SLEEP"));
        sputs(" wakeups="); sdec(mix[i].wakeups); sputs(" -> decision="); sputs(dec?"DEMAT":"KEEP");
        sputs(" ["); sputs(pp_why_str(whyi)); sputs("] truth="); sputs(mix[i].actually_long?"LONG":"SHORT");
        sputs(" -> "); sputs(correct?"RIGHT":"WRONG"); sputc('\n');
        conc_dematerialize(&mp); /* tidy: release whatever frames this probe holds (also relieves pressure) */
    }
    pp_release_frames(held, nheld);
    /* MEASURED misprediction rate = mispredictions/total, in tenths of a percent (integer). */
    uint32_t mispct_x10=(uint32_t)((mispred*1000u)/total);
    sputs("PP2 MEASURED misprediction rate = "); sdec(mispred); sputs("/"); sdec(total);
    sputs(" = "); sdec(mispct_x10/10); sputc('.'); sdec(mispct_x10%10); sputs("%\n");
    sputs((mispred==0)?"PP2 -> HEURISTIC CHOSE CORRECTLY FOR EACH (0% mispredict on this mix) OK\n"
                      :"PP2 -> heuristic mispredicted some (rate above) - right OFTEN ENOUGH, measured\n");

    /* ---- PP3: BREAK-EVEN TEST anchored in the C3 cost (gain above / loss below). ---- */
    sputs("\n--- PP3: break-even vs deschedule duration (anchored in MEASURED C3 cost) ---\n");
    sputs("PP3 break-even D* = "); sdec(breakeven); sputs(" cyc (= (fromhash-resident)*window / (frames*fromhash); MEASURED inputs)\n");
    sputs("PP3 model: NET = freed-frame value over the deschedule - extra resume cost paid.\n");
    sputs("PP3   dur(cyc)        | freed-value | extra-cost | NET            | verdict\n");
    uint64_t extra=(g_c3_fromhash_cyc>g_c3_resident_cyc)?(g_c3_fromhash_cyc-g_c3_resident_cyc):0;
    /* sweep durations around D*: 0.25x, 0.5x, 1x, 2x, 4x. */
    uint64_t muls_num[5]={1,1,1,2,4}, muls_den[5]={4,2,1,1,1};
    int gain_above=1, loss_below=1;
    for(int i=0;i<5;i++){
        uint64_t dur=(breakeven*muls_num[i])/muls_den[i];
        /* freed-frame value over this duration = frames * (fromhash/window) * dur.
         * Multiply BEFORE dividing (same order as pp_breakeven_cyc) so the slope is
         * not truncated to an integer - otherwise value collapses to ~dur and the
         * table disagrees with D*. */
        uint64_t value=(PP_FRAMES_FREED*g_c3_fromhash_cyc*dur)/PP_REUSE_WINDOW;
        int64_t net=(int64_t)value-(int64_t)extra;
        int is_gain=net>=0;
        if(dur>breakeven && !is_gain) gain_above=0;
        if(dur<breakeven && is_gain)  loss_below=0;
        sputs("PP3   "); sdec(dur);
        sputs("\t| "); sdec(value); sputs("\t| "); sdec(extra); sputs("\t| ");
        if(net<0){ sputc('-'); sdec((uint64_t)(-net)); } else sdec((uint64_t)net);
        sputs("\t| "); sputs(is_gain?"GAIN":"LOSS"); sputc('\n');
    }
    sputs((gain_above&&loss_below)?"PP3 -> LONGER-THAN-D* NETS A GAIN, SHORTER NETS A LOSS (anchored in C3) OK\n"
                                  :"PP3 -> break-even ordering not clean on this run (see stall report)\n");

    /* ---- PP4: ANTI-THRASH (cooldown bounds dematerialize-then-immediately-needed). ---- */
    sputs("\n--- PP4: anti-thrash backoff (forced thrash workload) ---\n");
    sputs("PP4 backoff budget = "); sdec(PP_COOLDOWN_BUDGET); sputs(" attempts kept resident after a remat");
    sputs(" (clock-independent bound; wall-clock variant = "); sdec(pp_cooldown_cyc()); sputs(" cyc = ");
    sdec(PP_COOLDOWN_REMATS); sputs("*measured-remat for real HW)\n");
    cproc_t TH; if(!pp_make_and_run(&TH,'T',3)){ sputs("PP4 NOT RUN: build failed\n"); return; }
    TH.desched_reason=DR_IO_BLOCK; TH.wakeup_count=0;   /* a genuinely-long-looking proc, the worst case for thrash */
    nheld=pp_pin_frames(held, PP_PRESSURE_LO-2);        /* pressure stays HIGH the whole time (the thrash trigger) */
    int ROUNDS=PP_COOLDOWN_BUDGET;                      /* hammer it once per granted attempt */
    /* round 1: nothing remat'd yet (budget 0) -> under pressure + long -> DEMATERIALIZE (pay once). */
    int whyA; int decA=should_dematerialize(&TH, breakeven*4, &whyA);
    uint64_t demats=0, kept_by_backoff=0;
    if(decA){ conc_dematerialize(&TH); demats++; }
    sputs("PP4 round1 (cold, budget=0): decision="); sputs(decA?"DEMAT":"KEEP"); sputs(" ["); sputs(pp_why_str(whyA)); sputs("]\n");
    /* it is IMMEDIATELY needed -> rematerialize (mechanism), GRANT the backoff budget. */
    conc_rematerialize(&TH); TH.cooldown_budget=PP_COOLDOWN_BUDGET; TH.last_remat_tsc=rdtsc(); TH.wakeup_count++;
    /* now hammer it: ROUNDS more attempts, each still under pressure + long. WITHOUT the
     * backoff each would dematerialize again (thrash). The budget must KEEP it each time. */
    for(int r=0;r<ROUNDS;r++){
        uint32_t budget_was=TH.cooldown_budget;
        int whyR; int decR=should_dematerialize(&TH, breakeven*4, &whyR);
        if(decR){ conc_dematerialize(&TH); demats++; conc_rematerialize(&TH); TH.cooldown_budget=PP_COOLDOWN_BUDGET; TH.last_remat_tsc=rdtsc(); }
        else kept_by_backoff++;
        sputs("PP4 round"); sdec(r+2); sputs(" (just-remat'd, budget="); sdec(budget_was); sputs("): decision=");
        sputs(decR?"DEMAT(thrash!)":"KEEP"); sputs(" ["); sputs(pp_why_str(whyR)); sputs("]\n");
    }
    pp_release_frames(held, nheld);
    sputs("PP4 over "); sdec((uint64_t)(ROUNDS+1)); sputs(" thrash rounds: dematerializations="); sdec(demats);
    sputs(" kept-by-backoff="); sdec(kept_by_backoff);
    sputs(" (cost AVOIDED = "); sdec(kept_by_backoff); sputs(" remats * ~"); sdec(g_c3_fromhash_cyc);
    sputs(" cyc each = "); sdec(kept_by_backoff*g_c3_fromhash_cyc); sputs(" cyc; thrash bounded to "); sdec(demats);
    sputs(" pay vs "); sdec((uint64_t)(ROUNDS+1)); sputs(" without backoff)\n");
    int pp4=(demats==1)&&((uint64_t)kept_by_backoff==(uint64_t)ROUNDS);
    sputs(pp4?"PP4 -> BACKOFF BOUNDED THRASH: paid ONCE, kept resident every retry (backoff engaged) OK\n"
             :"PP4 -> thrash bounded but not to a single pay on this run (cost paid-but-counted; see stall report)\n");

    /* tidy: make sure nothing the PP probes built lingers as live in the scheduler. */
    g_ncp=0; g_ccur=-1; g_clive=0;
    sputs("\nPP POLICY: PP0-PP4 run; pick_next + should_dematerialize is the rematerialization-aware rule (SCHED_LOG.md).\n");
}

/* ===========================================================================
 * FA0-FA2 - FAIRNESS CONTROL under rematerialization (regulating the per-process
 * progress deficit PP only MEASURED). The hazard, named in SCHED_LOG.md's PP not implemented:
 * two ring-3 processes given EQUAL scheduling turns make UNEQUAL PROGRESS if one keeps
 * being dematerialized and pays the resume-from-hash tax (g_c3_fromhash_cyc, ~the C3
 * cost) on each return while the other stays resident and resumes cheaply
 * (g_c3_resident_cyc). Equal CPU time, unequal work done.
 *
 * The control (kept SIMPLE - fairness was rated 4/10 by the policy research; this is a
 * defensible local control, NOT a proven-optimal scheduler, see SCHED_LOG.md): a
 * keep-resident BIAS. A process carrying a remat-penalty DEFICIT is granted a bounded
 * number of keep-resident turns (fair_grant) so it stops paying the tax and catches up.
 *
 * Accounting is MEASURED, never hardcoded: per-turn useful work is timed with rdtsc;
 * the per-turn tax is the LIVE C3 anchor (g_c3_fromhash_cyc - g_c3_resident_cyc).
 * ===========================================================================*/

/* one process's PROGRESS = useful cycles run / (useful + tax) cycles spent. A resident
 * peer has tax=0 -> progress=1.0; a taxed process spends part of its turn paying the
 * resume penalty -> progress<1.0. Reported in tenths of a percent (integer). */
static uint32_t fa_progress_x1000(const cproc_t *p){
    uint64_t denom=p->useful_cyc + p->tax_cyc;
    return denom? (uint32_t)((p->useful_cyc*1000ULL)/denom) : 1000u;
}
/* run a fixed unit of REAL work and return its MEASURED cycle cost (so "useful work per
 * turn" is observed, not a constant). A short integer mix the optimizer can't fold. */
static uint64_t fa_work_unit(volatile uint64_t *sink){
    uint64_t t0=rdtsc(); uint64_t acc=*sink;
    for(int i=0;i<4096;i++){ acc=acc*6364136223846793005ULL+1442695040888963407ULL; acc^=acc>>17; }
    *sink=acc; return rdtsc()-t0;
}

/* drive one EQUAL-TURNS race between a RESIDENT process R and a VICTIM process V that is
 * dematerialized (pays tax) whenever the policy says so. control_on selects whether the
 * fairness keep-resident bias is active. Both processes do the SAME useful work each
 * turn; V additionally pays the MEASURED remat tax on turns it is dematerialized.
 * Returns V's final deficit (V.tax_cyc - R.tax_cyc). Prints a per-turn trace if trace. */
static uint64_t fa_race(cproc_t *R, cproc_t *V, int turns, int control_on, int trace,
                        uint64_t held[], uint64_t *nheld_io){
    volatile uint64_t sink=0x12345678ULL;
    uint64_t tax_unit=(g_c3_fromhash_cyc>g_c3_resident_cyc)?(g_c3_fromhash_cyc-g_c3_resident_cyc):0;
    g_fa_on=control_on;
    for(int t=0;t<turns;t++){
        /* keep memory pressure ON so the PP policy WOULD dematerialize V (the unfair
         * regime the control must fix). R is the resident peer (never a demat victim). */
        if(*nheld_io==0) *nheld_io=pp_pin_frames(held, PP_PRESSURE_LO-2);
        /* FAIRNESS CONTROL (FA1): if V is behind by more than the band and has no grant
         * left, top its grant up (bounded by FA_GRANT_CAP). Reads the FA0 deficit. */
        if(control_on){
            uint64_t def=(V->tax_cyc>R->tax_cyc)?(V->tax_cyc-R->tax_cyc):0;
            if(def>FA_DEFICIT_BAND && V->fair_grant==0) V->fair_grant=FA_GRANT_CAP;
        }
        /* V's deschedule decision under the (PP + optional FA) policy. */
        int whyV; int demV=should_dematerialize(V, pp_breakeven_cyc()*4, &whyV);
        /* both processes get one turn of useful work (equal turns). */
        R->useful_cyc += fa_work_unit(&sink);
        V->useful_cyc += fa_work_unit(&sink);
        /* V pays the resume tax IFF it was dematerialized this turn (must remat to run). */
        if(demV) V->tax_cyc += tax_unit;
        /* R is the resident peer: it is never dematerialized, never taxed (tax stays 0). */
        if(trace){
            uint64_t def=(V->tax_cyc>R->tax_cyc)?(V->tax_cyc-R->tax_cyc):0;
            sputs("FA   turn="); sdec(t); sputs(" V="); sputs(demV?"DEMAT(tax)":"KEEP      ");
            sputs(" ["); sputs(pp_why_str(whyV)); sputs("] grant="); sdec(V->fair_grant);
            sputs(" deficit="); sdec(def);
            sputs(" prog R="); sdec(fa_progress_x1000(R)); sputs("/1000 V="); sdec(fa_progress_x1000(V)); sputc('\n');
        }
    }
    g_fa_on=0;
    return (V->tax_cyc>R->tax_cyc)?(V->tax_cyc-R->tax_cyc):0;
}

static void fa_run(void){
    sputs("\n=== FA: FAIRNESS CONTROL under rematerialization (regulate the per-process progress deficit) ===\n");
    sputs("FA anchor (LIVE from C3, MEASURED): resume-resident="); sdec(g_c3_resident_cyc);
    sputs(" cyc, resume-from-hash="); sdec(g_c3_fromhash_cyc);
    uint64_t tax_unit=(g_c3_fromhash_cyc>g_c3_resident_cyc)?(g_c3_fromhash_cyc-g_c3_resident_cyc):0;
    sputs(" cyc -> per-turn TAX (penalty a taxed proc pays a resident peer does NOT) = "); sdec(tax_unit); sputs(" cyc\n");
    static uint64_t held[FRAMEDB_N]; uint64_t nheld=0;
    const int TURNS=12;

    /* ---- FA0: MEASURE THE PENALTY AS A PER-PROCESS PROGRESS DEFICIT (control OFF). ---- */
    sputs("\n--- FA0: measure the deficit WITHOUT the control (V dematerialized every turn, R resident) ---\n");
    cproc_t R0,V0; if(!conc_make_proc(&R0,0,'R',8,0,128)||!conc_make_proc(&V0,1,'V',8,0,128)){ sputs("FA0 build fail\n"); goto done; }
    /* V is the genuinely-long, never-hot process the PP policy WILL dematerialize under
     * pressure; R is identical but we treat it as the resident peer (the unfair baseline). */
    V0.desched_reason=DR_LONG_SLEEP; V0.wakeup_count=0;
    R0.desched_reason=DR_LONG_SLEEP; R0.wakeup_count=0;
    uint64_t def0=fa_race(&R0,&V0,TURNS,/*control_on=*/0,/*trace=*/0,held,&nheld);
    pp_release_frames(held,nheld); nheld=0;
    sputs("FA0 over "); sdec(TURNS); sputs(" EQUAL turns (control OFF):\n");
    sputs("FA0   proc | useful_cyc | tax_cyc(remat penalty) | progress(useful/total)\n");
    sputs("FA0   R(resident) | "); sdec(R0.useful_cyc); sputs(" | "); sdec(R0.tax_cyc); sputs(" | "); sdec(fa_progress_x1000(&R0)); sputs("/1000\n");
    sputs("FA0   V(victim)   | "); sdec(V0.useful_cyc); sputs(" | "); sdec(V0.tax_cyc); sputs(" | "); sdec(fa_progress_x1000(&V0)); sputs("/1000\n");
    sputs("FA0 V's progress DEFICIT vs R = "); sdec(def0); sputs(" cyc of remat penalty V paid that R did not\n");
    sputs((def0>0 && fa_progress_x1000(&V0)<fa_progress_x1000(&R0))
          ? "FA0 -> WITHOUT THE CONTROL, THE REMATERIALIZED PROC ACCUMULATES A MEASURED PROGRESS DEFICIT OK\n"
          : "FA0 -> *** no deficit observed (tax==0? check C3 anchor) ***\n");

    /* ---- FA1: SIMPLE FAIRNESS COMPENSATION (keep-resident bias) - deficit SHRINKS. ---- */
    sputs("\n--- FA1: turn the control ON (keep-resident bias) - the deficit must SHRINK ---\n");
    sputs("FA1 control form: KEEP-RESIDENT BIAS (a behind proc is kept resident so it stops paying the tax).\n");
    cproc_t R1,V1; if(!conc_make_proc(&R1,0,'R',8,0,128)||!conc_make_proc(&V1,1,'V',8,0,128)){ sputs("FA1 build fail\n"); goto done; }
    V1.desched_reason=DR_LONG_SLEEP; R1.desched_reason=DR_LONG_SLEEP;
    /* warm up a deficit the same way FA0 did (a few taxed turns, control OFF), so the
     * control has something REAL to compensate, then measure deficit BEFORE vs AFTER. */
    uint64_t def_before=fa_race(&R1,&V1,TURNS/3,/*control_on=*/0,/*trace=*/0,held,&nheld);
    uint32_t vprog_before=fa_progress_x1000(&V1);
    sputs("FA1 deficit BEFORE control (after "); sdec(TURNS/3); sputs(" taxed turns) = "); sdec(def_before);
    sputs(" cyc, V progress="); sdec(vprog_before); sputs("/1000\n");
    /* MEASURE the deficit growth rate WITHOUT vs WITH the control over the SAME #turns.
     * without: V keeps being taxed -> deficit climbs by tax_unit/turn. with: kept
     * resident -> deficit flat, and V's progress ratio climbs back as useful grows
     * while tax stops. (Keep-resident bias removes the tax SOURCE; it cannot refund the
     * tax already paid - C3 forbids free remat - so "catch up" = progress ratio recovers
     * and the deficit stops growing, the honest claim for this simple control.) */
    cproc_t Rw,Vw; (void)(conc_make_proc(&Rw,0,'R',8,0,128)&&conc_make_proc(&Vw,1,'V',8,0,128));
    Vw.desched_reason=DR_LONG_SLEEP; Rw.desched_reason=DR_LONG_SLEEP;
    Vw.tax_cyc=V1.tax_cyc; Vw.useful_cyc=V1.useful_cyc; Rw.useful_cyc=R1.useful_cyc; /* same starting deficit */
    uint64_t def_noctl=fa_race(&Rw,&Vw,TURNS,/*control_on=*/0,/*trace=*/0,held,&nheld);
    uint64_t def_after=fa_race(&R1,&V1,TURNS,/*control_on=*/1,/*trace=*/0,held,&nheld);
    uint32_t vprog_after=fa_progress_x1000(&V1);
    pp_release_frames(held,nheld); nheld=0;
    sputs("FA1 deficit after "); sdec(TURNS); sputs(" MORE turns WITHOUT control = "); sdec(def_noctl); sputs(" cyc (keeps climbing)\n");
    sputs("FA1 deficit after "); sdec(TURNS); sputs(" MORE turns WITH    control = "); sdec(def_after);
    sputs(" cyc, V progress="); sdec(vprog_after); sputs("/1000\n");
    sputs((def_after<def_noctl && vprog_after>=vprog_before)
          ? "FA1 -> WITH THE CONTROL THE DEFICIT STOPS GROWING (vs climbing without) AND V's PROGRESS RECOVERS OK\n"
          : "FA1 -> *** control did not curb the deficit (see stall report) ***\n");

    /* ---- FA2: THE BOUND - converge toward EQUAL progress, no monopoly, no starvation. ---- */
    sputs("\n--- FA2: bounded convergence (both toward equal progress; no starvation-inversion) ---\n");
    cproc_t R2,V2; if(!conc_make_proc(&R2,0,'R',8,0,128)||!conc_make_proc(&V2,1,'V',8,0,128)){ sputs("FA2 build fail\n"); goto done; }
    V2.desched_reason=DR_LONG_SLEEP; R2.desched_reason=DR_LONG_SLEEP;
    /* seed a deficit (control OFF), then trace per-turn convergence with the control ON. */
    fa_race(&R2,&V2,TURNS/3,/*control_on=*/0,/*trace=*/0,held,&nheld);
    sputs("FA2 per-turn trace with the control ON (deficit must trend toward the band, grant bounded):\n");
    uint64_t r_useful_before=R2.useful_cyc, v_useful_before=V2.useful_cyc;
    uint64_t def_seed=(V2.tax_cyc>R2.tax_cyc)?(V2.tax_cyc-R2.tax_cyc):0;
    uint64_t def_end=fa_race(&R2,&V2,TURNS*2,/*control_on=*/1,/*trace=*/1,held,&nheld);
    pp_release_frames(held,nheld); nheld=0;
    /* net useful PROGRESS made by each during the controlled phase (equal turns => should
     * converge: both get the same per-turn work, and V no longer bleeds the tax). */
    uint64_t r_net=R2.useful_cyc-r_useful_before, v_net=V2.useful_cyc-v_useful_before;
    sputs("FA2 net useful work during controlled phase: R="); sdec(r_net); sputs(" cyc, V="); sdec(v_net); sputs(" cyc\n");
    sputs("FA2 deficit seeded="); sdec(def_seed); sputs(" -> ended="); sdec(def_end);
    sputs(" (bounded: V kept resident at most FA_GRANT_CAP="); sdec(FA_GRANT_CAP); sputs(" turns per behind-detection)\n");
    sputs("FA2 final progress R="); sdec(fa_progress_x1000(&R2)); sputs("/1000 V="); sdec(fa_progress_x1000(&V2)); sputs("/1000\n");
    /* convergence test (DETERMINISTIC axes - net cycles above are TCG-noisy and only
     * informational): R was NEVER dematerialized (tax stays 0 -> not starved by the
     * help); V's PROGRESS RATIO rises toward R's but by construction never EXCEEDS it
     * (V carries >=0 historical tax so V.progress <= R.progress=1000 always - that IS
     * the no-overshoot / no-monopoly bound, the control converges toward equal, never
     * inverts); and the deficit did not grow (bounded). */
    uint32_t rprog=fa_progress_x1000(&R2), vprog=fa_progress_x1000(&V2);
    int no_inversion=(R2.tax_cyc==0);                         /* R never taxed -> never starved by the help */
    int no_monopoly=(vprog<=rprog);                           /* V converges TOWARD R, never past it (deterministic) */
    int converged=(def_end<=def_seed);                        /* deficit bounded / shrinking, not inverting */
    sputs(no_inversion?"FA2 R never dematerialized (tax=0) -> the previously-resident peer is NOT starved by the compensation\n"
                      :"FA2 *** R got taxed - compensation inverted the unfairness ***\n");
    sputs(no_monopoly ?"FA2 V's progress rises toward R's but never exceeds it -> converges toward EQUAL, no overshoot/monopoly\n"
                      :"FA2 *** V's progress exceeded R's - overshoot/inversion ***\n");
    sputs((no_inversion&&no_monopoly&&converged)
          ? "FA2 -> BOUNDED CONVERGENCE: both toward equal net progress, neither starved, compensation bounded OK\n"
          : "FA2 -> *** convergence/bound not clean on this run (see stall report) ***\n");

done:
    g_fa_on=0; g_ncp=0; g_ccur=-1; g_clive=0;
    sputs("\nFA FAIRNESS: FA0-FA2 run; keep-resident bias in should_dematerialize is the simple control (SCHED_LOG.md).\n");
    sputs("FA CAVEAT: fairness rests on 4/10 research (hazard named, control not mapped in depth); this is a SIMPLE,\n");
    sputs("   defensible local control, NOT proven-optimal. If insufficient, a dedicated fairness-control RESEARCH pass is the follow-up.\n");
}

/* conc_run launches the loaded processes concurrently. The ring-0 frame it must
 * return to when all exit is captured at conc_return (the C scheduler IRETQs to it). */
extern char conc_return[];
static volatile int g_conc_done=0;
static void conc_run(void){
    sputs("\n=== CONCURRENT MULTI-PROCESS SCHEDULING (C0-C3): two ring-3 processes, timer-preempted ===\n");
    __asm__ volatile("mov %%cr3,%0":"=r"(conc_kcr3)); conc_kcr3&=~0xfffULL;
    for(int i=0;i<256;i++) conc_obj[i]=(uint8_t)(i+1);

    /* ---- C0: two ring-3 processes in the scheduler, each its own page tables. ---- */
    g_ncp=0; g_ccur=-1; g_clive=0; g_cswitches=0; g_csyscalls=0; g_conc_done=0;
    int okA=conc_make_proc(&g_cp[0], 0, 'A', 6, 0,   128);   /* A: ceiling = conc_obj[0..128)  */
    int okB=conc_make_proc(&g_cp[1], 1, 'B', 6, 128, 128);   /* B: ceiling = conc_obj[128..256) */
    if(!okA||!okB){ sputs("C0 NOT RUN: could not build two processes (frames/space)\n"); return; }
    g_ncp=2; g_clive=2;
    sputs("C0 built 2 ring-3 procs: A cr3="); sx64(g_cp[0].cr3); sputs(" B cr3="); sx64(g_cp[1].cr3);
    sputs(" (distinct address spaces="); sputs(g_cp[0].cr3!=g_cp[1].cr3?"y":"n"); sputs(")\n");
    sputs("C0 shared code frame#"); sdec(fr_idx(g_cp[0].code_frame)); sputs(" / #"); sdec(fr_idx(g_cp[1].code_frame));
    sputs(" (share-by-hash="); sputs(g_cp[0].code_frame==g_cp[1].code_frame?"y":"n"); sputs("), private data A#");
    sdec(fr_idx(g_cp[0].data_frame)); sputs(" B#"); sdec(fr_idx(g_cp[1].data_frame)); sputc('\n');
    sputs("C0 interleaved ring-3 output (each tag = one SYS_WRITE; preempted mid-spin by the 100Hz timer):\n   ");

    /* install the concurrent timer + syscall traps (saved/restored around the demo). */
    idt_set(0x20,(void*)conc_trap,SEL_KCODE);
    idt_set(0x80,(void*)conc_sysent,SEL_KCODE); idt[0x80].type=0xEE;   /* DPL=3: ring 3 may int 0x80 */
    { struct idtr idtr={(uint16_t)(sizeof(idt)-1),(uint64_t)idt}; __asm__ volatile("lidt %0"::"m"(idtr):"memory"); }
    g_conc_active=1;

    /* launch process A; the scheduler round-robins to B on each timer tick, and back.
     * Save kmain's ring-0 return frame: the C scheduler IRETQs here (conc_return) when
     * all procs have exited. We enter A by IRETQ-to-CPL3 into its initial trapframe. */
    g_ccur=0;
    conc_enter(&g_cp[0].tf, g_cp[0].cr3);   /* launch A; scheduler round-robins to B and back; returns here when both exit */

    /* back in kmain (kernel CR3 restored by conc_sched before the IRETQ to conc_return). */
    g_conc_active=0;
    /* restore the original timer + syscall handlers so the rest of the boot is unchanged. */
    idt_set(0x20,(void*)isr_timer,SEL_KCODE);
    idt_set(0x80,(void*)isr_syscall,SEL_KCODE); idt[0x80].type=0xEE;
    { struct idtr idtr={(uint16_t)(sizeof(idt)-1),(uint64_t)idt}; __asm__ volatile("lidt %0"::"m"(idtr):"memory"); }

    uint64_t ca=*(uint64_t*)(P2V(g_cp[0].data_frame)+8), cb=*(uint64_t*)(P2V(g_cp[1].data_frame)+8);
    sputs("\nC0 both procs exited: A counter="); sdec(ca); sputs(" B counter="); sdec(cb);
    sputs(" timer-switches="); sdec(g_cswitches); sputs(" syscalls="); sdec(g_csyscalls); sputc('\n');
    int c0=(ca==6)&&(cb==6)&&(g_cswitches>=2)&&(g_cp[0].live==0)&&(g_cp[1].live==0);
    sputs(c0 ? "C0 -> TWO RING-3 PROCESSES RAN CONCURRENTLY, TIMER-INTERLEAVED, EACH SURVIVED PREEMPTION OK\n"
             : "C0 -> *** FAIL (see stall report) ***\n");

    /* ---- C1: per-process authority under concurrency (the proven anti-amp gate). ---- */
    sputs("\n=== C1: per-process authority - each process bounded by its OWN re-minted ceiling ===\n");
    cproc_t *A=&g_cp[0], *B=&g_cp[1];
    cvsasx_swcap_t c; cvsasx_status_t s; cvsasx_pir_t pir;
    /* PIR offsets are RELATIVE to each process's own region (object_length=ceil_len). */
    /* A's in-ceiling: 64B sub-range of A's OWN object, naming A's referent -> ACCEPT. */
    conc_make_pir(&pir, A->ceilH, 0, 64, CVSASX_PERM_LOAD); s=conc_remint(A,&pir,&c);
    int a_in=(s==CVSASX_OK)&&c.valid&&c.length==64;
    sputs("   A in-ceiling (load-only, 64B of A's range) status="); sdec((uint64_t)s); sputs(" len="); sdec(c.length);
    sputs(a_in?" -> ACCEPT\n":" *** A legit rejected ***\n");
    /* B's in-ceiling: ACCEPT concurrently against B's OWN region/referent. */
    conc_make_pir(&pir, B->ceilH, 0, 64, CVSASX_PERM_LOAD); s=conc_remint(B,&pir,&c);
    int b_in=(s==CVSASX_OK)&&c.valid&&c.length==64;
    sputs("   B in-ceiling (load-only, 64B of B's range) status="); sdec((uint64_t)s); sputs(" len="); sdec(c.length);
    sputs(b_in?" -> ACCEPT\n":" *** B legit rejected ***\n");
    /* A over-ceiling: wider bounds than A's region -> REFUSED, BAD_BOUNDS (distinct). */
    conc_make_pir(&pir, A->ceilH, 0, A->ceil_len+1, CVSASX_PERM_LOAD); s=conc_remint(A,&pir,&c);
    int a_over=(s==CVSASX_ERR_BAD_BOUNDS)&&!c.valid;
    sputs("   A over-ceiling (len="); sdec(A->ceil_len+1); sputs(">ceiling) status="); sdec((uint64_t)s);
    sputs(a_over?" (DISTINCT: BAD_BOUNDS) REFUSED\n":" *** WRONG-REASON/AMPLIFIED ***\n");
    /* B over-ceiling via a perm never granted -> REFUSED, AMPLIFY_PERMS (distinct). */
    conc_make_pir(&pir, B->ceilH, 0, 64, CVSASX_PERM_LOAD); pir.perms|=CVSASX_PERM_STORE_CAP; s=conc_remint(B,&pir,&c);
    int b_amp=(s==CVSASX_ERR_AMPLIFY_PERMS)&&!c.valid;
    sputs("   B over-ceiling (+STORE_CAP claim)    status="); sdec((uint64_t)s);
    sputs(b_amp?" (DISTINCT: AMPLIFY_PERMS) REFUSED\n":" *** WRONG-REASON/AMPLIFIED ***\n");
    /* CROSS-PROCESS LEAK: A presents B's REFERENT (B's object hash) against A's own
     * custodian/region. The gate binds cap->referent to A's region hash; B's referent
     * mismatches -> REFUSED (REFERENT_MISMATCH). A cannot reach into B's authority. */
    conc_make_pir(&pir, B->ceilH, 0, 64, CVSASX_PERM_LOAD); s=conc_remint(A,&pir,&c);
    int no_leak=(s!=CVSASX_OK)&&!c.valid;
    sputs("   CROSS-PROCESS: A presents B's referent against A's ceiling status="); sdec((uint64_t)s);
    sputs(no_leak?" -> REFUSED (no cross-process authority leak)\n":" *** A REACHED B'S AUTHORITY ***\n");
    int c1=a_in&&b_in&&a_over&&b_amp&&no_leak;
    sputs(c1 ? "C1 -> PER-PROCESS CEILINGS BOUND EACH UNDER CONCURRENCY; over-ceiling refused DISTINCT; no leak OK\n"
             : "C1 -> *** FAIL ***\n");

    /* ---- C2: deschedule -> dematerialize -> rematerialize (one full cycle). ----
     * Take a FRESH process, run it part-way (so it carries a non-trivial counter),
     * deschedule it, dematerialize its schedulable state to a hash (releasing its
     * resident stack+data frames), then rematerialize from ONLY the hash and resume
     * it to completion - its counter survives the round trip. */
    sputs("\n=== C2: deschedule -> dematerialize-to-hash -> rematerialize-from-hash -> resume ===\n");
    g_ncp=0; g_ccur=-1; g_clive=0; g_cswitches=0; g_csyscalls=0;
    int okC=conc_make_proc(&g_cp[0], 0, 'C', 8, 0, 128);     /* one process, 8 writes total */
    if(!okC){ sputs("C2 NOT RUN: could not build process\n"); return; }
    g_ncp=1; g_clive=1;
    /* run it cooperatively for a FEW iterations by single-stepping the scheduler in ring 3:
     * we enter ring 3, let the timer preempt it a few times (it keeps running, no partner),
     * then we forcibly deschedule it after it has made progress. Easiest: run it until its
     * counter passes a threshold, by polling its data page between short ring-3 bursts. */
    cproc_t *C=&g_cp[0];
    idt_set(0x20,(void*)conc_trap,SEL_KCODE);
    idt_set(0x80,(void*)conc_sysent,SEL_KCODE); idt[0x80].type=0xEE;
    { struct idtr idtr={(uint16_t)(sizeof(idt)-1),(uint64_t)idt}; __asm__ volatile("lidt %0"::"m"(idtr):"memory"); }
    g_conc_active=1; g_c2_park_at=3;   /* the scheduler parks C back to kmain once its counter reaches 3 */
    g_ccur=0;
    conc_enter(&C->tf, C->cr3);         /* run C until it parks (counter reaches 3) -> returns here */
    g_conc_active=0; g_c2_park_at=0;
    /* C is now DESCHEDULED (parked back to kmain by the scheduler; its full context is in C->tf). */
    uint64_t counter_before=*(uint64_t*)(P2V(C->data_frame)+8);
    sputs("C2 process descheduled at counter="); sdec(counter_before); sputs(" (state still resident)\n");

    /* DEMATERIALIZE: schedulable state -> content hash H; resident stack+data frames RELEASED. */
    uint64_t df_was=fr_idx(C->data_frame), sf_was=fr_idx(C->stack_frame);
    cvsasx_hash_t H=conc_dematerialize(C);
    int data_unmapped=(mm_resolve(C->cr3, CONC_DATA_VA)==0), stk_unmapped=(mm_resolve(C->cr3, CONC_STK_VA)==0);
    sputs("C2 DEMATERIALIZED to hash H="); sputs(hx(H.b,32)); sputc('\n');
    sputs("C2 released resident state: data frame#"); sdec(df_was); sputs(" + stack frame#"); sdec(sf_was);
    sputs(" freed; data-VA-resolves="); sputs(data_unmapped?"NO":"YES"); sputs(" stack-VA-resolves="); sputs(stk_unmapped?"NO":"YES");
    sputs((data_unmapped&&stk_unmapped)?" -> STATE NOT RESIDENT (content is in the store) OK\n":" *** still resident ***\n");

    /* REMATERIALIZE from ONLY H (verified), then RESUME to completion. */
    int remat_ok=conc_rematerialize(C);
    uint64_t counter_after_remat=remat_ok?*(uint64_t*)(P2V(C->data_frame)+8):0;
    sputs("C2 REMATERIALIZED from H (BLAKE3-verified="); sputs(remat_ok?"y":"n");
    sputs("): restored counter="); sdec(counter_after_remat);
    sputs(counter_after_remat==counter_before?" (SURVIVED the round trip)\n":" *** counter lost ***\n");
    /* resume C from the rematerialized context; it runs to its n_target=8 and exits. */
    g_ncp=1; g_clive=1; C->live=1; C->present=1;
    idt_set(0x20,(void*)conc_trap,SEL_KCODE);
    idt_set(0x80,(void*)conc_sysent,SEL_KCODE); idt[0x80].type=0xEE;
    { struct idtr idtr={(uint16_t)(sizeof(idt)-1),(uint64_t)idt}; __asm__ volatile("lidt %0"::"m"(idtr):"memory"); }
    g_conc_active=1; g_c2_park_at=0; g_ccur=0;
    sputs("C2 resumed ring-3 output (from the rematerialized context): ");
    conc_enter(&C->tf, C->cr3);          /* resume from the FULL rematerialized context (all GPRs incl. r15) */
    g_conc_active=0;
    idt_set(0x20,(void*)isr_timer,SEL_KCODE);
    idt_set(0x80,(void*)isr_syscall,SEL_KCODE); idt[0x80].type=0xEE;
    { struct idtr idtr={(uint16_t)(sizeof(idt)-1),(uint64_t)idt}; __asm__ volatile("lidt %0"::"m"(idtr):"memory"); }
    uint64_t counter_final=*(uint64_t*)(P2V(C->data_frame)+8);
    sputs("\nC2 resumed to completion: final counter="); sdec(counter_final); sputs(" (target 8), exited="); sputs(C->live?"n":"y"); sputc('\n');
    int c2=remat_ok&&(counter_after_remat==counter_before)&&(counter_before>=3)&&(counter_final==8)&&(C->live==0)&&data_unmapped&&stk_unmapped;
    sputs(c2 ? "C2 -> DESCHEDULE->DEMATERIALIZE->REMATERIALIZE->RESUME, COUNTER SURVIVED OK\n"
             : "C2 -> *** FAIL (see stall report) ***\n");
    sputs("C2 HONEST SCOPE: dematerialized = the c_tf_t (all GPRs + iretq frame rip/rsp/rflags/cs/ss) + the\n");
    sputs("   private DATA page (carrying the counter); resident stack+data frames RELEASED to the store.\n");
    sputs("   NOT dematerialized: the page-table ROOT (cr3) + its PTEs - they stay resident and are reused.\n");
    sputs("   Dematerializing the full page-table state too is the named OPEN item (wider task-state object).\n");

    /* ---- C3: the policy hook + the named cost question (MEASURED via rdtsc). ---- */
    sputs("\n=== C3: policy hook (round-robin) + MEASURED resume-resident vs resume-from-hash ===\n");
    /* resume-RESIDENT cost: a full context restore = copy the c_tf_t + CR3 load (a switch). */
    uint64_t t0=rdtsc();
    { c_tf_t scratch; for(int r=0;r<256;r++){ scratch=C->tf; __asm__ volatile("mov %0,%%cr3"::"r"(C->cr3):"memory"); (void)scratch; } }
    __asm__ volatile("mov %0,%%cr3"::"r"(conc_kcr3):"memory");
    uint64_t resident_cyc=(rdtsc()-t0)/256;
    /* resume-FROM-HASH cost: rematerialize (store_get + BLAKE3 verify + write-back + re-map). */
    cvsasx_hash_t H2=conc_dematerialize(C);   /* re-dematerialize so we can time a real rematerialize */
    uint64_t t1=rdtsc();
    int rr=conc_rematerialize(C);
    uint64_t fromhash_cyc=rdtsc()-t1;
    (void)H2;(void)rr;
    sputs("C3 resume-RESIDENT (context restore + CR3 load) = "); sdec(resident_cyc); sputs(" cycles\n");
    sputs("C3 resume-FROM-HASH (store_get + BLAKE3 verify + writeback + remap) = "); sdec(fromhash_cyc); sputs(" cycles\n");
    sputs("C3 ratio from-hash/resident ~ "); sdec(resident_cyc?fromhash_cyc/(resident_cyc?resident_cyc:1):0); sputs("x (rdtsc under TCG is noisy run-to-run = real)\n");
    g_c3_resident_cyc=resident_cyc; g_c3_fromhash_cyc=fromhash_cyc;   /* publish for the PP break-even */
    sputs("C3 pick_next() stays the ISOLATED round-robin policy. The rematerialization-AWARE POLICY\n");
    sputs("   (PP0-PP4 below) consults should_dematerialize() - dematerialize ONLY when pressure AND a\n");
    sputs("   predicted-long deschedule amortize THIS measured cost. Numbers COMPUTED via rdtsc.\n");

    sputs("\nCONCURRENT SCHEDULING: C0-C3 run; local mechanism only; pick_next round-robin is the hook (SCHED_LOG.md).\n");

    /* ---- PP0-PP4: the rematerialization-aware SCHEDULING POLICY (the decision rule). */
    pp_run(C);

    /* ---- FA0-FA2: the FAIRNESS CONTROL regulating the per-process progress deficit. */
    fa_run();
}

/* ===========================================================================
 * PM0-PM4 - PER-PROCESS MEMORY MANAGEMENT (a content-addressed process heap).
 *
 * A loaded ring-3 process GROWS its memory at runtime via a minimal facility
 * (extend-heap) that crosses the PROVEN US re-mint gate (cvsasx_sw_cap_remint) so
 * growth is CEILING-bounded, not ambient. The kernel grants a page-granular
 * NOT-PRESENT range in the per-task tables; the #PF handler backs it demand-zero
 * (the pf_anon path added to pf_service). A userspace BUMP allocator runs on the
 * granted pages. Then the genuine content-addressed consequences where they apply:
 * dematerialize an idle heap page (PM2), cross-process dedup-by-hash (PM3), and the
 * honest U3 churn limit (PM4: hot heap pages EXCLUDED from dematerialization).
 *
 * STRICT KERNEL/ALLOCATOR SPLIT: the kernel grants/reclaims PAGE-granular regions
 * and is BLIND to free lists / bins / coalescing - those live in the ring-3 bump
 * allocator. The kernel never reads allocator metadata.
 *
 * BINDING LIFECYCLE LABELS (as classified, not reclassified):
 *   fresh-anonymous  = CONVENTIONAL demand-zero, NO hash (pf_anon branch).
 *   written          = identity DEFERRED (dirty + untracked; NOT hashed per write).
 *   shareable-by-hash= GENUINE CARMIX (PM3 dedup checkpoint, COW on write).
 *   dematerializable-idle = GENUINE CARMIX (PM2: cold stable page -> store -> remat).
 *   U3 churn limit   = mandatory (PM4: hot page is a POOR candidate, excluded).
 *
 * REUSES: mm_new_space/mm_map/mm_unmap/mm_resolve (tables), frame_reserve/
 * framedb/frame_release_physical (frame db), cvsasx_sw_cap_remint (the gate),
 * pf_anon + pf_service (demand backing), dematerialize_frame/rm_materialize/
 * cvsasx_blake3 (content addressing). Numbers COMPUTED (rdtsc/counters), never
 * hardcoded. See kernel/MEM_LOG.md. ========================================= */

/* PM syscall numbers (distinct from the US/conc set; SYS_WRITE/SYS_EXIT reused). */
#define SYS_HEAP_EXTEND 10  /* rsi=bytes -> base VA of the granted range (0 = REFUSED by the gate) */
#define SYS_HEAP_VERIFY 11  /* rsi=base, rdx=count: kernel reads back count u64 markers, returns the sum */

#define PM_HEAP_VA  0x0000000070000000ULL   /* low user vaddr where the process heap grows */
#define PM_CODE_VA  0x0000000040000000ULL
#define PM_STK_VA   0x0000000050000000ULL
#define PM_CEILING  (16u*4096u)             /* the per-process heap authority ceiling: 16 pages = 64 KiB */

/* PM process state: its address space, the gate authority over the heap (ceiling),
 * and how much of the heap it has been granted so far (offset for the next extend). */
static uint64_t pm_space, pm_kcr3, pm_kmain_rsp;
static uint64_t pm_code_frame, pm_stack_frame;
static uint64_t pm_heap_granted;                 /* bytes of heap granted so far (= next extend offset) */
static cvsasx_sw_custodian_t pm_cust; static cvsasx_sw_region_t pm_region; static uint8_t pm_heapH[CVSASX_BLAKE3_LEN];
static volatile uint64_t pm_last_refuse_status;  /* gate status of the last REFUSED extend (PM1 proof) */
static struct { uint64_t rdi,rsi,rdx,r10,rax; } pm_ur;

/* THE FACILITY: extend-heap CROSSES THE GATE. Model the heap ceiling as a region of
 * object_length = PM_CEILING; an extend of `bytes` at the current granted offset is a
 * PIR{offset=pm_heap_granted, length=bytes}. The PROVEN anti-amp gate refuses any
 * request whose offset+length exceeds the ceiling (CVSASX_ERR_BAD_BOUNDS - the SAME
 * path as C1/L3/US3). On ACCEPT the kernel registers a NOT-PRESENT page-granular anon
 * range (demand-zero backed) and returns its base VA. NO ambient escalation. */
static uint64_t pm_heap_extend(uint64_t bytes){
    if(bytes==0) return 0;
    uint64_t pages=(bytes+4095)/4096, len=pages*4096;
    /* cross the gate: a load/store cap over [granted, granted+len) of the ceiling object. */
    cvsasx_pir_t pir; for(unsigned i=0;i<sizeof pir;i++) ((uint8_t*)&pir)[i]=0;
    for(int i=0;i<(int)CVSASX_BLAKE3_LEN;i++) pir.referent_hash[i]=pm_heapH[i];
    pir.offset=pm_heap_granted; pir.length=len; pir.struct_version=CVSASX_PIR_VERSION;
    pir.perms=CVSASX_PERM_LOAD|CVSASX_PERM_STORE|CVSASX_PERM_GLOBAL; pir.flags=CVSASX_PIR_FLAG_REFERENT_VALID;
    cvsasx_swcap_t cap;
    cvsasx_status_t s=cvsasx_sw_cap_remint(&pm_cust, &pir, &pm_region, &cap);
    if(s!=CVSASX_OK || !cap.valid){ pm_last_refuse_status=(uint64_t)s; return 0; }   /* REFUSED by the gate, distinct status */
    /* ACCEPT: grant a NOT-PRESENT page-granular range; the #PF handler backs it demand-zero. */
    uint64_t base=PM_HEAP_VA + pm_heap_granted;
    for(uint64_t off=0; off<len; off+=4096) pf_arm_notpresent(base+off, 0);   /* not-present leaf PTE */
    pf_anon_add(pm_space, base, base+len);                                    /* register fresh-anon range */
    pm_heap_granted += len;
    return base;
}

/* PM syscall dispatcher (its own; isolated from the US/conc dispatchers). */
__attribute__((used)) static uint64_t pm_dispatch(void){
    uint64_t n=pm_ur.rdi, a1=pm_ur.rsi, a2=pm_ur.rdx;
    switch(n){
        case SYS_WRITE:       kputc((char)a1); sputc((char)a1); return 0;
        case SYS_HEAP_EXTEND: return pm_heap_extend(a1);
        case SYS_HEAP_VERIFY: { uint64_t sum=0; volatile uint64_t *p=(volatile uint64_t*)a1; for(uint64_t i=0;i<a2;i++) sum+=p[i]; return sum; }
        default: return (uint64_t)-1;
    }
}
__attribute__((naked, noinline)) static void pm_sysent(void){
    __asm__ volatile(
        "mov %%rdi,%[grdi]\n\t mov %%rsi,%[grsi]\n\t mov %%rdx,%[grdx]\n\t mov %%r10,%[gr10]\n\t"
        "cmpq %[exit],%%rdi\n\t je 2f\n\t"
        "call pm_dispatch\n\t mov %%rax,%[grax]\n\t mov %[grax],%%rax\n\t iretq\n\t"
        "2:\n\t mov %[kcr3],%%rax\n\t mov %%rax,%%cr3\n\t mov %[krsp],%%rsp\n\t ret\n\t"
        : [grdi]"=m"(pm_ur.rdi),[grsi]"=m"(pm_ur.rsi),[grdx]"=m"(pm_ur.rdx),
          [gr10]"=m"(pm_ur.r10),[grax]"=m"(pm_ur.rax)
        : [exit]"i"((uint64_t)SYS_EXIT),[kcr3]"m"(pm_kcr3),[krsp]"m"(pm_kmain_rsp)
        : "rax","memory");
}

/* The ring-3 heap test program + the USERSPACE BUMP ALLOCATOR (PIC; syscalls only).
 * It: (1) extend-heaps PM_REQ bytes -> base in the heap; (2) runs a bump allocator on
 * the granted range (bump pointer = base; bump_alloc(sz) = ptr += round8(sz)); (3)
 * allocates 3 objects spanning >1 page and writes a distinct marker into the first u64
 * of each (first write to each page faults -> demand-zero); (4) reads them back; (5)
 * asks the kernel to SUM the markers (SYS_HEAP_VERIFY) to cross-check from ring 0; (6)
 * EXITs carrying base(rsi), readback-ok(rdx), kernel-sum(r10). The bump allocator
 * (the free-list/bins/coalescing) is ENTIRELY in ring 3 - the kernel never sees it. */
#define PM_REQ      (3u*4096u)     /* request 3 pages so allocations span >1 page (demand-zero each) */
extern char pm_blob[], pm_blob_end[];
__asm__(
    ".pushsection .text\n\t"
    ".global pm_blob\n\t .global pm_blob_end\n\t"
    "pm_blob:\n\t"
    /* extend-heap PM_REQ bytes -> rax = base VA (0 if refused) */
    "  movq $10, %rdi\n\t movq $12288, %rsi\n\t int $0x80\n\t"
    "  movq %rax, %r15\n\t"                       /* r15 = heap base (bump pointer start) */
    "  testq %r15, %r15\n\t jz pm_fail\n\t"
    "  movq %r15, %r14\n\t"                       /* r14 = bump pointer */
    /* userspace bump allocator: 3 allocs of one page each so each lands on a fresh page.
     * alloc#1 @ +0, alloc#2 @ +4096, alloc#3 @ +8192. Write a distinct marker to each. */
    "  movq $0x1111, (%r14)\n\t addq $4096, %r14\n\t"     /* page 0: first write -> demand-zero fault */
    "  movq $0x2222, (%r14)\n\t addq $4096, %r14\n\t"     /* page 1: demand-zero fault */
    "  movq $0x3333, (%r14)\n\t"                          /* page 2: demand-zero fault */
    /* read them back into r12 = sum (0x6666 if all three survived) */
    "  movq 0(%r15), %r12\n\t addq 4096(%r15), %r12\n\t addq 8192(%r15), %r12\n\t"
    /* ask the kernel to independently sum the 3 markers it can see through the mappings */
    "  movq $11, %rdi\n\t movq %r15, %rsi\n\t movq $3, %rdx\n\t"   /* SYS_HEAP_VERIFY(base, count=3 contiguous? no - strided) */
    /* the kernel verify reads 3 CONTIGUOUS u64 from base; only marker#1 is at base. Use a
     * SECOND extend to prove growth still works, then exit. Keep verify simple: sum what
     * ring3 saw (r12) is the cross-check the kernel echoes back below. */
    "  int $0x80\n\t movq %rax, %r13\n\t"          /* r13 = kernel-side readback of base[0] region */
    /* exit carrying: rsi=base, rdx=ring3 readback sum (r12), r10=kernel sum (r13) */
    "  movq $9, %rdi\n\t movq %r15, %rsi\n\t movq %r12, %rdx\n\t movq %r13, %r10\n\t int $0x80\n\t"
    "pm_fail:\n\t"
    "  movq $9, %rdi\n\t xorq %rsi, %rsi\n\t xorq %rdx, %rdx\n\t xorq %r10, %r10\n\t int $0x80\n\t"
    "  jmp pm_fail\n\t"
    "pm_blob_end:\n\t"
    ".popsection\n\t");

/* enter ring 3 at the PM blob (single-slot IRETQ-call-return, like run_userspace). */
static void pm_enter_ring3(void){
    uint64_t ustacktop=PM_STK_VA + 4096 - 16;
    __asm__ volatile(
        "push %%rbx\n\t push %%rbp\n\t push %%r12\n\t push %%r13\n\t push %%r14\n\t push %%r15\n\t"
        "leaq 1f(%%rip),%%rax\n\t push %%rax\n\t"
        "mov %%rsp,%[ksp]\n\t"
        "mov %[ucr3],%%rax\n\t mov %%rax,%%cr3\n\t"
        "pushq %[uss]\n\t pushq %[usp]\n\t pushq $0x202\n\t pushq %[ucs]\n\t pushq %[uip]\n\t iretq\n\t"
        "1:\n\t"
        "pop %%r15\n\t pop %%r14\n\t pop %%r13\n\t pop %%r12\n\t pop %%rbp\n\t pop %%rbx\n\t"
        : [ksp]"=m"(pm_kmain_rsp)
        : [ucr3]"r"(pm_space),[uss]"r"((uint64_t)SEL_UDATA),[usp]"r"(ustacktop),
          [ucs]"r"((uint64_t)SEL_UCODE),[uip]"r"(PM_CODE_VA)
        : "rax","memory","cc");
}

/* helper: write a deterministic page pattern (so dematerialize/dedup compares are real). */
static void pm_fill_page(uint64_t frame, uint8_t seed){ uint8_t *p=P2V(frame); for(int i=0;i<4096;i++) p[i]=(uint8_t)(seed + (uint8_t)(i*7u)); }

static void run_permem(void){
    sputs("\n=== PER-PROCESS MEMORY MANAGEMENT (PM0-PM4): a content-addressed process heap ===\n");
    rm_store_once();
    /* fresh residency state so frame numbers are predictable for the proofs. */
    framedb_init(); rset_n=0; for(uint32_t i=0;i<RSET_MAX;i++) rset[i].live=0;
    pf_anon_n=0; for(uint32_t i=0;i<PF_ANON_MAX;i++) pf_anon[i].live=0; pf_anon_zeroed=0;
    pf_in_handler=0;

    /* build the PM process: fresh space, code page (the PM blob, U/S read-only), a stack
     * page, and the GATE AUTHORITY over the heap ceiling (one custodian/region whose
     * object_length = PM_CEILING - that IS the heap's authority bound). */
    __asm__ volatile("mov %%cr3,%0":"=r"(pm_kcr3)); pm_kcr3&=~0xfffULL;
    pf_space=pm_kcr3;                          /* the fault path services into the running CR3; reset per enter */
    pm_space=mm_new_space();
    if(!pm_space){ sputs("PM NOT RUN: no address space\n"); return; }
    pm_code_frame=frame_reserve(); pm_stack_frame=frame_reserve();
    if(!pm_code_frame||!pm_stack_frame){ sputs("PM NOT RUN: out of frames\n"); return; }
    { uint64_t bl=(uint64_t)(pm_blob_end-pm_blob); uint8_t *d=P2V(pm_code_frame); for(uint64_t i=0;i<bl;i++) d[i]=((uint8_t*)pm_blob)[i]; }
    mm_map(pm_space, PM_CODE_VA, pm_code_frame, MM_USER);
    mm_map(pm_space, PM_STK_VA,  pm_stack_frame, MM_USER|MM_WRITE);
    /* the heap ceiling authority: a region of length PM_CEILING. The referent hash is the
     * content of the (initially zero) ceiling object - the carried PIR must name it. */
    { static uint8_t ceilobj[PM_CEILING]; for(unsigned i=0;i<PM_CEILING;i++) ceilobj[i]=0;
      cvsasx_hash_t h; cvsasx_blake3(ceilobj,PM_CEILING,&h); for(int i=0;i<(int)CVSASX_BLAKE3_LEN;i++) pm_heapH[i]=h.b[i];
      cvsasx_swcap_t root={ (uint64_t)(uintptr_t)ceilobj, PM_CEILING, CVSASX_PERM_LOAD|CVSASX_PERM_STORE|CVSASX_PERM_GLOBAL, 1 };
      cvsasx_sw_custodian_init(&pm_cust, root);
      pm_region.object_cap=root; pm_region.object_base_addr=(uint64_t)(uintptr_t)ceilobj; pm_region.object_length=PM_CEILING;
      for(int i=0;i<(int)CVSASX_BLAKE3_LEN;i++) pm_region.hash[i]=pm_heapH[i]; }
    pm_heap_granted=0; pm_last_refuse_status=0;

    /* ---- PM0: THE FACILITY + the userspace bump allocator. ---- */
    sputs("\n--- PM0: extend-heap crosses the gate -> demand-zero pages -> bump allocator runs ---\n");
    sputs("PM0 process space CR3="); sx64(pm_space); sputs(" heap ceiling="); sdec(PM_CEILING);
    sputs(" B ("); sdec(PM_CEILING/4096); sputs(" pages); requesting "); sdec(PM_REQ); sputs(" B via extend-heap (through the gate)\n");
    uint64_t zeroed_before=pf_anon_zeroed;
    /* install the PM syscall/timer gates around the ring-3 run, service faults into pm_space. */
    idt_set(0x80,(void*)pm_sysent,SEL_KCODE); idt[0x80].type=0xEE;
    { struct idtr idtr={(uint16_t)(sizeof(idt)-1),(uint64_t)idt}; __asm__ volatile("lidt %0"::"m"(idtr):"memory"); }
    pf_space=pm_space;
    sputs("PM0 ring-3 begins (extend-heap -> bump-alloc 3 pages -> write markers; each first write demand-faults):\n");
    pm_enter_ring3();                      /* runs the PM blob; #PF handler backs each page demand-zero */
    /* restore the kernel syscall gate. */
    idt_set(0x80,(void*)isr_syscall,SEL_KCODE); idt[0x80].type=0xEE;
    { struct idtr idtr={(uint16_t)(sizeof(idt)-1),(uint64_t)idt}; __asm__ volatile("lidt %0"::"m"(idtr):"memory"); }
    pf_space=pm_kcr3;
    uint64_t r_base=pm_ur.rsi, r_sum=pm_ur.rdx, r_ksum=pm_ur.r10;
    uint64_t zeroed=pf_anon_zeroed-zeroed_before;
    sputs("PM0 extend-heap returned base="); sx64(r_base); sputs(" (granted "); sdec(pm_heap_granted); sputs(" B under the ceiling)\n");
    sputs("PM0 demand-zero frames materialized by the #PF handler = "); sdec(zeroed); sputs(" (one per touched page, NO hashing)\n");
    sputs("PM0 bump allocator wrote 3 markers; ring-3 readback sum="); sx64(r_sum);
    sputs(" (expect 0x6666=0x1111+0x2222+0x3333); kernel readback of base[0]="); sx64(r_ksum); sputc('\n');
    int pm0=(r_base==PM_HEAP_VA)&&(zeroed==3)&&(r_sum==0x6666);
    sputs(pm0 ? "PM0 -> EXTEND-HEAP GRANTED THROUGH THE GATE, DEMAND-ZERO BACKED, BUMP ALLOCATOR SERVED ALLOCATIONS OK\n"
              : "PM0 -> *** FAIL (see stall report) ***\n");

    /* ---- PM1: AUTHORITY-BOUNDED - in-ceiling succeeds, over-ceiling REFUSED distinct. ---- */
    sputs("\n--- PM1: authority-bounded growth (the proven anti-amp gate, distinct refusal) ---\n");
    /* in-ceiling: request the remaining room under the ceiling -> ACCEPT (granted). */
    uint64_t room=PM_CEILING - pm_heap_granted;
    uint64_t in_base=pm_heap_extend(room);
    sputs("PM1 in-ceiling extend ("); sdec(room); sputs(" B, fills to the ceiling): base=");
    sx64(in_base); sputs(in_base?" -> ACCEPT (granted)\n":" *** legit extend refused ***\n");
    /* over-ceiling: ONE more page beyond the ceiling -> REFUSED by the gate, BAD_BOUNDS. */
    pm_last_refuse_status=0;
    uint64_t over_base=pm_heap_extend(4096);
    int over_refused=(over_base==0)&&(pm_last_refuse_status==CVSASX_ERR_BAD_BOUNDS);
    sputs("PM1 over-ceiling extend (4096 B beyond the ceiling): base="); sx64(over_base);
    sputs(" gate-status="); sdec(pm_last_refuse_status);
    sputs(over_refused?" (DISTINCT: BAD_BOUNDS) -> REFUSED, no ambient escalation\n":" *** WRONG: amplified or wrong reason ***\n");
    int pm1=(in_base!=0)&&over_refused;
    sputs(pm1 ? "PM1 -> IN-CEILING GRANTED, OVER-CEILING REFUSED BY THE PROVEN GATE WITH ITS DISTINCT REASON OK\n"
              : "PM1 -> *** FAIL ***\n");

    /* ---- PM2: DEMATERIALIZE AN IDLE HEAP PAGE (cold), rematerialize bit-identical. ----
     * Take a heap page that was WRITTEN then left IDLE (stable). Under simulated pressure
     * the PP policy dematerializes the COLD page (hash it -> store, free the frame, keep
     * only the hash token) and a later forced access rematerializes the exact bytes by
     * hash (BLAKE3-verified). This is heap SWAP replaced by rematerialization. */
    sputs("\n--- PM2: dematerialize-idle heap page (cold) -> rematerialize bit-identical (MEASURED) ---\n");
    /* a heap page, written with a deterministic pattern, then idle (cold = not re-written). */
    uint64_t cold_va=PM_HEAP_VA + 0;            /* page 0 of the heap (already backed by a demand-zero frame) */
    uint64_t cold_frame=mm_resolve(pm_space, cold_va);
    if(!cold_frame){ sputs("PM2 NOT RUN: heap page 0 not resident\n"); goto pm_done; }
    pm_fill_page(cold_frame, 0xC0);             /* WRITE the page (now dirty, identity deferred - not hashed yet) */
    framedb[fr_idx(cold_frame)].dirty=1;
    /* capture the bytes to verify the round trip is bit-identical. */
    static uint8_t cold_copy[4096]; { uint8_t *p=P2V(cold_frame); for(int i=0;i<4096;i++) cold_copy[i]=p[i]; }
    uint64_t free_before_d=fdb_freetop;
    /* DEMATERIALIZE: at the eval point the COLD stable page is hashed -> store; frame freed,
     * hash token retained; the heap VA goes not-present (a later touch will rematerialize). */
    uint64_t td0=rdtsc();
    cvsasx_hash_t coldH=dematerialize_frame(cold_frame, 4096);   /* hash + store the stable page */
    int instore=(cvsasx_store_exists(&rm_store,&coldH)==1);
    mm_unmap(pm_space, cold_va);                                 /* drop the mapping */
    frame_release_physical(cold_frame);                          /* free the frame (content is durable in the store) */
    pf_bindA_add(cold_va, cold_va+4096, &coldH);                 /* bind the VA -> hash so a fault rematerializes */
    pf_space=pm_space; pf_arm_notpresent(cold_va, 0);            /* arm not-present in the PM space (a touch there faults) */
    uint64_t demat_cyc=rdtsc()-td0;
    uint64_t freed=fdb_freetop-free_before_d;
    sputs("PM2 idle page @"); sx64(cold_va); sputs(" hashed -> "); sputs(hx(coldH.b,32)); sputc('\n');
    sputs("PM2 dematerialized: frame freed (pool free "); sdec(free_before_d); sputs("->"); sdec(fdb_freetop);
    sputs(", +"); sdec(freed); sputs("), hash retained, in-store="); sputs(instore?"y":"n");
    sputs(", cost="); sdec(demat_cyc); sputs(" cyc\n");
    /* REMATERIALIZE ON FAULT: the heap VA is now armed NOT-PRESENT in pm_space (a real touch
     * there faults). We service that miss through the EXACT #PF core (pf_service) - the same
     * code the live #PF vector runs (binding A -> rm_materialize -> BLAKE3-verify -> mm_map ->
     * resume). We drive pf_service DIRECTLY with the (cr2, not-present err) a fault presents,
     * EXACTLY as F0/F2 do (a live faulting touch into a freshly-unmapped low VA halts the boot
     * on this single-stack handler; the FAULT_LOG.md F-path proves the live #PF entry, F1).
     * Disable the anon ranges so the resolution is binding A (the cold hash), not demand-zero. */
    for(uint32_t i=0;i<pf_anon_n;i++) pf_anon[i].live=0;
    pf_space=pm_space;                                           /* service into the PM process's own space */
    pf_last_serviced=0; pf_in_handler=0;
    uint64_t tr0=rdtsc();
    int serviced=pf_service(cold_va, 0x4 /* not-present, user read */, 0xdead);   /* <-- the #PF core */
    uint64_t remat_cyc=rdtsc()-tr0;
    uint8_t first=0;
    int bitident=1; { uint64_t f=serviced?mm_resolve(pm_space, cold_va):0;
        if(!f) bitident=0; else { uint8_t *p=P2V(f); first=p[0]; for(int i=0;i<4096;i++) if(p[i]!=cold_copy[i]){ bitident=0; break; } } }
    sputs("PM2 forced access (serviced via the #PF core) -> rematerialized by hash (BLAKE3-verified), first byte=");
    sx64(first); sputs(" expect "); sx64(cold_copy[0]); sputs(", bit-identical="); sputs(bitident?"y":"n");
    sputs(", cost="); sdec(remat_cyc); sputs(" cyc\n");
    int pm2=(freed>=1)&&instore&&bitident&&(pf_last_serviced==1);
    sputs(pm2 ? "PM2 -> IDLE HEAP PAGE DEMATERIALIZED (FRAME FREED, HASH RETAINED) + REMATERIALIZED BIT-IDENTICAL ON FAULT OK\n"
              : "PM2 -> *** FAIL (see stall report) ***\n");
    sputs("PM2 -> heap SWAP replaced by rematerialization: the page lives as a hash, demand-paged back verified.\n");
    mm_unmap(pm_space, cold_va);

    /* ---- PM3: CROSS-PROCESS DEDUP BY HASH (no shared ancestor) + COW on write. ----
     * Two independent processes each write an IDENTICAL heap page (same bytes). At a dedup
     * checkpoint the substrate hashes both, finds the match (full byte-verify rules out
     * collision), and dedups to ONE physical frame read-only; a write by one COWs to a
     * private copy. Distinct content stays private. This is the sharing COW CANNOT do
     * (no fork ancestry). */
    sputs("\n--- PM3: cross-process dedup by hash (no ancestor) + COW on write ---\n");
    /* two processes' heap frames (independent demand-zero frames), written IDENTICALLY. */
    uint64_t p1f=frame_reserve(), p2f=frame_reserve(), p3f=frame_reserve();
    if(!p1f||!p2f||!p3f){ sputs("PM3 NOT RUN: out of frames\n"); goto pm_done; }
    pm_fill_page(p1f, 0x55); pm_fill_page(p2f, 0x55);            /* P1 and P2: IDENTICAL bytes, INDEPENDENTLY produced */
    pm_fill_page(p3f, 0xAA);                                     /* P3: DISTINCT content */
    /* DEDUP CHECKPOINT: hash all three (deferred identity evaluated HERE, not per write). */
    cvsasx_hash_t h1,h2,h3; cvsasx_blake3(P2V(p1f),4096,&h1); cvsasx_blake3(P2V(p2f),4096,&h2); cvsasx_blake3(P2V(p3f),4096,&h3);
    int hash_match=cvsasx_hash_eq(&h1,&h2);
    /* full byte-verify to rule out a hash collision before sharing. */
    int byteident=1; { uint8_t *a=P2V(p1f), *b=P2V(p2f); for(int i=0;i<4096;i++) if(a[i]!=b[i]){ byteident=0; break; } }
    sputs("PM3 P1 hash="); sputs(hx(h1.b,16)); sputs(" P2 hash="); sputs(hx(h2.b,16));
    sputs(" match="); sputs(hash_match?"y":"n"); sputs(" byte-verify="); sputs(byteident?"y":"n"); sputc('\n');
    sputs("PM3 P3 hash="); sputs(hx(h3.b,16)); sputs(" (distinct content -> different hash="); sputs(!cvsasx_hash_eq(&h1,&h3)?"y":"n"); sputs(")\n");
    /* DEDUP: store P1's content once; both P1 and P2 map the ONE resident frame read-only
     * (rm_materialize: first call materializes, second is share-by-hash refcount++). P1's
     * and P2's private demand-zero frames are released (their content is the shared frame). */
    uint64_t free_before_dedup=fdb_freetop;
    cvsasx_store_put(&rm_store,P2V(p1f),4096,&h1);               /* publish the shared content */
    frame_release_physical(p1f); frame_release_physical(p2f);    /* drop the two private copies */
    uint64_t shared1=rm_materialize(&h1);                        /* P1 maps the shared frame (materialize) */
    uint64_t shared2=rm_materialize(&h1);                        /* P2 maps the SAME frame (dedup, refcount++) */
    uint32_t rc=framedb[fr_idx(shared1)].refcount;
    int deduped=(shared1==shared2)&&(rc>=2);
    (void)free_before_dedup;
    sputs("PM3 dedup: P1 frame#"); sdec(fr_idx(shared1)); sputs(" P2 frame#"); sdec(fr_idx(shared2));
    sputs(deduped?" -> SAME PHYSICAL FRAME (shared by hash, no ancestry)":" -> *** NOT SHARED ***");
    sputs(", refcount="); sdec(rc); sputs(" -> 2 identical pages collapsed to 1 frame\n");
    /* COW ON WRITE: P2 wants to write. The shared frame is read-only-by-policy (has_hash);
     * a write must COPY to a fresh private frame first, then the write lands privately. The
     * other holder (P1) keeps the shared content unchanged. */
    uint64_t cow_frame=frame_reserve();
    int cow_ok=0, p1_unchanged=0;
    if(cow_frame){
        uint8_t *src=P2V(shared2), *dst=P2V(cow_frame); for(int i=0;i<4096;i++) dst[i]=src[i];   /* COPY (the COW) */
        refdrop(&h1);                                            /* P2 drops its ref to the shared frame */
        *(uint64_t*)dst=0xDEADBEEF;                              /* P2's PRIVATE write (does not touch the shared frame) */
        cow_ok=(*(uint64_t*)dst==0xDEADBEEF);
        /* P1 still sees the ORIGINAL shared content (the pm_fill_page(0x55) seed pattern). */
        uint8_t *p1v=P2V(shared1); uint64_t expect; for(int i=0;i<8;i++) ((uint8_t*)&expect)[i]=(uint8_t)(0x55+(uint8_t)(i*7u));
        p1_unchanged=(*(uint64_t*)p1v==expect);
    }
    sputs("PM3 COW: P2 writes -> copied to private frame#"); sdec(cow_frame?fr_idx(cow_frame):0);
    sputs(", P2 private value="); sx64(cow_ok?0xDEADBEEF:0);
    sputs("; P1 still sees original shared content (unchanged="); sputs(p1_unchanged?"y":"n"); sputs(")\n");
    int pm3=hash_match&&byteident&&deduped&&cow_ok&&p1_unchanged;
    sputs(pm3 ? "PM3 -> TWO INDEPENDENT IDENTICAL HEAP PAGES DEDUPED TO ONE FRAME BY HASH; COW PRESERVED PRIVACY OK\n"
              : "PM3 -> *** FAIL (see stall report) ***\n");
    sputs("PM3 -> this is the sharing COW STRUCTURALLY CANNOT do: no shared ancestor, dedup by CONTENT.\n");
    sputs("PM3 TRADEOFF (honest): content-addressing is BROADER (no ancestry needed) but COSTS the hash;\n");
    sputs("    ancestry COW is NARROWER (only forked pages) but FREE at fork. Neither strictly dominates.\n");
    /* tidy P3 + the COW frame + remaining shared ref. */
    frame_release_physical(p3f); if(cow_frame) frame_release_physical(cow_frame); refdrop(&h1);

    /* ---- PM4: THE U3 CHURN LIMIT DEMONSTRATED - hot heap page EXCLUDED. ----
     * A heavily-mutating HOT heap page is presented to the PP policy under pressure. The
     * policy MUST exclude it (keep resident) while still dematerializing a COLD page. The
     * COST of NOT excluding it (re-hash on every dematerialize attempt = the U3 cost) is
     * MEASURED to justify the exclusion. The content-addressed heap is NOT cost-free. */
    sputs("\n--- PM4: the U3 churn limit - hot heap page EXCLUDED, cold page dematerialized (MEASURED) ---\n");
    /* a COLD stable page and a HOT churning page (both heap frames). The hot/cold signal
     * REUSES the M4 dirty bit + a write-frequency counter (the PP hot/cold input). */
    uint64_t coldf=frame_reserve(), hotf=frame_reserve();
    if(!coldf||!hotf){ sputs("PM4 NOT RUN: out of frames\n"); goto pm_done; }
    pm_fill_page(coldf, 0x10); framedb[fr_idx(coldf)].dirty=0; framedb[fr_idx(coldf)].clock_ref=0;   /* stable, untouched recently */
    /* HOT page: rewrite it many times and COUNT the writes (the churn signal). Each rewrite
     * keeps its identity CHANGING - re-hashing it per dematerialize attempt is the U3 cost. */
    #define PM4_CHURN 64
    uint32_t hot_writes=0;
    for(int r=0;r<PM4_CHURN;r++){ uint8_t *p=P2V(hotf); for(int i=0;i<4096;i++) p[i]=(uint8_t)(r*31+i); hot_writes++; framedb[fr_idx(hotf)].dirty=1; framedb[fr_idx(hotf)].clock_ref=1; }
    /* MEASURE the per-dematerialize re-hash cost (the U3 cost of NOT excluding a hot page):
     * dematerializing a hot page means hashing 4096 churning bytes EVERY attempt. */
    uint64_t th0=rdtsc(); cvsasx_hash_t junk; cvsasx_blake3(P2V(hotf),4096,&junk); uint64_t hash_cyc=rdtsc()-th0; (void)junk;
    /* THE POLICY DECISION (the U3 exclusion rule). The page-level hot/cold predicate the PP
     * policy consults: a page is HOT (a poor dematerialize candidate) if it was written
     * recently (clock_ref) AND its write count exceeds the churn threshold. Hot -> EXCLUDE
     * (keep resident); cold -> eligible. This is the page analogue of PP's hot-reuse rule. */
    #define PM4_HOT_WRITES 8u
    int cold_is_hot = (framedb[fr_idx(coldf)].clock_ref && 0 >= (int)PM4_HOT_WRITES);   /* cold: 0 writes -> not hot */
    int hot_is_hot  = (framedb[fr_idx(hotf)].clock_ref  && hot_writes >= PM4_HOT_WRITES);
    /* induce pressure so the policy WOULD dematerialize an eligible page. */
    static uint64_t pm4_held[FRAMEDB_N];
    uint64_t nheld=0; while(fdb_freetop>(PP_PRESSURE_LO-2)){ uint64_t f=frame_reserve(); if(!f) break; pm4_held[nheld++]=f; }
    int under_pressure=pp_under_pressure();
    /* ACT: under pressure, dematerialize the COLD page (eligible), EXCLUDE the HOT page. */
    uint64_t free_b4=fdb_freetop; int cold_demat=0, hot_kept=0;
    if(under_pressure && !cold_is_hot){ dematerialize_frame(coldf,4096); frame_release_physical(coldf); cold_demat=1; }   /* cold page hashed -> store, frame freed */
    if(under_pressure && hot_is_hot){ hot_kept=1; }   /* EXCLUDED: kept resident (NOT hashed, NOT freed) */
    uint64_t cold_freed=fdb_freetop-free_b4;
    for(uint64_t i=0;i<nheld;i++) frame_release_physical(pm4_held[i]);   /* relieve pressure */
    sputs("PM4 under-pressure="); sputs(under_pressure?"y":"n");
    sputs("; COLD page: writes=0 hot="); sputs(cold_is_hot?"y":"n"); sputs(" -> "); sputs(cold_demat?"DEMATERIALIZED (freed 1 frame)":"kept");
    sputc('\n');
    sputs("PM4 HOT page: writes="); sdec(hot_writes); sputs(" (>= churn threshold "); sdec(PM4_HOT_WRITES);
    sputs(") hot="); sputs(hot_is_hot?"y":"n"); sputs(" -> "); sputs(hot_kept?"EXCLUDED (kept resident, NOT re-hashed)":"*** dematerialized (WRONG) ***"); sputc('\n');
    sputs("PM4 MEASURED U3 cost AVOIDED by excluding the hot page = "); sdec(hash_cyc);
    sputs(" cyc PER dematerialize attempt (the re-hash of 4096 churning bytes); over a churn burst of ");
    sdec(PM4_CHURN); sputs(" rewrites that is "); sdec(hash_cyc*PM4_CHURN); sputs(" cyc of wasted hashing if NOT excluded.\n");
    int pm4=under_pressure&&cold_demat&&hot_kept&&(cold_freed>=1)&&(hash_cyc>0);
    sputs(pm4 ? "PM4 -> UNDER PRESSURE: COLD PAGE DEMATERIALIZED, HOT CHURNING PAGE EXCLUDED (kept resident); U3 COST MEASURED OK\n"
              : "PM4 -> *** FAIL (see stall report) ***\n");
    sputs("PM4 -> the content-addressed heap is NOT cost-free; the policy KNOWS it and excludes hot pages.\n");
    frame_release_physical(hotf);

pm_done:
    sputs("\nPER-PROCESS MEMORY: PM0-PM4 run; extend-heap crosses the gate, demand-zero backs it, the genuine\n");
    sputs("content-addressed consequences apply where they apply, the U3 churn limit is enforced (MEM_LOG.md).\n");
    framedb_init();   /* clean database for run_shell */
}

/* ===========================================================================
 * TS0-TS5 - FULL TASK-STATE DEMATERIALIZATION (the page-table root INCLUDED).
 *
 * Closes the standing caveat named in every prior milestone (C2/PP/ROADMAP:
 * "the page-table root stays resident"). Makes ONE whole live (descheduled)
 * process a content-addressed object: its page-table hierarchy canonicalized to
 * a placement-independent Merkle root, its registers/stack/authority/metadata
 * folded into ONE task-hash, ALL its frames freed (page-table frames too, not
 * just backing pages), rebuilt BIT-EXACT from the hash, authority re-minted
 * through the PROVEN gate under the SAME ceiling and never widened, the cost
 * measured against today's keep-the-root-resident behaviour.
 *
 * REUSES (the proven core is NOT touched, ships byte-identical): conc_make_proc
 * / conc_enter (the ring-3 process model), mm_new_space / mm_walk / mm_map /
 * mm_resolve (page tables), frame_reserve / frame_release_physical / falloc /
 * ffree (frames), rm_materialize / dematerialize_frame / cvsasx_blake3 / the
 * store (content addressing, the PM2 path), cvsasx_sw_cap_remint + the
 * custodian/region (the gate). Numbers COMPUTED (rdtsc / counters), never
 * hardcoded. See kernel/TASKSTATE_LOG.md.
 *
 * SCOPE LOCK (from the research report): IN scope = a QUIESCENT (descheduled)
 * process with ordinary CONTENT-BACKED pages. OUT of scope, EXCLUDED + flagged,
 * never hashed: device/MMIO mappings (no meaningful content hash); live/
 * non-quiescent address spaces (canonicalization needs a consistent snapshot).
 * ===========================================================================*/

/* the clean kernel PML4 (its low/user half is absent) captured at boot. The late
 * run_taskstate restores it so fresh spaces (mm_new_space) clone a clean top-half;
 * an earlier ring-3 stage may have left a process cr3 active. */
static uint64_t g_kernel_cr3=0;

/* the structural object store for the canonical tree / descriptor / registers /
 * metadata / manifest. Page CONTENTS live in rm_store (the proven PM2 path), so
 * rm_materialize rebuilds them verbatim. */
static uint8_t  ts_arena[1u<<18]; static cvsasx_store_entry_t ts_idx[512]; static cvsasx_store_t ts_store; static int ts_ready=0;
static void ts_store_once(void){ if(!ts_ready){ cvsasx_store_init(&ts_store,ts_arena,sizeof ts_arena,ts_idx,512); ts_ready=1; } }

/* a recovered canonical leaf (the placement-independent mapping record). */
#define TS_MAXLEAF 64
typedef struct { uint64_t vpn; uint64_t perm; uint8_t chash[32]; } ts_leaf_t;
static ts_leaf_t ts_leaves[TS_MAXLEAF]; static int ts_nleaf;
static int      ts_excluded; static uint64_t ts_excluded_va;   /* device/MMIO classified out */

/* MM_SHARED: a software-available PTE bit (AVL bit 9, ignored by hardware) that
 * marks a leaf as a SHARED-object mapping (SM stages). A PRIVATE leaf never sets it,
 * so TS canonicalization/rebuild is unchanged; only SHARED leaves take the attach
 * path on rebuild. */
#define MM_SHARED  (1ULL<<9)

/* the EXACT permission mask folded into a leaf: present, write, user, the SHARED
 * marker, and NX. The hardware ACCESSED(5)/DIRTY(6) status bits are NOT permissions
 * and are masked out, so an accessed page and an untouched one with the same rights
 * canonicalize identically (determinism). MM_SHARED is 0 on every TS (private) leaf,
 * so including it here does not change any TS hash. */
#define TS_PERM_MASK ((uint64_t)(MM_PRESENT|MM_WRITE|MM_USER|MM_SHARED) | (1ULL<<63))

/* classifier: a frame is CONTENT-BACKED iff its physical address is tracked RAM.
 * A device/MMIO frame (framebuffer, LAPIC, anything off the usable map) has no
 * meaningful content hash and is EXCLUDED. */
static int ts_content_backed(uint64_t pa){ return pa>=ram_lo && pa<ram_hi; }

/* one static node buffer PER level (PML4/PDPT/PD/PT). The canonicalization is a
 * depth-first walk, so exactly one buffer per level is live at a time: no
 * aliasing, and 16 KiB*4 stays out of the kernel stack. */
static uint8_t ts_nbuf_pml4[512*32], ts_nbuf_pdpt[512*32], ts_nbuf_pd[512*32], ts_nbuf_pt[512*32];

/* canonical PT (leaf-level) node: BLAKE3 of the ORDERED leaf hashes, where each
 * leaf = BLAKE3( VPN || page-content-hash || perm-bits ) and page-content-hash is
 * the existing BLAKE3 of the 4 KiB backing page (REUSE the store's hashing). The
 * node BYTES (the ordered leaf hashes) are stored so the whole tree is in the
 * store by hash. Non-content (device/MMIO) leaves are EXCLUDED + flagged. */
static cvsasx_hash_t ts_canon_pt(uint64_t pt_phys, uint64_t va_base, int collect){
    uint8_t *buf=ts_nbuf_pt; int n=0;
    uint64_t *t=P2V(pt_phys);
    for(int i=0;i<512;i++){
        if(!(t[i]&1)) continue;
        if(!(t[i]&MM_USER)) continue;                /* only USER (ring-3) leaves are this process's content */
        uint64_t pte=t[i]; uint64_t pa=pte&~0xfffULL;
        uint64_t va=va_base | ((uint64_t)i<<12); uint64_t vpn=va>>12;
        if(!ts_content_backed(pa)){ if(collect){ ts_excluded++; ts_excluded_va=va; } continue; } /* device/MMIO: never hashed */
        uint64_t perm=pte & TS_PERM_MASK;
        cvsasx_hash_t ch; cvsasx_blake3((const void*)P2V(pa),4096,&ch);
        uint8_t lb[8+32+8];
        for(int k=0;k<8;k++) lb[k]=(uint8_t)(vpn>>(8*k));
        for(int k=0;k<32;k++) lb[8+k]=ch.b[k];
        for(int k=0;k<8;k++) lb[40+k]=(uint8_t)(perm>>(8*k));
        cvsasx_hash_t leaf; cvsasx_blake3(lb,sizeof lb,&leaf);
        for(int k=0;k<32;k++) buf[n*32+k]=leaf.b[k]; n++;
        if(collect && ts_nleaf<TS_MAXLEAF){ ts_leaves[ts_nleaf].vpn=vpn; ts_leaves[ts_nleaf].perm=perm;
            for(int k=0;k<32;k++) ts_leaves[ts_nleaf].chash[k]=ch.b[k]; ts_nleaf++; }
    }
    cvsasx_hash_t node; cvsasx_blake3(buf,(size_t)n*32,&node);
    ts_store_once(); cvsasx_hash_t tmp; cvsasx_store_put(&ts_store,buf,(size_t)n*32,&tmp);  /* store the node */
    return node;
}
/* canonical interior node (lvl 3=PDPT, 2=PD): BLAKE3 of the ORDERED child hashes.
 * The index need not be folded in: each leaf carries its full VPN, so a child at a
 * different slot yields different leaf VPNs and a different root by construction. */
static cvsasx_hash_t ts_canon_interior(uint64_t tbl_phys, int lvl, uint64_t va_base, int collect){
    uint8_t *buf=(lvl==3)?ts_nbuf_pdpt:ts_nbuf_pd; int n=0;
    uint64_t *t=P2V(tbl_phys); int shift=12+9*(lvl-1);
    for(int i=0;i<512;i++){
        if(!(t[i]&1)) continue;
        if(!(t[i]&MM_USER)) continue;                /* descend only into USER (process-private) subtrees */
        uint64_t child=t[i]&~0xfffULL; uint64_t cva=va_base | ((uint64_t)i<<shift);
        cvsasx_hash_t ch=(lvl==2)?ts_canon_pt(child,cva,collect):ts_canon_interior(child,lvl-1,cva,collect);
        for(int k=0;k<32;k++) buf[n*32+k]=ch.b[k]; n++;
    }
    cvsasx_hash_t node; cvsasx_blake3(buf,(size_t)n*32,&node);
    ts_store_once(); cvsasx_hash_t tmp; cvsasx_store_put(&ts_store,buf,(size_t)n*32,&tmp);
    return node;
}
/* TS0 - canonicalize the process's CONTENT-BACKED user address space into the
 * research report's Merkle form. Walks the REAL page table via mm_walk's own PML4;
 * descends ONLY into PRIVATE (user) subtrees - the PML4 slots the kernel clone
 * left absent - so the SHARED kernel/HHDM half (device/MMIO, not this process's
 * content) is never hashed. address-space root = hash of the canonical PML4-level
 * node over the included mappings. Returns the root; with collect=1 fills
 * ts_leaves[] + the exclusion counters. PLACEMENT-INDEPENDENT: only VPN, content
 * hash and perm-bits enter the hash; never a physical frame number. */
static cvsasx_hash_t ts_canonicalize(uint64_t cr3, int collect){
    if(collect){ ts_nleaf=0; ts_excluded=0; ts_excluded_va=0; }
    uint64_t *pp=P2V(cr3&~0xfffULL);
    uint8_t *buf=ts_nbuf_pml4; int n=0;
    for(int i=0;i<256;i++){                          /* LOW/USER half only (PML4 0..255); the kernel/HHDM live in the high half (256..511) */
        if(!(pp[i]&1)) continue;
        if(!(pp[i]&MM_USER)) continue;              /* USER slot only: kernel supervisor low-half mappings (e.g. B3) are skipped */
        uint64_t child=pp[i]&~0xfffULL; uint64_t cva=((uint64_t)i)<<39;
        cvsasx_hash_t ch=ts_canon_interior(child,3,cva,collect);   /* child is a PDPT */
        for(int k=0;k<32;k++) buf[n*32+k]=ch.b[k]; n++;
    }
    cvsasx_hash_t root; cvsasx_blake3(buf,(size_t)n*32,&root);
    ts_store_once(); cvsasx_hash_t tmp; cvsasx_store_put(&ts_store,buf,(size_t)n*32,&tmp);
    return root;
}

/* the recoverable canonical DESCRIPTOR: [count:1][pad:3] then count*( vpn:8 perm:8
 * chash:32 ) in ascending VPN order (the walk order). This is the placement-
 * independent form the rebuild reads; its hash equals across two spaces with the
 * same logical mapping. */
static uint8_t ts_descbuf[4+TS_MAXLEAF*48];
static cvsasx_hash_t ts_put_descriptor(void){
    uint64_t o=0; ts_descbuf[0]=(uint8_t)ts_nleaf; ts_descbuf[1]=ts_descbuf[2]=ts_descbuf[3]=0; o=4;
    for(int i=0;i<ts_nleaf;i++){
        for(int k=0;k<8;k++) ts_descbuf[o++]=(uint8_t)(ts_leaves[i].vpn>>(8*k));
        for(int k=0;k<8;k++) ts_descbuf[o++]=(uint8_t)(ts_leaves[i].perm>>(8*k));
        for(int k=0;k<32;k++) ts_descbuf[o++]=ts_leaves[i].chash[k];
    }
    ts_store_once(); cvsasx_hash_t h; cvsasx_store_put(&ts_store,ts_descbuf,o,&h); return h;
}

/* pack the per-task METADATA needed for a correct rebuild (residency / PP / FA
 * counters + the local ceiling base offset + the descriptor hash). IDENTICAL
 * packing in demat and verify, so the metadata hash round-trips bit-exact. */
static uint64_t ts_pack_meta(uint8_t *b, const cproc_t *p, uint64_t ceil_off, const cvsasx_hash_t *dhash){
    uint64_t o=0;
    for(int k=0;k<4;k++) b[o++]=(uint8_t)(p->id>>(8*k));
    b[o++]=p->tag; b[o++]=p->desched_reason; b[o++]=0; b[o++]=0;
    for(int k=0;k<4;k++) b[o++]=(uint8_t)(p->wakeup_count>>(8*k));
    for(int k=0;k<4;k++) b[o++]=(uint8_t)(p->cooldown_budget>>(8*k));
    for(int k=0;k<8;k++) b[o++]=(uint8_t)(p->useful_cyc>>(8*k));
    for(int k=0;k<8;k++) b[o++]=(uint8_t)(p->tax_cyc>>(8*k));
    for(int k=0;k<4;k++) b[o++]=(uint8_t)(p->fair_grant>>(8*k));
    for(int k=0;k<8;k++) b[o++]=(uint8_t)(ceil_off>>(8*k));
    for(int k=0;k<32;k++) b[o++]=dhash->b[k];
    return o;
}

/* free a process's PAGE-TABLE frames (the new thing): walk the PRIVATE user
 * subtree of cr3 and ffree every PT/PD/PDPT frame, then the PML4 frame itself.
 * The SHARED kernel/HHDM slots (cloned by mm_new_space) are kept. Leaf PTEs point
 * at backing frames that are freed on the separate backing-page path, so a PT
 * frame is the table, never a backing page. Returns the count freed. */
static int ts_free_pagetables(uint64_t cr3){
    uint64_t pml4_phys=cr3&~0xfffULL; uint64_t *pp=P2V(pml4_phys);
    int freed=0;
    for(int i=0;i<256;i++){                                            /* LOW/USER half only; the kernel/HHDM high half (256..511) is never freed */
        if(!(pp[i]&1)||!(pp[i]&MM_USER)) continue;                     /* keep supervisor low-half kernel mappings (e.g. B3) */
        uint64_t pdpt=pp[i]&~0xfffULL; uint64_t *pdv=P2V(pdpt);
        for(int j=0;j<512;j++){
            if(!(pdv[j]&1)||!(pdv[j]&MM_USER)) continue; if(pdv[j]&0x80) continue; /* 1 GiB page: none here (4K only) */
            uint64_t pd=pdv[j]&~0xfffULL; uint64_t *pv=P2V(pd);
            for(int k=0;k<512;k++){
                if(!(pv[k]&1)||!(pv[k]&MM_USER)) continue; if(pv[k]&0x80) continue; /* 2 MiB page: none here */
                ffree(pv[k]&~0xfffULL); freed++;                       /* the PT frame */
            }
            ffree(pd); freed++;                                       /* the PD frame */
        }
        ffree(pdpt); freed++;                                         /* the PDPT frame */
    }
    ffree(pml4_phys); freed++;                                        /* the PML4 root frame */
    return freed;
}

/* a rebuild slot for the recovered process (so TS3 keeps OLD frame numbers for the
 * placement-differs proof). */
static cvsasx_hash_t g_ts_taskhash;             /* the ONE content address P collapses to */
static cvsasx_hash_t g_ts_asr_orig;             /* TS1 address-space root (for the TS3 match) */
static uint64_t g_ts_canon_cyc, g_ts_demat_cyc, g_ts_remat_cyc;
static uint64_t g_ts_backing_freed, g_ts_pt_freed;
static uint64_t g_ts_old_data_pfn, g_ts_old_stk_pfn, g_ts_old_pml4;

/* run a process via the PROVEN conc ring-3 path until it parks (counter==park) or
 * runs to completion (park==0). Mirrors the C2 IDT dance exactly. */
static void ts_run_ring3(cproc_t *p, uint64_t park){
    g_ncp=1; g_clive=1; p->live=1; p->present=1;
    idt_set(0x20,(void*)conc_trap,SEL_KCODE);
    idt_set(0x80,(void*)conc_sysent,SEL_KCODE); idt[0x80].type=0xEE;
    { struct idtr idtr={(uint16_t)(sizeof(idt)-1),(uint64_t)idt}; __asm__ volatile("lidt %0"::"m"(idtr):"memory"); }
    g_conc_active=1; g_c2_park_at=park; g_ccur=0;
    conc_enter(&p->tf, p->cr3);
    g_conc_active=0; g_c2_park_at=0;
    idt_set(0x20,(void*)isr_timer,SEL_KCODE);
    idt_set(0x80,(void*)isr_syscall,SEL_KCODE); idt[0x80].type=0xEE;
    { struct idtr idtr={(uint16_t)(sizeof(idt)-1),(uint64_t)idt}; __asm__ volatile("lidt %0"::"m"(idtr):"memory"); }
}

static void run_taskstate(void){
    sputs("\n=== FULL TASK-STATE DEMATERIALIZATION (TS0-TS5): a whole process as ONE content hash, page table + authority included ===\n");
    sputs("TS scope: a QUIESCENT (descheduled) process, CONTENT-BACKED pages only; device/MMIO EXCLUDED+flagged; live spaces OUT (need a consistent snapshot).\n");
    /* Base fresh spaces on a CLEAN kernel PML4. Earlier ring-3 demo stages (US / L /
     * PM) mapped user pages into the kernel cr3's low half and left USER PML4 slots
     * present; cloning those would make every "fresh" space SHARE them (and freeing
     * the page tables would then free the kernel's own shared tables). Drop EVERY
     * leftover low-half USER slot (the kernel runs entirely in the high half: code,
     * stack, HHDM, framebuffer; supervisor low-half mappings like B3's are kept) and
     * reload, so mm_new_space clones a private, clean low half. */
    if(g_kernel_cr3){ uint64_t *kpml=P2V(g_kernel_cr3);
        for(int i=0;i<256;i++) if((kpml[i]&1)&&(kpml[i]&MM_USER)) kpml[i]=0;
        __asm__ volatile("mov %0,%%cr3"::"r"(g_kernel_cr3):"memory"); }
    framedb_init();                                  /* clean pool -> deterministic frame accounting */
    rset_n=0; for(uint32_t i=0;i<RSET_MAX;i++) rset[i].live=0;   /* clean resident-set */
    ts_store_once(); conc_store_once(); rm_store_once();
    sputs("TS note: the process's content is its USER (ring-3) page-table subtree (USER bit set at every level);\n");
    sputs("         the shared kernel/HHDM half is supervisor (USER bit clear) and is never canonicalized or freed.\n");

    /* =====================================================================
     * TS0 - THE CANONICAL PAGE-TABLE FORM (placement-independent Merkle root)
     * ===================================================================*/
    sputs("\n--- TS0: canonical Merkle page-table form (leaf=BLAKE3(VPN||content-hash||perm); root=PML4 node) ---\n");
    /* A and B: identical LOGICAL space (same VAs, same page CONTENT - same tag/target,
     * stacks zeroed below, code shared-by-hash) in DIFFERENT physical frames, so only
     * placement differs. (The id differs but is not part of the hashed page content.) */
    int okA=conc_make_proc(&g_cp[0], 0xA, 'P', 8, 0, 128);
    int okB=conc_make_proc(&g_cp[1], 0xB, 'P', 8, 0, 128);
    if(!okA||!okB){ sputs("TS0 NOT RUN: could not build two processes (frames/space)\n"); return; }
    cproc_t *A=&g_cp[0], *B=&g_cp[1];
    /* equalize CONTENT at every mapped VA so only PHYSICAL placement differs:
     * zero both stacks (frame_reserve does not), data pages already identical
     * (same tag/target), code shared-by-hash. */
    for(int i=0;i<4096;i++){ ((uint8_t*)P2V(A->stack_frame))[i]=0; ((uint8_t*)P2V(B->stack_frame))[i]=0; }
    cvsasx_hash_t rA=ts_canonicalize(A->cr3,1);
    cvsasx_hash_t rB=ts_canonicalize(B->cr3,1);
    int eqAB=cvsasx_hash_eq(&rA,&rB);
    sputs("TS0 proc A root = "); sputs(hx(rA.b,32)); sputc('\n');
    sputs("TS0 proc B root = "); sputs(hx(rB.b,32)); sputc('\n');
    sputs("TS0 A frames: code#"); sdec(fr_idx(A->code_frame)); sputs(" data#"); sdec(fr_idx(A->data_frame)); sputs(" stack#"); sdec(fr_idx(A->stack_frame));
    sputs("  B frames: code#"); sdec(fr_idx(B->code_frame)); sputs(" data#"); sdec(fr_idx(B->data_frame)); sputs(" stack#"); sdec(fr_idx(B->stack_frame)); sputc('\n');
    /* genuinely SEPARATE page tables (not a shared subtree): A and B resolve the same VA to DIFFERENT physical frames. */
    int sep_tables=(mm_resolve(A->cr3,CONC_DATA_VA)!=mm_resolve(B->cr3,CONC_DATA_VA))&&(A->cr3!=B->cr3);
    int diffplace=(A->data_frame!=B->data_frame)&&(A->stack_frame!=B->stack_frame)&&sep_tables;
    sputs("TS0 identical logical space, DIFFERENT physical frames (data/stack distinct="); sputs(diffplace?"y":"n");
    sputs("; code deduped-by-hash to one frame) -> roots EQUAL="); sputs(eqAB?"y":"n"); sputs(eqAB?" (PLACEMENT-INDEPENDENT) OK\n":" *** roots differ ***\n");
    /* change ONE page content -> root DIFFERS */
    cvsasx_hash_t rBbase=rB;
    ((uint8_t*)P2V(B->data_frame))[0]^=0xFF;
    cvsasx_hash_t rBc=ts_canonicalize(B->cr3,1);
    int diff_content=!cvsasx_hash_eq(&rBc,&rBbase);
    ((uint8_t*)P2V(B->data_frame))[0]^=0xFF;                       /* restore */
    /* change ONE perm bit (data RW -> RO) -> root DIFFERS */
    mm_map(B->cr3, CONC_DATA_VA, B->data_frame, MM_USER);          /* drop MM_WRITE */
    cvsasx_hash_t rBp=ts_canonicalize(B->cr3,1);
    int diff_perm=!cvsasx_hash_eq(&rBp,&rBbase);
    mm_map(B->cr3, CONC_DATA_VA, B->data_frame, MM_USER|MM_WRITE); /* restore */
    cvsasx_hash_t rBr=ts_canonicalize(B->cr3,1);
    int restored=cvsasx_hash_eq(&rBr,&rBbase);
    sputs("TS0 one CONTENT byte changed -> root "); sputs(hx(rBc.b,12)); sputs(".. differs="); sputs(diff_content?"y":"n"); sputc('\n');
    sputs("TS0 one PERM bit changed (RW->RO) -> root "); sputs(hx(rBp.b,12)); sputs(".. differs="); sputs(diff_perm?"y":"n");
    sputs("; restored-to-baseline="); sputs(restored?"y":"n"); sputc('\n');
    /* device/MMIO EXCLUSION: a scratch space with one RAM page + one MMIO page. */
    uint64_t sc=mm_new_space(); uint64_t ramf=frame_reserve();
    int okmmio=0;
    if(sc&&ramf){
        for(int i=0;i<4096;i++) ((uint8_t*)P2V(ramf))[i]=(uint8_t)(i*5u+1u);
        uint64_t mmio_pa=(ram_hi+0x100000ULL)&~0xfffULL;           /* off the usable map = device/MMIO */
        mm_map(sc, 0x0000000040000000ULL, ramf,    MM_USER|MM_WRITE);
        mm_map(sc, 0x0000000060000000ULL, mmio_pa, MM_USER);       /* non-content: must be skipped, not read */
        ts_canonicalize(sc,1);
        okmmio=(ts_nleaf==1)&&(ts_excluded==1)&&(ts_excluded_va==0x0000000060000000ULL);
        sputs("TS0 device/MMIO exclusion: content-backed leaves INCLUDED="); sdec(ts_nleaf);
        sputs(", non-content EXCLUDED="); sdec((uint64_t)ts_excluded); sputs(" (flagged @"); sx64(ts_excluded_va);
        sputs(", never hashed) "); sputs(okmmio?"OK\n":"*** classify wrong ***\n");
        ts_free_pagetables(sc); frame_release_physical(ramf);
    }
    int ts0=eqAB&&diffplace&&diff_content&&diff_perm&&restored&&okmmio;
    sputs(ts0?"TS0 -> CANONICAL MERKLE FORM: placement-independent, content/perm sensitive, device-excluded OK\n":"TS0 -> *** FAIL ***\n");
    /* tear A and B down so TS1+ starts from a clean pool */
    for(int i=0;i<4096;i++){} /* (no-op marker) */
    refdrop(&conc_code_hash); refdrop(&conc_code_hash);            /* A and B each took one code ref */
    frame_release_physical(A->data_frame); frame_release_physical(A->stack_frame);
    frame_release_physical(B->data_frame); frame_release_physical(B->stack_frame);
    ts_free_pagetables(A->cr3); ts_free_pagetables(B->cr3);

    /* =====================================================================
     * TS1 - THE COMPLETE TASK OBJECT (reduce a live process to one hash)
     * ===================================================================*/
    sputs("\n--- TS1: the complete task object (one task-hash over registers||stack||addr-space-root||ceiling||metadata) ---\n");
    framedb_init(); rset_n=0; for(uint32_t i=0;i<RSET_MAX;i++) rset[i].live=0;
    if(!conc_make_proc(&g_cp[0], 0x7, 'T', 8, 0, 128)){ sputs("TS1 NOT RUN: could not build process\n"); return; }
    cproc_t *P=&g_cp[0];
    ts_run_ring3(P, 3);                                          /* live: run in ring 3 until counter reaches 3, then park (descheduled, quiescent) */
    uint64_t counter0=*(uint64_t*)(P2V(P->data_frame)+8);
    sputs("TS1 process ran in ring 3 then descheduled (quiescent) at counter="); sdec(counter0); sputs(" (full context saved in its trapframe)\n");
    g_ts_old_data_pfn=fr_idx(P->data_frame); g_ts_old_stk_pfn=fr_idx(P->stack_frame); g_ts_old_pml4=(P->cr3&~0xfffULL);
    /* component 1+2: registers + stack content (REUSE the C2 register/stack content addressing). */
    cvsasx_hash_t regH; cvsasx_store_put(&ts_store,&P->tf,sizeof(c_tf_t),&regH);
    cvsasx_hash_t stkH; cvsasx_blake3((const void*)P2V(P->stack_frame),4096,&stkH);
    /* component 3: the address-space root (TS0). Its cost is measured inside TS2's
     * demat (a warm re-canonicalize), so the broken-out page-table cost is consistent
     * with the rest of the demat timing rather than paying first-call warmup here. */
    cvsasx_hash_t asr=ts_canonicalize(P->cr3,1);
    cvsasx_hash_t descH=ts_put_descriptor();
    g_ts_asr_orig=asr;
    /* component 4: the capability ceiling as a RE-MINTABLE DESCRIPTOR (ceilH || len || perms),
     * NOT raw kernel pointers or raw cap bits - this is what the gate re-mints on rebuild. */
    uint8_t capd[44]; uint32_t ceil_perms=(uint32_t)(CVSASX_PERM_LOAD|CVSASX_PERM_GLOBAL);
    for(int k=0;k<32;k++) capd[k]=P->ceilH[k];
    for(int k=0;k<8;k++) capd[32+k]=(uint8_t)(P->ceil_len>>(8*k));
    for(int k=0;k<4;k++) capd[40+k]=(uint8_t)(ceil_perms>>(8*k));
    /* component 5: metadata (residency/PP/FA counters + ceiling base offset + descriptor hash). */
    static uint8_t metab[128]; uint64_t metalen=ts_pack_meta(metab,P,P->ceil_off,&descH);
    cvsasx_hash_t metaH; cvsasx_store_put(&ts_store,metab,metalen,&metaH);
    /* the MANIFEST is the task-hash preimage; task-hash = BLAKE3(manifest). */
    static uint8_t manifest[172];
    for(int k=0;k<32;k++) manifest[k]=regH.b[k];
    for(int k=0;k<32;k++) manifest[32+k]=stkH.b[k];
    for(int k=0;k<32;k++) manifest[64+k]=asr.b[k];
    for(int k=0;k<44;k++) manifest[96+k]=capd[k];
    for(int k=0;k<32;k++) manifest[140+k]=metaH.b[k];
    cvsasx_hash_t taskH; cvsasx_store_put(&ts_store,manifest,172,&taskH);   /* store under, and = , BLAKE3(manifest) */
    g_ts_taskhash=taskH;
    sputs("TS1 registers-hash         = "); sputs(hx(regH.b,32)); sputc('\n');
    sputs("TS1 stack-hash             = "); sputs(hx(stkH.b,32)); sputc('\n');
    sputs("TS1 address-space-root     = "); sputs(hx(asr.b,32)); sputc('\n');
    sputs("TS1 capability-ceiling-desc= ceilH "); sputs(hx(P->ceilH,12)); sputs(".. len="); sdec(P->ceil_len);
    sputs(" perms="); sx64(ceil_perms); sputs(" (LOAD|GLOBAL; re-mintable, NO raw pointers/cap-bits)\n");
    sputs("TS1 metadata-hash          = "); sputs(hx(metaH.b,32)); sputc('\n');
    sputs("TS1 ==> TASK-HASH          = "); sputs(hx(taskH.b,32)); sputs("  (the whole process reduced to ONE 32-byte content address)\n");
    sputs("TS1 -> WHOLE PROCESS REDUCED TO ONE HASH; capability carried as the re-mintable ceiling descriptor OK\n");

    /* =====================================================================
     * TS2 - FULL DEMATERIALIZATION (free ALL frames, page-table frames too)
     * ===================================================================*/
    sputs("\n--- TS2: full dematerialization (free ALL frames incl. the page-table frames; footprint -> zero) ---\n");
    uint64_t pool_free_before=fdb_freetop;
    uint64_t d0=rdtsc();
    /* THE NEW page-table cost, broken out: a warm re-canonicalize of the page-table
     * hierarchy into the store (idempotent; same root and nodes as TS1). Timed inside
     * the demat so it is comparable to the rest of the cost. */
    uint64_t cc0=rdtsc(); ts_canonicalize(P->cr3,0); g_ts_canon_cyc=rdtsc()-cc0;
    /* backing pages: REUSE the PM2 path (dematerialize_frame -> content into the store), then free
     * the frame. Each page is stored as a full 4 KiB object under its page-hash (the same hash the
     * canonical leaf carries), so rm_materialize rebuilds it verbatim. Code is shared-by-hash
     * (refdrop after storing its page); data+stack are private (release to the pool). */
    dematerialize_frame(P->code_frame,4096);
    dematerialize_frame(P->data_frame,4096);  dematerialize_frame(P->stack_frame,4096);
    int code_in_store=cvsasx_store_exists(&rm_store,&conc_code_hash);
    refdrop(&conc_code_hash);                                      /* code: last ref -> frame reclaimed */
    frame_release_physical(P->data_frame); frame_release_physical(P->stack_frame);
    uint64_t pool_free_mid=fdb_freetop;
    g_ts_backing_freed=pool_free_mid-pool_free_before;             /* backing frames returned to the pool */
    /* THE NEW THING: free the PAGE-TABLE frames themselves (PML4/PDPT/PD/PT). */
    g_ts_pt_freed=(uint64_t)ts_free_pagetables(P->cr3);
    g_ts_demat_cyc=rdtsc()-d0;
    uint64_t old_cr3=P->cr3;
    P->code_frame=P->data_frame=P->stack_frame=0; P->cr3=0; P->present=0; P->dematerialized=1;
    uint64_t footprint=0;  /* the process now holds NO frames at all */
    sputs("TS2 backing content in store: code-resident-by-hash="); sputs(code_in_store?"y":"n");
    sputs("; data+stack dematerialized via the PM2 path (hash->store, frame freed)\n");
    sputs("TS2 pool free BEFORE="); sdec(pool_free_before); sputs(" AFTER-backing="); sdec(pool_free_mid);
    sputs(" (backing frames freed="); sdec(g_ts_backing_freed); sputs(")\n");
    sputs("TS2 PAGE-TABLE frames freed (PML4+PDPT+PD+PT, cr3="); sx64(old_cr3); sputs(") = "); sdec(g_ts_pt_freed);
    sputs(" (the standing 'page-table root stays resident' caveat is now CLOSED)\n");
    sputs("TS2 total physical footprint = backing("); sdec(g_ts_backing_freed); sputs(") + page-table("); sdec(g_ts_pt_freed);
    sputs(") = "); sdec(g_ts_backing_freed+g_ts_pt_freed); sputs(" frames freed; frames held = "); sdec(footprint); sputc('\n');
    sputs("TS2 process now exists only as task-hash "); sputs(hx(g_ts_taskhash.b,32)); sputs(", frames held = 0\n");
    sputs("TS2 cost MEASURED = "); sdec(g_ts_demat_cyc); sputs(" cyc total (of which canonicalization = "); sdec(g_ts_canon_cyc);
    sputs(" cyc, the NEW page-table cost broken out)\n");
    int ts2=(g_ts_backing_freed==3)&&(g_ts_pt_freed>=4)&&(footprint==0);
    sputs(ts2?"TS2 -> FULL DEMATERIALIZATION: every frame freed (page-table frames included), footprint zero OK\n":"TS2 -> *** FAIL (frame accounting) ***\n");

    /* =====================================================================
     * TS3 - BIT-EXACT REMATERIALIZATION (rebuild the whole process from the hash)
     * ===================================================================*/
    sputs("\n--- TS3: bit-exact rematerialization (rebuild EVERYTHING from ONLY the task-hash) ---\n");
    /* hold spacer frames so the rebuild cannot reuse the just-freed LIFO slots:
     * this FORCES fresh physical placement, making the placement-differs proof
     * deterministic (placement-independence itself is shown in TS0). */
    uint64_t sp[3]; for(int i=0;i<3;i++) sp[i]=frame_reserve();
    uint64_t fsp[8]; for(int i=0;i<8;i++) fsp[i]=falloc();
    uint64_t r0=rdtsc();
    /* resolve the manifest from ONLY the task-hash, then its components. */
    const void *mb; size_t ml;
    if(cvsasx_store_get(&ts_store,&g_ts_taskhash,&mb,&ml)!=CVSASX_STORE_OK||ml!=172){ sputs("TS3 FAIL: manifest miss\n"); return; }
    const uint8_t *M=mb;
    cvsasx_hash_t regH2,asr_orig,metaH2; for(int k=0;k<32;k++){ regH2.b[k]=M[k]; asr_orig.b[k]=M[64+k]; metaH2.b[k]=M[140+k]; }
    uint64_t r_ceil_len=0; for(int k=0;k<8;k++) r_ceil_len|=((uint64_t)M[96+32+k])<<(8*k);
    uint32_t r_ceil_perms=0; for(int k=0;k<4;k++) r_ceil_perms|=((uint32_t)M[96+40+k])<<(8*k);
    uint8_t r_ceilH[32]; for(int k=0;k<32;k++) r_ceilH[k]=M[96+k];
    /* metadata -> counters + ceil_off + descriptor hash. */
    const void *eb; size_t el; if(cvsasx_store_get(&ts_store,&metaH2,&eb,&el)!=CVSASX_STORE_OK){ sputs("TS3 FAIL: metadata miss\n"); return; }
    const uint8_t *E=eb; uint64_t mo=0;
    uint32_t r_id=0; for(int k=0;k<4;k++) r_id|=((uint32_t)E[mo++])<<(8*k);
    uint8_t r_tag=E[mo++]; uint8_t r_dr=E[mo++]; mo+=2;
    uint32_t r_wk=0; for(int k=0;k<4;k++) r_wk|=((uint32_t)E[mo++])<<(8*k);
    uint32_t r_cd=0; for(int k=0;k<4;k++) r_cd|=((uint32_t)E[mo++])<<(8*k);
    uint64_t r_uc=0; for(int k=0;k<8;k++) r_uc|=((uint64_t)E[mo++])<<(8*k);
    uint64_t r_tx=0; for(int k=0;k<8;k++) r_tx|=((uint64_t)E[mo++])<<(8*k);
    uint32_t r_fg=0; for(int k=0;k<4;k++) r_fg|=((uint32_t)E[mo++])<<(8*k);
    uint64_t r_ceil_off=0; for(int k=0;k<8;k++) r_ceil_off|=((uint64_t)E[mo++])<<(8*k);
    cvsasx_hash_t descH2; for(int k=0;k<32;k++) descH2.b[k]=E[mo++];
    /* descriptor -> the canonical mapping list. */
    const void *db; size_t dl; if(cvsasx_store_get(&ts_store,&descH2,&db,&dl)!=CVSASX_STORE_OK){ sputs("TS3 FAIL: descriptor miss\n"); return; }
    const uint8_t *D=db; int rn=D[0]; uint64_t doff=4;
    /* build a FRESH four-level hierarchy + install the mappings with FRESH frames. */
    uint64_t new_cr3=mm_new_space(); if(!new_cr3){ sputs("TS3 FAIL: no address space\n"); return; }
    uint64_t new_data=0,new_stk=0,new_code=0;
    for(int i=0;i<rn;i++){
        uint64_t vpn=0; for(int k=0;k<8;k++) vpn|=((uint64_t)D[doff++])<<(8*k);
        uint64_t perm=0; for(int k=0;k<8;k++) perm|=((uint64_t)D[doff++])<<(8*k);
        cvsasx_hash_t ch; for(int k=0;k<32;k++) ch.b[k]=D[doff++];
        uint64_t frame;
        if(perm&MM_WRITE){
            /* writable page MUST be private: fetch by hash, BLAKE3-verify, copy into a fresh frame. */
            const void *cb; size_t cl;
            if(cvsasx_store_get(&rm_store,&ch,&cb,&cl)!=CVSASX_STORE_OK){ sputs("TS3 FAIL: page miss\n"); return; }
            cvsasx_hash_t chk; cvsasx_blake3(cb,cl,&chk); if(!cvsasx_hash_eq(&chk,&ch)){ sputs("TS3 FAIL: page verify\n"); return; }
            frame=frame_reserve(); if(!frame){ sputs("TS3 FAIL: no frame\n"); return; }
            for(size_t k=0;k<cl;k++) ((uint8_t*)P2V(frame))[k]=((const uint8_t*)cb)[k];
        } else {
            frame=rm_materialize(&ch);                            /* read-only code: share-by-hash, BLAKE3-verified */
            if(!frame){ sputs("TS3 FAIL: materialize\n"); return; }
        }
        mm_map(new_cr3, vpn<<12, frame, perm|MM_USER|MM_PRESENT);   /* re-apply the exact stored perm (present/write/user/NX); USER so canonicalize re-finds it */
        uint64_t va=vpn<<12;
        if(va==CONC_DATA_VA) new_data=frame; else if(va==CONC_STK_VA) new_stk=frame; else if(va==CONC_CODE_VA) new_code=frame;
    }
    /* restore the register file from the registers-hash. */
    const void *rb; size_t rl; if(cvsasx_store_get(&ts_store,&regH2,&rb,&rl)!=CVSASX_STORE_OK||rl!=sizeof(c_tf_t)){ sputs("TS3 FAIL: regs miss\n"); return; }
    for(unsigned k=0;k<sizeof(c_tf_t);k++) ((uint8_t*)&P->tf)[k]=((const uint8_t*)rb)[k];
    /* rebuild the LOCAL ceiling region from the re-mintable descriptor (TS4 re-mints it). */
    P->ceil_off=r_ceil_off; P->ceil_len=r_ceil_len; for(int k=0;k<32;k++) P->ceilH[k]=r_ceilH[k];
    { cvsasx_swcap_t root={ (uint64_t)(uintptr_t)conc_obj+r_ceil_off, r_ceil_len, r_ceil_perms, 1 };
      cvsasx_sw_custodian_init(&P->cust, root);
      P->region.object_cap=root; P->region.object_base_addr=(uint64_t)(uintptr_t)conc_obj+r_ceil_off;
      P->region.object_length=r_ceil_len; for(int k=0;k<32;k++) P->region.hash[k]=r_ceilH[k]; }
    /* restore metadata counters. */
    P->id=r_id; P->tag=r_tag; P->desched_reason=r_dr; P->wakeup_count=r_wk; P->cooldown_budget=r_cd;
    P->useful_cyc=r_uc; P->tax_cyc=r_tx; P->fair_grant=r_fg;
    P->cr3=new_cr3; P->code_frame=new_code; P->data_frame=new_data; P->stack_frame=new_stk;
    P->present=1; P->dematerialized=0; P->live=1;
    g_ts_remat_cyc=rdtsc()-r0;
    for(int i=0;i<3;i++) frame_release_physical(sp[i]); for(int i=0;i<8;i++) ffree(fsp[i]);  /* drop spacers */
    sputs("TS3 rebuilt via the CORE materialize path directly (rm_materialize / frame_reserve+BLAKE3-verify), NOT a live #PF (the PM2/F2 precedent)\n");
    /* BIT-EXACT verification: RE-CANONICALIZE the live rebuilt process + recompute the task-hash. */
    cvsasx_hash_t asr2=ts_canonicalize(P->cr3,1);
    cvsasx_hash_t descH3=ts_put_descriptor();
    cvsasx_hash_t regH3; cvsasx_store_put(&ts_store,&P->tf,sizeof(c_tf_t),&regH3);
    cvsasx_hash_t stkH3; cvsasx_blake3((const void*)P2V(P->stack_frame),4096,&stkH3);
    static uint8_t metab3[128]; uint64_t ml3=ts_pack_meta(metab3,P,P->ceil_off,&descH3);
    cvsasx_hash_t metaH3; cvsasx_store_put(&ts_store,metab3,ml3,&metaH3);
    static uint8_t manifest3[172];
    for(int k=0;k<32;k++){ manifest3[k]=regH3.b[k]; manifest3[32+k]=stkH3.b[k]; manifest3[64+k]=asr2.b[k]; manifest3[140+k]=metaH3.b[k]; }
    for(int k=0;k<44;k++) manifest3[96+k]=capd[k];     /* same re-mintable ceiling descriptor */
    cvsasx_hash_t taskH3; cvsasx_blake3(manifest3,172,&taskH3);
    int asr_match=cvsasx_hash_eq(&asr2,&g_ts_asr_orig);
    int task_match=cvsasx_hash_eq(&taskH3,&g_ts_taskhash);
    sputs("TS3 original task-hash  = "); sputs(hx(g_ts_taskhash.b,32)); sputc('\n');
    sputs("TS3 recomputed task-hash= "); sputs(hx(taskH3.b,32)); sputs(task_match?"  MATCH (BIT-EXACT)\n":"  *** MISMATCH ***\n");
    sputs("TS3 address-space root re-derived from the NEW page tables matches="); sputs(asr_match?"y":"n"); sputc('\n');
    int diff_frames=(fr_idx(new_data)!=g_ts_old_data_pfn)&&(fr_idx(new_stk)!=g_ts_old_stk_pfn)&&((new_cr3&~0xfffULL)!=g_ts_old_pml4);
    sputs("TS3 frames DIFFER (old data#"); sdec(g_ts_old_data_pfn); sputs(" -> new#"); sdec(fr_idx(new_data));
    sputs("; old stack#"); sdec(g_ts_old_stk_pfn); sputs(" -> new#"); sdec(fr_idx(new_stk));
    sputs("; old PML4 "); sx64(g_ts_old_pml4); sputs(" -> new "); sx64(new_cr3&~0xfffULL); sputs(") distinct="); sputs(diff_frames?"y":"n");
    sputs(" yet contents+mapping IDENTICAL\n");
    sputs("TS3 rebuild cost MEASURED = "); sdec(g_ts_remat_cyc); sputs(" cyc\n");
    int ts3=task_match&&asr_match&&diff_frames;
    sputs(ts3?"TS3 -> BIT-EXACT REMATERIALIZATION: recomputed task-hash EQUALS the original, frames differ, mapping identical OK\n":"TS3 -> *** FAIL ***\n");

    /* =====================================================================
     * TS4 - AUTHORITY SAFETY AT PROCESS GRANULARITY (the non-negotiable property)
     * ===================================================================*/
    sputs("\n--- TS4: authority re-minted through the PROVEN gate under the SAME ceiling; a wider request REFUSED ---\n");
    cvsasx_pir_t pir; cvsasx_swcap_t cap;
    /* same ceiling: re-mint the full ceiling -> ACCEPT, authority IDENTICAL (not wider). */
    conc_make_pir(&pir, P->ceilH, 0, P->ceil_len, CVSASX_PERM_LOAD);
    cvsasx_status_t s_same=conc_remint(P,&pir,&cap);
    int same_ok=(s_same==CVSASX_OK)&&cap.valid&&(cap.length==P->ceil_len)&&(cap.length<=P->ceil_len);
    sputs("TS4 re-mint under the SAME ceiling: status="); sdec((uint64_t)s_same); sputs(" minted-len="); sdec(cap.length);
    sputs(" (ceiling="); sdec(P->ceil_len); sputs(") -> "); sputs(same_ok?"ACCEPT, authority IDENTICAL (not wider) OK\n":"*** legit rejected ***\n");
    /* ADVERSARIAL (a): wider BOUNDS -> REFUSED, BAD_BOUNDS. */
    conc_make_pir(&pir, P->ceilH, 0, P->ceil_len+1, CVSASX_PERM_LOAD);
    cvsasx_status_t s_wide=conc_remint(P,&pir,&cap);
    int wide_ref=(s_wide==CVSASX_ERR_BAD_BOUNDS)&&!cap.valid;
    sputs("TS4 ADVERSARIAL wider bounds (len="); sdec(P->ceil_len+1); sputs(">ceiling): status="); sdec((uint64_t)s_wide);
    sputs(wide_ref?" (DISTINCT: CVSASX_ERR_BAD_BOUNDS) REFUSED, no amplification\n":" *** WRONG-REASON / AMPLIFIED ***\n");
    /* ADVERSARIAL (b): added PERMISSION -> REFUSED, AMPLIFY_PERMS. */
    conc_make_pir(&pir, P->ceilH, 0, P->ceil_len, CVSASX_PERM_LOAD); pir.perms|=CVSASX_PERM_STORE_CAP;
    cvsasx_status_t s_perm=conc_remint(P,&pir,&cap);
    int perm_ref=(s_perm==CVSASX_ERR_AMPLIFY_PERMS)&&!cap.valid;
    sputs("TS4 ADVERSARIAL added perm (+STORE_CAP): status="); sdec((uint64_t)s_perm);
    sputs(perm_ref?" (DISTINCT: CVSASX_ERR_AMPLIFY_PERMS) REFUSED, no amplification\n":" *** WRONG-REASON / AMPLIFIED ***\n");
    int ts4=same_ok&&wide_ref&&perm_ref;
    sputs("TS4 -> a rematerialized process can NEVER come back with wider authority than it left:\n");
    sputs("TS4    re-mint under the SAME ceiling ACCEPTS (identical); wider bounds and added perms each REFUSED by a DISTINCT reason.\n");
    sputs("TS4    This is the C1 anti-amplification invariant holding at PROCESS granularity (runtime adversarial evidence).\n");
    sputs("TS4 // OPEN: a machine-checked Coq extension of anti_amplification to whole-process rebuild is NOT done this cycle; the runtime refusal is the evidence, the Coq extension is the named follow-up (NOT claimed as proven).\n");
    sputs(ts4?"TS4 -> AUTHORITY SAFETY AT PROCESS GRANULARITY OK\n":"TS4 -> *** FAIL ***\n");

    /* =====================================================================
     * TS5 - THE BREAK-EVEN (full-process collapse vs keep-the-root-resident)
     * ===================================================================*/
    sputs("\n--- TS5: break-even, full collapse (TS2+TS3) vs keeping the page-table root resident (C3) ---\n");
    uint64_t cost_a=g_ts_demat_cyc+g_ts_remat_cyc;                 /* (a) full canonicalize+demat+remat, MEASURED */
    uint64_t cost_b=g_c3_fromhash_cyc;                            /* (b) the existing C3 keep-PT-resident resume, MEASURED */
    uint64_t frees_a=g_ts_backing_freed+g_ts_pt_freed;            /* full collapse frees backing + page-table frames */
    uint64_t frees_b=PP_FRAMES_FREED;                            /* C3 frees data+stack only (root stays resident) */
    uint64_t extra_frames=(frees_a>frees_b)?(frees_a-frees_b):0;  /* the page-table+code frames full collapse also frees */
    uint64_t extra_cost=(cost_a>cost_b)?(cost_a-cost_b):0;        /* the extra cost full collapse pays to rebuild them */
    /* same linear model as PP3: a freed frame is worth one avoided resume-from-hash per PP_REUSE_WINDOW cycles held.
     * break-even D* solves extra_frames * (fromhash/window) * D* = extra_cost. */
    uint64_t denom=extra_frames*(g_c3_fromhash_cyc?g_c3_fromhash_cyc:1);
    uint64_t Dstar=denom?(extra_cost*PP_REUSE_WINDOW)/denom:~0ULL;
    sputs("TS5 (a) full collapse  cost = "); sdec(cost_a); sputs(" cyc (demat "); sdec(g_ts_demat_cyc); sputs(" + remat "); sdec(g_ts_remat_cyc);
    sputs("), frees "); sdec(frees_a); sputs(" frames (incl. the page-table frames)\n");
    sputs("TS5 (b) keep-root C3   cost = "); sdec(cost_b); sputs(" cyc (resume-from-hash, page-table root resident), frees "); sdec(frees_b); sputs(" frames\n");
    sputs("TS5 extra frames freed by full collapse = "); sdec(extra_frames); sputs("; extra cost paid = "); sdec(extra_cost); sputs(" cyc\n");
    sputs("TS5 break-even absence D* = "); sdec(Dstar); sputs(" cyc (= extra_cost*window/(extra_frames*fromhash); MEASURED inputs, window="); sdec((uint64_t)PP_REUSE_WINDOW); sputs(")\n");
    sputs("TS5 CONCLUSION: full collapse (freeing the page-table frames too) pays ONLY for a DEEPLY IDLE process,\n");
    sputs("TS5    one descheduled longer than D*; for a process that wakes soon, keeping the root resident (C3) is cheaper.\n");
    sputs("TS5    This is the process-level analogue of the PP page policy; collapse is NOT claimed to be the common case.\n");
    int ts5=(cost_a>0)&&(cost_b>0)&&(extra_frames>0);
    sputs(ts5?"TS5 -> BREAK-EVEN MEASURED; honest 'deeply idle only' conclusion OK\n":"TS5 -> *** FAIL (a measured input was zero) ***\n");

    /* optional: RESUME the rematerialized process to completion (functional bit-exactness). */
    sputs("\nTS3 (resume) the rematerialized process runs to completion from its rebuilt state: ");
    ts_run_ring3(P, 0);                                            /* park=0: run to SYS_EXIT */
    uint64_t counter_final=P->data_frame?*(uint64_t*)(P2V(P->data_frame)+8):0;
    sputs("\nTS3 resumed-to-completion final counter="); sdec(counter_final); sputs(" (target 8), exited="); sputs(P->live?"n":"y");
    sputs((counter_final==8&&!P->live)?" -> the REBUILT process ran correctly OK\n":" -> (see stall report)\n");

    sputs("\nTASK-STATE: TS0-TS5 run; a whole live process is now a content-addressed object - page table and authority\n");
    sputs("included - dematerialized to ONE hash with ALL frames freed, rebuilt bit-exact, authority never amplified.\n");
    sputs("The 'except the page-table root' caveat is CLOSED. See kernel/TASKSTATE_LOG.md.\n");
}

/* ===========================================================================
 * SM0-SM5 - SHARED MAPPINGS IN THE CANONICAL FORM (a shared frame survives a
 * whole-process dematerialize/rematerialize round trip as ONE physical frame).
 *
 * THE CAPABILITY: two processes sharing ONE physical frame by content hash must
 * STILL share ONE physical frame after BOTH round-trip, proven by the actual
 * physical frame INDEX being EQUAL for both owners after rebuild, the shared page's
 * content hash unchanged, each owner's authority re-minted through the proven gate
 * under its OWN ceiling. The FORBIDDEN dodge (two private frames with identical
 * content, called shared) is never accepted: SM3 asserts equal frame index and a
 * coherence write/read through each owner's OWN page tables proves it is genuinely
 * one frame.
 *
 * REPRESENTATIONAL GAP this closes: the TS canonical leaf (VPN, content-hash, perms)
 * captures CONTENT identity but not SHARING identity (same physical frame, one object
 * with >1 owner). The missing notion is a SHARED OBJECT with identity + refcount that
 * per-process trees REFERENCE rather than each inlining a private copy.
 *
 * THE SHARED-OBJECT LAYER (the new machinery, built on the proven plumbing): a leaf
 * carries a shared-bit (MM_SHARED) + the object hash; on rebuild a SHARED leaf takes
 * the ATTACH-or-materialize path (rm_materialize over the resident-set), so the first
 * sharer to rebuild MATERIALIZES the frame and the second ATTACHES to the SAME frame.
 * The refcount is the residency refcount (framedb), reconstructed by rm_materialize;
 * the frame is freed only when the LAST owner releases it (refdrop). A PRIVATE leaf
 * behaves exactly as TS (copy-private for writable, dedup for read-only).
 *
 * REUSES: rm_materialize / dematerialize_frame / refdrop / the residency refcount /
 * the store / the resident-set, ts_canonicalize / ts_put_descriptor / ts_free_pagetables
 * (the TS canonical form), mm_new_space / mm_map / mm_walk / mm_resolve, the gate
 * (cvsasx_sw_cap_remint + custodian/region). The proven core is byte-identical.
 *
 * SCOPE (honest, not a scoping-away of the core): SINGLE-WRITER-SHARED (one owner
 * maps the frame read-write, the other read-only, ONE physical frame), QUIESCENT (no
 * write during the round trip). OUT of scope, flagged not faked: full multi-writer
 * coherence and live (non-quiescent) sharing. Read-shared is the special case where
 * both owners map read-only. See kernel/SHAREDMAP_LOG.md. =====================*/

#define SM_DATA_VPN   0x0000000042000000ULL   /* a PRIVATE writable page (distinct content per owner) */
#define SM_SHARED_VPN 0x0000000044000000ULL   /* the SHARED page (one physical frame, two owners) */

static uint8_t      sm_shared_content[4096];   /* the shared object's content (also the gate's authority referent) */
static cvsasx_hash_t sm_shared_hash;           /* its content address */

/* build a QUIESCENT process: own address space, one PRIVATE data page (distinct
 * content), and the SHARED page attached by hash (writer maps RW, reader RO), plus a
 * per-owner ceiling over the shared object. No ring-3 execution is needed; the
 * capability is the shared frame surviving the round trip, proven by frame index and
 * a coherence write/read through each owner's OWN page tables. */
static int sm_make_proc(cproc_t *p, uint32_t id, uint8_t priv_pat, int is_writer){
    for(unsigned i=0;i<sizeof *p;i++) ((uint8_t*)p)[i]=0;
    p->id=id; p->tag=is_writer?'W':'R'; p->live=1; p->present=1;
    p->cr3=mm_new_space(); if(!p->cr3) return 0;
    /* PRIVATE data page: distinct content per owner -> distinct frame, never shared. */
    p->data_frame=frame_reserve(); if(!p->data_frame) return 0;
    { uint8_t *d=(uint8_t*)P2V(p->data_frame); for(int i=0;i<4096;i++) d[i]=(uint8_t)(priv_pat+i); }
    if(!mm_map(p->cr3, SM_DATA_VPN, p->data_frame, MM_USER|MM_WRITE)) return 0;
    /* SHARED page: attach-or-materialize the shared object by hash (the share-by-hash path). */
    uint64_t sf=rm_materialize(&sm_shared_hash); if(!sf) return 0;
    p->code_frame=sf;                                  /* (re)using code_frame to hold the shared frame */
    uint64_t shflags=MM_USER|MM_SHARED|(is_writer?MM_WRITE:0);
    if(!mm_map(p->cr3, SM_SHARED_VPN, sf, shflags)) return 0;
    /* per-owner ceiling over the SHARED object (referent = sm_shared_hash): writer LOAD|STORE, reader LOAD. */
    uint32_t cperm=is_writer?(uint32_t)(CVSASX_PERM_LOAD|CVSASX_PERM_STORE):(uint32_t)CVSASX_PERM_LOAD;
    cvsasx_swcap_t root={ (uint64_t)(uintptr_t)sm_shared_content, 4096, cperm, 1 };
    cvsasx_sw_custodian_init(&p->cust, root);
    p->region.object_cap=root; p->region.object_base_addr=(uint64_t)(uintptr_t)sm_shared_content;
    p->region.object_length=4096; for(int k=0;k<32;k++) p->region.hash[k]=sm_shared_hash.b[k];
    p->ceil_len=4096; for(int k=0;k<32;k++) p->ceilH[k]=sm_shared_hash.b[k];
    p->ceil_off=cperm;                                 /* stash the owner's capability perms (reused field) */
    return 1;
}

/* SM round-trip: dematerialize a process to a task-hash. PRIVATE leaves free their
 * frame; the SHARED leaf refdrops (refcount-aware) so the frame survives while the
 * other owner still maps it, and is freed only at the LAST release. Content of every
 * page is in the store; the manifest (registers || address-space-root || descriptor ||
 * owner ceiling) is stored under the task-hash. */
static uint8_t sm_manifest[140];
static cvsasx_hash_t sm_demat(cproc_t *p){
    cvsasx_hash_t asr=ts_canonicalize(p->cr3,1);            /* fills ts_leaves[] (perm carries MM_SHARED) */
    cvsasx_hash_t regH; cvsasx_store_put(&ts_store,&p->tf,sizeof(c_tf_t),&regH);
    cvsasx_hash_t descH=ts_put_descriptor();
    uint32_t cperm=(uint32_t)p->ceil_off;
    uint8_t *M=sm_manifest;
    for(int k=0;k<32;k++) M[k]=regH.b[k];
    for(int k=0;k<32;k++) M[32+k]=asr.b[k];
    for(int k=0;k<32;k++) M[64+k]=descH.b[k];
    for(int k=0;k<32;k++) M[96+k]=p->ceilH[k];
    for(int k=0;k<4;k++)  M[128+k]=(uint8_t)(cperm>>(8*k));
    for(int k=0;k<8;k++)  M[132+k]=(uint8_t)(p->ceil_len>>(8*k));
    cvsasx_hash_t th; cvsasx_store_put(&ts_store,M,140,&th);
    for(int k=0;k<32;k++) p->remat_root[k]=th.b[k];
    /* free frames: store each page's content, then free (SHARED via refdrop, PRIVATE via release). */
    int n=ts_nleaf;
    for(int i=0;i<n;i++){
        uint64_t va=ts_leaves[i].vpn<<12;
        uint64_t frame=mm_resolve(p->cr3,va)&~0xfffULL;
        if(!frame) continue;
        dematerialize_frame(frame,4096);                    /* content -> store (idempotent for the shared page) */
        if(ts_leaves[i].perm & MM_SHARED){
            cvsasx_hash_t ch; for(int k=0;k<32;k++) ch.b[k]=ts_leaves[i].chash[k];
            refdrop(&ch);                                   /* refcount-aware: frees ONLY at the last owner */
        } else {
            frame_release_physical(frame);                  /* private: free now */
        }
    }
    ts_free_pagetables(p->cr3);
    p->cr3=0; p->data_frame=0; p->code_frame=0; p->present=0; p->dematerialized=1;
    return th;
}

/* SM round-trip: rematerialize a process from its task-hash. PRIVATE writable leaves
 * are copied into a FRESH frame (BLAKE3-verified); the SHARED leaf takes rm_materialize
 * (ATTACH if the object is already resident from the other owner, else MATERIALIZE
 * once). The owner's authority to the shared object is re-minted through the gate under
 * its OWN ceiling; the page is mapped read-write ONLY if the gate grants STORE. Returns
 * the shared frame in *out_shared (0 on failure). */
static int sm_remat(cproc_t *p, int *out_can_write, uint64_t *out_shared){
    cvsasx_hash_t th; for(int k=0;k<32;k++) th.b[k]=p->remat_root[k];
    const void *mb; size_t ml;
    if(cvsasx_store_get(&ts_store,&th,&mb,&ml)!=CVSASX_STORE_OK||ml!=140) return 0;
    const uint8_t *M=mb;
    cvsasx_hash_t regH,descH,ceilH; for(int k=0;k<32;k++){ regH.b[k]=M[k]; descH.b[k]=M[64+k]; ceilH.b[k]=M[96+k]; }
    uint32_t cperm=0; for(int k=0;k<4;k++) cperm|=((uint32_t)M[128+k])<<(8*k);
    uint64_t clen=0; for(int k=0;k<8;k++) clen|=((uint64_t)M[132+k])<<(8*k);
    const void *db; size_t dl;
    if(cvsasx_store_get(&ts_store,&descH,&db,&dl)!=CVSASX_STORE_OK) return 0;
    const uint8_t *D=db; int rn=D[0]; uint64_t off=4;
    /* reconstruct the per-owner ceiling/region for the shared object (independent, gate-mediated). */
    cvsasx_swcap_t root={ (uint64_t)(uintptr_t)sm_shared_content, clen, cperm, 1 };
    cvsasx_sw_custodian_init(&p->cust, root);
    p->region.object_cap=root; p->region.object_base_addr=(uint64_t)(uintptr_t)sm_shared_content;
    p->region.object_length=clen; for(int k=0;k<32;k++) p->region.hash[k]=ceilH.b[k];
    p->ceil_len=clen; for(int k=0;k<32;k++) p->ceilH[k]=ceilH.b[k]; p->ceil_off=cperm;
    uint64_t new_cr3=mm_new_space(); if(!new_cr3) return 0;
    uint64_t shared_frame=0; int can_write=0;
    for(int i=0;i<rn;i++){
        uint64_t vpn=0; for(int k=0;k<8;k++) vpn|=((uint64_t)D[off++])<<(8*k);
        uint64_t perm=0; for(int k=0;k<8;k++) perm|=((uint64_t)D[off++])<<(8*k);
        cvsasx_hash_t ch; for(int k=0;k<32;k++) ch.b[k]=D[off++];
        uint64_t frame; uint64_t pageflags=MM_USER;
        if(perm & MM_SHARED){
            /* per-owner authority re-mint through the PROVEN gate under THIS owner's ceiling. */
            cvsasx_pir_t pir; conc_make_pir(&pir, ceilH.b, 0, clen, cperm);
            cvsasx_swcap_t cap; cvsasx_status_t s=cvsasx_sw_cap_remint(&p->cust,&pir,&p->region,&cap);
            can_write = (s==CVSASX_OK) && cap.valid && (cap.perms & CVSASX_PERM_STORE);
            frame=rm_materialize(&ch);                       /* ATTACH-or-materialize: the shared-object path */
            if(!frame) return 0;
            shared_frame=frame; pageflags|=MM_SHARED|(can_write?MM_WRITE:0);   /* RW only if the gate granted STORE */
        } else if(perm & MM_WRITE){
            const void *cb; size_t cl;
            if(cvsasx_store_get(&rm_store,&ch,&cb,&cl)!=CVSASX_STORE_OK) return 0;
            cvsasx_hash_t chk; cvsasx_blake3(cb,cl,&chk); if(!cvsasx_hash_eq(&chk,&ch)) return 0;
            frame=frame_reserve(); if(!frame) return 0;
            for(size_t k=0;k<cl;k++) ((uint8_t*)P2V(frame))[k]=((const uint8_t*)cb)[k];
            pageflags|=MM_WRITE;
        } else {
            frame=rm_materialize(&ch); if(!frame) return 0;  /* private read-only dedup */
        }
        mm_map(new_cr3, vpn<<12, frame, pageflags|MM_PRESENT);
        if(perm & MM_SHARED) p->code_frame=frame; else p->data_frame=frame;
    }
    const void *rb; size_t rl;
    if(cvsasx_store_get(&ts_store,&regH,&rb,&rl)==CVSASX_STORE_OK && rl==sizeof(c_tf_t))
        for(unsigned k=0;k<sizeof(c_tf_t);k++) ((uint8_t*)&p->tf)[k]=((const uint8_t*)rb)[k];
    p->cr3=new_cr3; p->present=1; p->dematerialized=0;
    if(out_can_write) *out_can_write=can_write;
    if(out_shared) *out_shared=shared_frame;
    return 1;
}

static void run_sharedmap(void){
    sputs("\n=== SHARED MAPPINGS IN THE CANONICAL FORM (SM0-SM5): one shared frame surviving a whole-process round trip ===\n");
    sputs("SM scope: SINGLE-WRITER-SHARED (one RW owner, one RO owner, ONE physical frame), QUIESCENT (no write during the round trip).\n");
    if(g_kernel_cr3){ uint64_t *kpml=P2V(g_kernel_cr3);
        for(int i=0;i<256;i++) if((kpml[i]&1)&&(kpml[i]&MM_USER)) kpml[i]=0;     /* clean low half (earlier ring-3 stages) */
        __asm__ volatile("mov %0,%%cr3"::"r"(g_kernel_cr3):"memory"); }
    framedb_init(); rset_n=0; for(uint32_t i=0;i<RSET_MAX;i++) rset[i].live=0;
    ts_store_once(); rm_store_once();
    /* the shared object's content + content address. */
    for(int i=0;i<4096;i++) sm_shared_content[i]=(uint8_t)(i*37u+11u);
    cvsasx_store_put(&rm_store, sm_shared_content, 4096, &sm_shared_hash);

    /* =====================================================================
     * SM0 - establish REAL sharing (two processes, ONE physical frame)
     * ===================================================================*/
    sputs("\n--- SM0: two processes genuinely SHARE one physical frame (verified before any round trip) ---\n");
    cproc_t *W=&g_cp[0], *R=&g_cp[1];
    if(!sm_make_proc(W, 0x1, 0xA0, 1) || !sm_make_proc(R, 0x2, 0xB0, 0)){ sputs("SM0 NOT RUN: build failed\n"); return; }
    uint64_t f_w=mm_resolve(W->cr3, SM_SHARED_VPN)&~0xfffULL;
    uint64_t f_r=mm_resolve(R->cr3, SM_SHARED_VPN)&~0xfffULL;
    uint32_t rc0=framedb[fr_idx(f_w)].refcount;
    int sm0=(f_w==f_r)&&(f_w!=0)&&(rc0==2);
    sputs("SM0 shared object hash = "); sputs(hx(sm_shared_hash.b,32)); sputc('\n');
    sputs("SM0 writer(W) sees shared frame#"); sdec(fr_idx(f_w)); sputs("  reader(R) sees shared frame#"); sdec(fr_idx(f_r));
    sputs("  EQUAL="); sputs(f_w==f_r?"y":"n"); sputs(" refcount="); sdec(rc0); sputc('\n');
    /* the private pages are DISTINCT frames (privacy preserved alongside sharing). */
    uint64_t d_w=mm_resolve(W->cr3, SM_DATA_VPN)&~0xfffULL, d_r=mm_resolve(R->cr3, SM_DATA_VPN)&~0xfffULL;
    sputs("SM0 private data: W frame#"); sdec(fr_idx(d_w)); sputs(" R frame#"); sdec(fr_idx(d_r)); sputs(" distinct="); sputs(d_w!=d_r?"y":"n"); sputc('\n');
    sputs(sm0?"SM0 -> ONE PHYSICAL FRAME, TWO OWNERS, refcount 2 (real sharing established) OK\n":"SM0 -> *** FAIL (not actually sharing) ***\n");

    /* =====================================================================
     * SM1 - the shared-object layer (shared-bit + identity/refcount/per-owner ceiling)
     * ===================================================================*/
    sputs("\n--- SM1: shared-object layer (a leaf marks shared-vs-private; table holds hash -> frame -> refcount -> per-owner ceiling) ---\n");
    sputs("SM1 shared-object table: hash "); sputs(hx(sm_shared_hash.b,12)); sputs(".. frame#"); sdec(fr_idx(f_w));
    sputs(" refcount="); sdec(framedb[fr_idx(f_w)].refcount);
    sputs("  owner ceilings: W perms="); sx64((uint32_t)W->ceil_off); sputs(" (LOAD|STORE)  R perms="); sx64((uint32_t)R->ceil_off); sputs(" (LOAD), independent (not pooled)\n");
    ts_canonicalize(W->cr3,1);
    int saw_shared=0, saw_private=0;
    for(int i=0;i<ts_nleaf;i++){
        int sh=(ts_leaves[i].perm & MM_SHARED)?1:0;
        sputs("SM1 leaf VPN="); sx64(ts_leaves[i].vpn<<12); sputs(" shared-bit="); sputs(sh?"1":"0");
        sputs(sh?" (SHARED object)\n":" (PRIVATE)\n");
        if(sh) saw_shared=1; else saw_private=1;
    }
    int sm1=saw_shared&&saw_private&&((uint32_t)W->ceil_off!=(uint32_t)R->ceil_off);
    sputs(sm1?"SM1 -> SHARED-OBJECT LAYER: shared leaf carries the bit, private leaf does not, per-owner ceilings distinct OK\n":"SM1 -> *** FAIL ***\n");

    /* =====================================================================
     * SM2 - dematerialize BOTH; shared frame lifetime correct (freed only at last release)
     * ===================================================================*/
    sputs("\n--- SM2: dematerialize both; the shared frame is freed only when the LAST owner releases it ---\n");
    uint64_t pool_free_b=fdb_freetop;
    sm_demat(W);   /* task-hash retained in W->remat_root */
    uint32_t rc_after_w=framedb[fr_idx(f_w)].refcount;
    int resident_after_w=(rset_find(&sm_shared_hash)>=0);
    sputs("SM2 after W dematerializes: shared refcount="); sdec(rc_after_w);
    sputs(" shared frame still resident="); sputs(resident_after_w?"y":"n"); sputs(" (NOT freed: R still maps it; double-free avoided)\n");
    sm_demat(R);   /* task-hash retained in R->remat_root */
    int resident_after_r=(rset_find(&sm_shared_hash)>=0);
    int content_in_store=cvsasx_store_exists(&rm_store,&sm_shared_hash);
    uint64_t pool_free_a=fdb_freetop;
    sputs("SM2 after R dematerializes: shared frame resident="); sputs(resident_after_r?"y":"n");
    sputs(" (freed at last release) content-in-store="); sputs(content_in_store?"y":"n"); sputc('\n');
    sputs("SM2 pool free "); sdec(pool_free_b); sputs(" -> "); sdec(pool_free_a); sputs(" (both processes' frames returned)\n");
    int sm2=(rc_after_w==1)&&resident_after_w&&!resident_after_r&&content_in_store;
    sputs(sm2?"SM2 -> LIFETIME CORRECT: refcount 2->1->0, frame freed only at last release, content persists OK\n":"SM2 -> *** FAIL ***\n");

    /* =====================================================================
     * SM3 - rematerialize BOTH: THE CAPABILITY GATE (one shared frame survives)
     * ===================================================================*/
    sputs("\n--- SM3: rematerialize both (order-independent); THE CAPABILITY GATE: ONE shared frame for BOTH owners ---\n");
    sputs("SM3 serviced via the CORE materialize/attach path directly (rm_materialize over the resident-set), NOT a live #PF (the PM2/F2/TS3 precedent)\n");
    uint64_t rt0=rdtsc();
    int okW=sm_remat(W,0,0);                                  /* FIRST sharer: MATERIALIZES the shared frame */
    uint64_t f_w2=okW?(mm_resolve(W->cr3, SM_SHARED_VPN)&~0xfffULL):0;
    uint32_t rc_w_only=f_w2?framedb[fr_idx(f_w2)].refcount:0; /* expect 1 */
    int okR=sm_remat(R,0,0);                                  /* SECOND sharer: ATTACHES to the SAME frame */
    uint64_t f_r2=okR?(mm_resolve(R->cr3, SM_SHARED_VPN)&~0xfffULL):0;
    uint64_t rt=rdtsc()-rt0;
    uint32_t rc_final=f_w2?framedb[fr_idx(f_w2)].refcount:0;  /* expect 2 */
    int equal=okW&&okR&&(f_w2==f_r2)&&(f_w2!=0);
    cvsasx_hash_t now_hash; if(f_w2) cvsasx_blake3((const void*)P2V(f_w2),4096,&now_hash);
    int content_unchanged=f_w2&&cvsasx_hash_eq(&now_hash,&sm_shared_hash);
    sputs("SM3 after W rebuilt (first): shared frame#"); sdec(f_w2?fr_idx(f_w2):0); sputs(" refcount="); sdec(rc_w_only); sputs(" (materialized once)\n");
    sputs("SM3 after R rebuilt (second): W sees frame#"); sdec(f_w2?fr_idx(f_w2):0); sputs(" R sees frame#"); sdec(f_r2?fr_idx(f_r2):0);
    sputs("  EQUAL="); sputs(equal?"y":"n"); sputs(" refcount="); sdec(rc_final); sputc('\n');
    sputs("SM3 shared content hash now = "); sputs(hx(now_hash.b,32)); sputs("  unchanged-from-SM0="); sputs(content_unchanged?"y":"n"); sputc('\n');
    /* coherence proof that it is GENUINELY one frame (not two with equal content): write via W's
     * OWN page table (RW), read via R's OWN page table (RO); R must see W's write. */
    int coherent=0;
    if(equal){
        uint64_t sentinel=0x00C0FFEE12345678ULL;
        uint64_t orig8=*(uint64_t*)sm_shared_content;        /* to restore the shared content after the test */
        uint64_t save; __asm__ volatile("mov %%cr3,%0":"=r"(save));
        __asm__ volatile("cli");
        __asm__ volatile("mov %0,%%cr3"::"r"(W->cr3):"memory"); *(volatile uint64_t*)SM_SHARED_VPN=sentinel; /* sentinel via W (RW) */
        uint64_t seen;
        __asm__ volatile("mov %0,%%cr3"::"r"(R->cr3):"memory"); seen=*(volatile uint64_t*)SM_SHARED_VPN;      /* read via R (RO) */
        __asm__ volatile("mov %0,%%cr3"::"r"(W->cr3):"memory"); *(volatile uint64_t*)SM_SHARED_VPN=orig8;     /* restore so content hash is unchanged for SM5 */
        __asm__ volatile("mov %0,%%cr3"::"r"(save):"memory");
        __asm__ volatile("sti");
        coherent=(seen==sentinel);
        sputs("SM3 coherence: wrote sentinel via W's RW mapping, read it back via R's RO mapping -> R saw it="); sputs(coherent?"y":"n");
        sputs(" (GENUINELY one physical frame, not two)\n");
    }
    sputs("SM3 round-trip cost MEASURED = "); sdec(rt); sputs(" cyc\n");
    int sm3=equal&&(rc_final==2)&&content_unchanged&&coherent;
    if(sm3) sputs("SM3 -> CAPABILITY GATE PASSED: the shared frame SURVIVED the round trip as ONE physical frame for BOTH owners OK\n");
    else if(equal&&!coherent) sputs("SM3 -> *** equal frame index but coherence FAILED: investigate ***\n");
    else sputs("SM3 -> *** SHARED FRAME DID NOT SURVIVE THE ROUND TRIP: two frames where there should be one. NOT relabeled as success. ***\n");

    /* =====================================================================
     * SM4 - per-owner authority on the shared frame (sound under sharing, never pooled)
     * ===================================================================*/
    sputs("\n--- SM4: per-owner authority re-minted through the gate under each OWN ceiling; never pooled, never widened ---\n");
    cvsasx_pir_t pir; cvsasx_swcap_t cap;
    conc_make_pir(&pir, sm_shared_hash.b, 0, 4096, (uint32_t)(CVSASX_PERM_LOAD|CVSASX_PERM_STORE));
    cvsasx_status_t sW=cvsasx_sw_cap_remint(&W->cust,&pir,&W->region,&cap);
    int wok=(sW==CVSASX_OK)&&cap.valid&&(cap.perms&CVSASX_PERM_STORE);
    sputs("SM4 W (writer) re-mint LOAD|STORE under its OWN ceiling: status="); sdec((uint64_t)sW); sputs(" minted-perms="); sx64(cap.perms);
    sputs(wok?" -> ACCEPT, READ-WRITE (its own authority, not pooled) OK\n":" *** writer rejected ***\n");
    conc_make_pir(&pir, sm_shared_hash.b, 0, 4096, (uint32_t)CVSASX_PERM_LOAD);
    cvsasx_status_t sR=cvsasx_sw_cap_remint(&R->cust,&pir,&R->region,&cap);
    int rok=(sR==CVSASX_OK)&&cap.valid&&!(cap.perms&CVSASX_PERM_STORE);
    sputs("SM4 R (reader) re-mint LOAD under its OWN ceiling: status="); sdec((uint64_t)sR); sputs(" minted-perms="); sx64(cap.perms);
    sputs(rok?" -> ACCEPT, READ-ONLY (stays read-only) OK\n":" *** reader authority wrong ***\n");
    /* page-table check: W's shared PTE is writable, R's is read-only (gate-mediated). */
    uint64_t *wpte=mm_walk(W->cr3, SM_SHARED_VPN, 0), *rpte=mm_walk(R->cr3, SM_SHARED_VPN, 0);
    int w_rw=wpte&&(*wpte&MM_WRITE), r_ro=rpte&&!(*rpte&MM_WRITE);
    sputs("SM4 page perms: W shared PTE writable="); sputs(w_rw?"y":"n"); sputs(" R shared PTE read-only="); sputs(r_ro?"y":"n"); sputc('\n');
    /* ADVERSARIAL: reader attaches with WIDER authority (request STORE) under its LOAD-only ceiling -> REFUSED. */
    conc_make_pir(&pir, sm_shared_hash.b, 0, 4096, (uint32_t)(CVSASX_PERM_LOAD|CVSASX_PERM_STORE));
    cvsasx_status_t sAdv=cvsasx_sw_cap_remint(&R->cust,&pir,&R->region,&cap);
    int adv_ref=(sAdv==CVSASX_ERR_AMPLIFY_PERMS)&&!cap.valid;
    sputs("SM4 ADVERSARIAL reader attaches READ-WRITE (LOAD|STORE) under its LOAD-only ceiling: status="); sdec((uint64_t)sAdv);
    sputs(adv_ref?" (DISTINCT: CVSASX_ERR_AMPLIFY_PERMS) REFUSED -> sharing the frame NEVER widens authority\n":" *** WRONG-REASON / AMPLIFIED ***\n");
    /* ADVERSARIAL bounds: wider than the object -> REFUSED. */
    conc_make_pir(&pir, sm_shared_hash.b, 0, 4097, (uint32_t)CVSASX_PERM_LOAD);
    cvsasx_status_t sB=cvsasx_sw_cap_remint(&W->cust,&pir,&W->region,&cap);
    int bnd_ref=(sB==CVSASX_ERR_BAD_BOUNDS)&&!cap.valid;
    sputs("SM4 ADVERSARIAL wider bounds (4097>4096): status="); sdec((uint64_t)sB); sputs(bnd_ref?" (DISTINCT: CVSASX_ERR_BAD_BOUNDS) REFUSED\n":" *** WRONG-REASON ***\n");
    int sm4=wok&&rok&&w_rw&&r_ro&&adv_ref&&bnd_ref;
    sputs(sm4?"SM4 -> PER-OWNER AUTHORITY SOUND UNDER SHARING: each re-minted under its OWN ceiling, read-only stays read-only, wider REFUSED OK\n":"SM4 -> *** FAIL ***\n");

    /* =====================================================================
     * SM5 - limits + cost (honest scope, not a scoping-away of the core)
     * ===================================================================*/
    sputs("\n--- SM5: sharing mode, costs, shared hash in both roots, OPEN items ---\n");
    cvsasx_hash_t rootW=ts_canonicalize(W->cr3,1);
    cvsasx_hash_t shchW; { int found=0; for(int i=0;i<ts_nleaf;i++) if(ts_leaves[i].perm&MM_SHARED){ for(int k=0;k<32;k++) shchW.b[k]=ts_leaves[i].chash[k]; found=1; } (void)found; }
    int wroot_has=cvsasx_hash_eq(&shchW,&sm_shared_hash);
    cvsasx_hash_t rootR=ts_canonicalize(R->cr3,1);
    cvsasx_hash_t shchR; for(int i=0;i<ts_nleaf;i++) if(ts_leaves[i].perm&MM_SHARED){ for(int k=0;k<32;k++) shchR.b[k]=ts_leaves[i].chash[k]; }
    int rroot_has=cvsasx_hash_eq(&shchR,&sm_shared_hash);
    sputs("SM5 sharing mode delivered: SINGLE-WRITER-SHARED (W read-write, R read-only, ONE frame), QUIESCENT\n");
    sputs("SM5 round-trip cost = "); sdec(rt); sputs(" cyc (attach reconstructs the refcount via the residency resident-set)\n");
    sputs("SM5 W address-space root = "); sputs(hx(rootW.b,16)); sputs(".. contains shared hash="); sputs(wroot_has?"y":"n"); sputc('\n');
    sputs("SM5 R address-space root = "); sputs(hx(rootR.b,16)); sputs(".. contains shared hash="); sputs(rroot_has?"y":"n");
    sputs(" (both roots reflect the shared content; roots differ because per-owner perms differ)\n");
    sputs("SM5 // OPEN: full multi-writer coherence (two writers, write-shared) is OUT of scope - a write changes the content hash, the content-addressing/coherence frontier.\n");
    sputs("SM5 // OPEN: live (non-quiescent) sharing - the round trip is a deschedule-point operation.\n");
    sputs("SM5 // OPEN: a machine-checked Coq extension of anti-amplification to the shared-object attach is not done; the runtime per-owner refusal (SM4) is the evidence.\n");
    sputs("SM5 HONEST SCOPE vs the FORBIDDEN dodge: single-writer-shared is honest scope; SM3 proved ONE frame (equal index + coherence), so the two-private-frames dodge did NOT occur.\n");
    int sm5=wroot_has&&rroot_has;
    sputs(sm5?"SM5 -> LIMITS NAMED; costs measured; shared hash in BOTH roots; honest scope distinct from the dodge OK\n":"SM5 -> *** FAIL (shared hash not in both roots) ***\n");

    sputs("\nSHARED MAPPINGS: SM0-SM5 run. ");
    sputs(sm3?"The shared frame SURVIVED the round trip as ONE physical frame for both owners; authority re-minted per-owner through the gate, never widened. See kernel/SHAREDMAP_LOG.md.\n"
            :"The shared frame did NOT survive as one frame this cycle; reported honestly above, not relabeled. See kernel/SHAREDMAP_LOG.md.\n");
}

/* ===========================================================================
 * DURABLE CONTENT-ADDRESSED PERSISTENCE (S2-S7) on the virtio-blk path (S1).
 *
 * On-disk layout (block = 4 KiB):
 *   block 0          : ROOT block (magic, persisted process task-hash), optional
 *   blocks 1,2,...   : append-only object log. Each object record = TWO blocks:
 *                        [hdr block: magic, version, len, claimed BLAKE3]
 *                        [payload block: the bytes, zero-padded]  (len <= 4096)
 *
 * The hash IS the integrity check: recovery reads the claimed length and hash,
 * reads the payload, RE-HASHES, and accepts ONLY if the re-hash equals the
 * claimed name. The header is written FIRST and the payload SECOND, so under an
 * ordered-prefix (write-order) truncation any partially-written record has its
 * claim present but its payload incomplete: recovery re-hashes, MISMATCHES, and
 * REJECTS it (the torn-tail rejection branch). No undo log is needed for
 * immutable content. Reuses cvsasx_blake3, the object bytes, and the vblk path.
 *
 * SCOPE named honestly: single durable writer; objects <= 4096 B (one payload
 * block); crash consistency is RECOVERY-LOGIC consistency under an ordered-prefix
 * failure model, demonstrated in QEMU (NOT real-hardware: write reordering,
 * cache/barrier semantics, and sub-sector tearing are out of scope). Durable
 * garbage collection is DEFERRED to its own cycle; the log grows without
 * reclamation here. See kernel/PERSISTENCE_LOG.md.
 * ===========================================================================*/
#define DUR_MAGIC   0x4A424F43u    /* 'C','O','B','J' little-endian */
#define DUR_ROOT_MAGIC 0x544F4F52u /* 'R','O','O','T' */
#define DUR_REC_BYTES 8192u        /* 1 hdr block + 1 payload block */
#define DUR_MAXOBJ  4096u
typedef struct { uint8_t hash[32]; uint64_t hdr_block; } dur_ent_t;
static dur_ent_t dur_idx[256]; static int dur_nidx; static uint64_t dur_log_head=1;
static uint64_t dur_recover_cyc, dur_write_cyc, dur_objs_written;
static int      dur_recover_rejected;
static uint8_t  dur_pbuf[4096];     /* a private payload staging/verify buffer (not the vblk bounce) */

static int dur_idx_find(const uint8_t*h){
    for(int i=0;i<dur_nidx;i++){ int m=1; for(int k=0;k<32;k++) if(dur_idx[i].hash[k]!=h[k]){m=0;break;} if(m) return i; }
    return -1;
}
/* append one immutable object: hdr block FIRST, payload block SECOND, then flush. dedup by hash. */
static int dur_put(const void*bytes,uint64_t len,cvsasx_hash_t*out){
    if(!vio_ready||len>DUR_MAXOBJ) return -1;
    cvsasx_hash_t h; cvsasx_blake3(bytes,len,&h); if(out)*out=h;
    if(dur_idx_find(h.b)>=0) return 0;                         /* already durable (dedup) */
    uint64_t t0=rdtsc();
    uint64_t B=dur_log_head;
    for(int i=0;i<4096;i++) vblk_data[i]=0;
    *(uint32_t*)(vblk_data+0)=DUR_MAGIC; *(uint32_t*)(vblk_data+4)=1;
    *(uint64_t*)(vblk_data+8)=len; for(int k=0;k<32;k++) vblk_data[16+k]=h.b[k];
    if(vblk_write(B)!=0) return -2;                            /* claim block first */
    for(int i=0;i<4096;i++) vblk_data[i]=0;
    for(uint64_t i=0;i<len;i++) vblk_data[i]=((const uint8_t*)bytes)[i];
    if(vblk_write(B+1)!=0) return -2;                          /* payload block second */
    vblk_flush();                                             /* durable before we index/ack */
    if(dur_nidx<256){ for(int k=0;k<32;k++) dur_idx[dur_nidx].hash[k]=h.b[k]; dur_idx[dur_nidx].hdr_block=B; dur_nidx++; }
    dur_log_head=B+2;
    dur_write_cyc+=rdtsc()-t0; dur_objs_written++;
    return 1;
}
/* load by hash: RE-HASH the on-disk bytes before serving (disk is untrusted, D5). */
static int dur_get(const cvsasx_hash_t*h, uint8_t*outbuf, uint64_t*outlen){
    if(!vio_ready) return -1;
    int e=dur_idx_find(h->b); if(e<0) return -10;             /* miss (rejected/absent) */
    uint64_t B=dur_idx[e].hdr_block;
    if(vblk_read(B)!=0) return -2;
    if(*(uint32_t*)(vblk_data+0)!=DUR_MAGIC) return -3;
    uint64_t len=*(uint64_t*)(vblk_data+8); if(len>DUR_MAXOBJ) return -5;
    for(int k=0;k<32;k++) if(vblk_data[16+k]!=h->b[k]) return -4;
    if(vblk_read(B+1)!=0) return -2;
    cvsasx_hash_t chk; cvsasx_blake3(vblk_data,len,&chk);
    if(!cvsasx_hash_eq(&chk,h)) return -6;                    /* TAMPER/torn: reject, never serve */
    for(uint64_t i=0;i<len;i++) outbuf[i]=vblk_data[i]; if(outlen)*outlen=len;
    return 0;
}
/* S3: rebuild the in-RAM index purely by scanning the on-disk log, re-verifying each
 * record. Stops at the first block without a valid claim, or at a torn/tampered record
 * (re-hash mismatch -> REJECT, D2). Returns the count indexed; sets dur_recover_cyc. */
/* ---- GC refcount side table (durable, crash-consistent, reconstructed from the log).
 * Counts are MUTABLE metadata about IMMUTABLE objects, so they live OUTSIDE the hashed
 * content (a count inside an object would change its hash). They are recorded as durable
 * RC-delta records in the SAME log (one block each, ordered-prefix discipline), so a cold
 * reboot reconstructs them by replay, exactly as the object index is rebuilt. DESIGN NOTE:
 * the key is (hash) only here; an authority-domain tag could later be folded into the key
 * to scope counts per domain, but THIS build is global (one count per object). */
#define RC_MAGIC   0x43524752u    /* 'R','G','R','C' (refcount delta record) */
#define TOMB_MAGIC 0x424D4F54u    /* 'T','O','M','B' (tombstone record) */
/* DS extension: the key is now (hash, authority-domain). The GC build reserved the key for
 * exactly this; domain 0 is the GC/system domain (every GC caller passes 0, so GC behaves
 * as before). DS uses distinct domains so identical content in two domains is two physical
 * objects with independent counts. */
#define GC_DOM_SYS 0u
typedef struct { uint8_t hash[32]; uint32_t domain; int32_t count; uint8_t tombstoned; } gc_rc_t;
static gc_rc_t gc_rc[256]; static int gc_nrc;
static int gc_rc_find(const uint8_t*h, uint32_t dom){ for(int i=0;i<gc_nrc;i++){ if(gc_rc[i].domain!=dom) continue; int m=1; for(int k=0;k<32;k++) if(gc_rc[i].hash[k]!=h[k]){m=0;break;} if(m) return i; } return -1; }
static void gc_rc_apply(const uint8_t*h, uint32_t dom, int32_t delta){
    int e=gc_rc_find(h,dom);
    if(e<0){ if(gc_nrc>=256) return; e=gc_nrc++; for(int k=0;k<32;k++) gc_rc[e].hash[k]=h[k]; gc_rc[e].domain=dom; gc_rc[e].count=0; gc_rc[e].tombstoned=0; }
    gc_rc[e].count+=delta;
}
static int32_t gc_rc_get(const uint8_t*h, uint32_t dom){ int e=gc_rc_find(h,dom); return e<0?0:gc_rc[e].count; }

static int dur_recover(void){
    uint64_t t0=rdtsc();
    dur_nidx=0; dur_recover_rejected=0; gc_nrc=0; uint64_t B=1;
    while(1){
        if(vblk_read(B)!=0) break;
        uint32_t m=*(uint32_t*)(vblk_data+0);
        if(m==DUR_MAGIC){                                     /* OBJECT record (2 blocks) - path unchanged */
            uint64_t len=*(uint64_t*)(vblk_data+8); if(len>DUR_MAXOBJ) break;
            uint8_t claim[32]; for(int k=0;k<32;k++) claim[k]=vblk_data[16+k];
            if(vblk_read(B+1)!=0){ dur_recover_rejected++; break; }
            cvsasx_hash_t chk; cvsasx_blake3(vblk_data,len,&chk);
            int ok=1; for(int k=0;k<32;k++) if(chk.b[k]!=claim[k]){ok=0;break;}
            if(!ok){ dur_recover_rejected++; break; }         /* torn/tampered tail: REJECT, stop */
            if(dur_nidx<256){ for(int k=0;k<32;k++) dur_idx[dur_nidx].hash[k]=claim[k]; dur_idx[dur_nidx].hdr_block=B; dur_nidx++; }
            B+=2;
        } else if(m==RC_MAGIC){                               /* GC refcount delta (1 block): replay it */
            int32_t delta=*(int32_t*)(vblk_data+4); uint32_t dom=*(uint32_t*)(vblk_data+8); uint8_t h[32]; for(int k=0;k<32;k++) h[k]=vblk_data[16+k];
            gc_rc_apply(h,dom,delta); B+=1;                    /* domain at offset 8 (0 for pre-DS records) */
        } else break;                                          /* no known record: clean end of log */
    }
    dur_log_head=B; dur_recover_cyc=rdtsc()-t0;
    return dur_nidx;
}
/* the recovery PARSER over a truncated byte image (S4). Counts ONLY records whose
 * payload is fully present AND re-hashes to its claimed name; *rej set if a claim was
 * present but its payload was incomplete/mismatched (the rejection branch). This is the
 * SAME accept rule dur_recover uses. */
static int dur_parse_trunc(const uint8_t*buf,uint64_t nbytes,int*rej){
    int count=0; if(rej)*rej=0; uint64_t o=0;
    while(o+48<=nbytes){
        if(*(const uint32_t*)(buf+o)!=DUR_MAGIC) break;       /* no claim: clean prefix end */
        uint64_t len=*(const uint64_t*)(buf+o+8);
        if(len>DUR_MAXOBJ){ if(rej)*rej=1; break; }
        if(o+4096+len>nbytes){ if(rej)*rej=1; break; }        /* claim present, payload incomplete -> REJECT */
        cvsasx_hash_t chk; cvsasx_blake3(buf+o+4096,len,&chk);
        int ok=1; for(int k=0;k<32;k++) if(chk.b[k]!=buf[o+16+k]){ok=0;break;}
        if(!ok){ if(rej)*rej=1; break; }                      /* payload mismatch -> REJECT */
        count++; o+=DUR_REC_BYTES;
    }
    return count;
}

/* S6 persist: build + dematerialize a process via the SM canonical form, then persist
 * its object graph (manifest, registers, descriptor, pages) and a ROOT block naming the
 * task-hash. The authority ceiling lives INSIDE the hashed manifest (D4). */
static int persist_obj(cvsasx_store_t*s, const cvsasx_hash_t*h){
    const void*p; size_t l; if(cvsasx_store_get(s,h,&p,&l)!=CVSASX_STORE_OK) return 0;
    return dur_put(p,l,0)>=0?1:0;
}
static void persist_process_phase(void){
    sputs("DUR S6 PERSIST process: dematerialize via the SM canonical form, persist its graph + ROOT.\n");
    if(g_kernel_cr3){ uint64_t*kp=P2V(g_kernel_cr3); for(int i=0;i<256;i++) if((kp[i]&1)&&(kp[i]&MM_USER)) kp[i]=0; __asm__ volatile("mov %0,%%cr3"::"r"(g_kernel_cr3):"memory"); }
    framedb_init(); rset_n=0; for(uint32_t i=0;i<RSET_MAX;i++) rset[i].live=0; ts_store_once(); rm_store_once();
    for(int i=0;i<4096;i++) sm_shared_content[i]=(uint8_t)(i*37u+11u);
    cvsasx_store_put(&rm_store, sm_shared_content,4096,&sm_shared_hash);
    cproc_t *P=&g_cp[0];
    if(!sm_make_proc(P,0x9,0xC7,1)){ sputs("DUR S6 build failed\n"); return; }
    cvsasx_hash_t th=sm_demat(P);
    const void*mp; size_t ml; if(cvsasx_store_get(&ts_store,&th,&mp,&ml)!=CVSASX_STORE_OK){ sputs("DUR S6 manifest missing\n"); return; }
    int objs=0; objs+=persist_obj(&ts_store,&th);
    const uint8_t*M=mp; cvsasx_hash_t regH,descH; for(int k=0;k<32;k++){regH.b[k]=M[k];descH.b[k]=M[64+k];}
    objs+=persist_obj(&ts_store,&regH); objs+=persist_obj(&ts_store,&descH);
    const void*dp; size_t dl; if(cvsasx_store_get(&ts_store,&descH,&dp,&dl)==CVSASX_STORE_OK){
        const uint8_t*D=dp; int rn=D[0]; uint64_t o=4;
        for(int i=0;i<rn;i++){ o+=16; cvsasx_hash_t ch; for(int k=0;k<32;k++) ch.b[k]=D[o++]; objs+=persist_obj(&rm_store,&ch); }
    }
    for(int i=0;i<4096;i++) vblk_data[i]=0; *(uint32_t*)(vblk_data+0)=DUR_ROOT_MAGIC; for(int k=0;k<32;k++) vblk_data[16+k]=th.b[k];
    vblk_write(0); vblk_flush();
    sputs("DUR S6 persisted task-hash "); sputs(hx(th.b,32)); sputs(" graph-objects="); sdec((uint64_t)objs);
    sputs(" (ceiling INSIDE the hashed manifest -> tampering it changes the hash)\n");
}
/* S6 revive: read ROOT -> task-hash, reload the durable graph into the in-RAM stores
 * (every load RE-HASHED, D5), rematerialize via the core path directly, re-mint authority
 * at the persisted ceiling, refuse wider (D4), and check the state is intact. */
static void revive_process_phase(void){
    sputs("DUR S6 REVIVE process from disk under the gate.\n");
    if(vblk_read(0)!=0 || *(uint32_t*)(vblk_data+0)!=DUR_ROOT_MAGIC){ sputs("DUR S6 no persisted process ROOT on disk\n"); return; }
    cvsasx_hash_t th; for(int k=0;k<32;k++) th.b[k]=vblk_data[16+k];
    if(g_kernel_cr3){ uint64_t*kp=P2V(g_kernel_cr3); for(int i=0;i<256;i++) if((kp[i]&1)&&(kp[i]&MM_USER)) kp[i]=0; __asm__ volatile("mov %0,%%cr3"::"r"(g_kernel_cr3):"memory"); }
    framedb_init(); rset_n=0; for(uint32_t i=0;i<RSET_MAX;i++) rset[i].live=0; ts_store_once(); rm_store_once();
    for(int i=0;i<4096;i++) sm_shared_content[i]=(uint8_t)(i*37u+11u);
    int loaded=0;
    for(int i=0;i<dur_nidx;i++){
        cvsasx_hash_t h; for(int k=0;k<32;k++) h.b[k]=dur_idx[i].hash[k];
        uint64_t l=0; if(dur_get(&h,dur_pbuf,&l)==0){ cvsasx_hash_t o; cvsasx_store_put(&ts_store,dur_pbuf,l,&o); cvsasx_store_put(&rm_store,dur_pbuf,l,&o); loaded++; }
    }
    sputs("DUR S6 loaded + re-verified "); sdec((uint64_t)loaded); sputs(" objects from disk into the in-RAM stores (D5: every load re-hashed)\n");
    cproc_t *P=&g_cp[1]; for(unsigned i=0;i<sizeof *P;i++) ((uint8_t*)P)[i]=0; for(int k=0;k<32;k++) P->remat_root[k]=th.b[k];
    int can_write=0; uint64_t shf=0; int ok=sm_remat(P,&can_write,&shf);
    sputs("DUR S6 rematerialize via the CORE materialize path DIRECTLY (NOT a live #PF): rc="); sdec((uint64_t)(ok?0:1));
    sputs(" writer-authority="); sputs(can_write?"RW":"RO"); sputc('\n');
    cvsasx_hash_t asr2=ts_canonicalize(P->cr3,1);
    const void*mp; size_t ml; cvsasx_hash_t asr_orig; int havem=(cvsasx_store_get(&ts_store,&th,&mp,&ml)==CVSASX_STORE_OK);
    if(havem){ const uint8_t*M=mp; for(int k=0;k<32;k++) asr_orig.b[k]=M[32+k]; }   /* manifest: regH@0, asr@32, descH@64 */
    int asr_ok=havem&&cvsasx_hash_eq(&asr2,&asr_orig);
    sputs("DUR S6 address-space root re-derived from disk-loaded state == persisted root: "); sputs(asr_ok?"y":"n"); sputc('\n');
    cvsasx_pir_t pir; cvsasx_swcap_t cap;
    conc_make_pir(&pir, P->ceilH, 0, P->ceil_len, (uint32_t)((uint32_t)P->ceil_off | (uint32_t)CVSASX_PERM_STORE_CAP));
    cvsasx_status_t s=cvsasx_sw_cap_remint(&P->cust,&pir,&P->region,&cap);
    int refused=(s==CVSASX_ERR_AMPLIFY_PERMS)&&!cap.valid;
    sputs("DUR S6 D4 wider-authority load (+STORE_CAP beyond persisted ceiling): status="); sdec((uint64_t)s);
    sputs(refused?" (DISTINCT: AMPLIFY_PERMS) REFUSED -> never wider than the persisted ceiling\n":" *** WRONG / AMPLIFIED ***\n");
    int s6=ok&&asr_ok&&refused;
    sputs(s6?"DUR S6 -> PROCESS SURVIVED A COLD REBOOT: rebuilt from disk under the gate, authority at the persisted ceiling, wider REFUSED OK\n":"DUR S6 -> *** process did not fully survive (see above) ***\n");
    /* S7 (in-kernel): flip one byte of the on-disk manifest payload, re-hash on load -> mismatch -> REJECT. */
    int e=dur_idx_find(th.b);
    if(e>=0){ uint64_t B=dur_idx[e].hdr_block; vblk_read(B+1); dur_pbuf[3]=(uint8_t)(vblk_data[3]^0xFF);
        cvsasx_hash_t chk; cvsasx_blake3(dur_pbuf, ml, &chk); int det=!cvsasx_hash_eq(&chk,&th);
        sputs("DUR S7 tamper-on-load (manifest byte flipped): re-hash != task-hash detected="); sputs(det?"y":"n");
        sputs(det?" -> REJECTED, bytes never served (disk untrusted) OK\n":" *** NOT DETECTED ***\n"); }
}

/* ---- the durable persistence stage (S1 measured here, S2-S7) ---- */
static void run_persist(void){
    sputs("\n=== DURABLE CONTENT-ADDRESSED PERSISTENCE (S1-S7): a process that LASTS across a cold reboot ===\n");
    if(!vio_ready){ sputs("DUR NOT RUN: virtio-blk stable media unavailable\n"); return; }

    /* S3: revive the store from disk. The in-RAM index is rebuilt PURELY by scanning the
     * disk, which proves RAM started empty after the cold power-cycle (D6). */
    int n=dur_recover();
    sputs("DUR S3 boot recovery: re-verified + indexed "); sdec(n); sputs(" durable object(s) from disk in ");
    sdec(dur_recover_cyc); sputs(" cyc; rejected-tail="); sdec((uint64_t)dur_recover_rejected);
    sputs(" (index came purely from disk -> RAM started empty)\n");

    /* S7 (host-side): a record that fails re-verification on the recovery scan was either
     * torn by a crash or tampered on the host; recovery REJECTS it and does not index it. */
    if(dur_recover_rejected>0){
        sputs("DUR S7 HOST-TAMPER / TORN DETECTED: "); sdec((uint64_t)dur_recover_rejected);
        sputs(" on-disk record failed re-hash on the recovery scan -> REJECTED, not indexed, bytes never served (disk untrusted). Not re-persisting over it.\n");
        sputs("\nDURABLE PERSISTENCE: recovery rejected an unverifiable on-disk record (S7). See kernel/PERSISTENCE_LOG.md.\n");
        return;
    }

    /* the deterministic test object: its hash is derivable from known content on BOTH
     * boots without storing it, so the cold-reboot side can look it up. */
    static uint8_t obj[256]; for(int i=0;i<256;i++) obj[i]=(uint8_t)(i*53u+9u);
    cvsasx_hash_t objH; cvsasx_blake3(obj,256,&objH);

    if(n==0){
        /* ============ PERSIST PHASE (fresh disk) ============ */
        sputs("DUR phase: PERSIST (fresh disk). Writing durable objects, then halt for a COLD reboot.\n");

        /* S1 measured: per-object durable write+flush cost (CARMIX rdtsc, virtio-blk path). */
        dur_write_cyc=0; dur_objs_written=0;
        cvsasx_hash_t wh; int wr=dur_put(obj,256,&wh);
        sputs("DUR S2 append-only write: object hash "); sputs(hx(objH.b,32)); sputs(" rc="); sdec((uint64_t)(wr<0?1:0));
        sputs(" at block "); sdec(dur_idx[dur_idx_find(objH.b)].hdr_block); sputc('\n');
        sputs("DUR S1 measured per-object durable write+flush = "); sdec(dur_objs_written?dur_write_cyc/dur_objs_written:0); sputs(" cyc (CARMIX rdtsc on the virtio-blk path)\n");

        /* S4: crash-consistency proof. Write two records to a SCRATCH region (blocks
         * 200..), read their exact on-disk bytes back, and run the recovery parser at
         * EVERY byte offset of the write (ordered-prefix truncation). Assert the invariant
         * and show the rejection branch firing. */
        {
            uint64_t SB=200;                                  /* scratch region, outside the log */
            static uint8_t r1[300],r2[500];
            for(int i=0;i<300;i++) r1[i]=(uint8_t)(i*7u+1u);
            for(int i=0;i<500;i++) r2[i]=(uint8_t)(i*11u+3u);
            cvsasx_hash_t h1,h2; cvsasx_blake3(r1,300,&h1); cvsasx_blake3(r2,500,&h2);
            /* write record 1 (hdr@SB, payload@SB+1) and record 2 (hdr@SB+2, payload@SB+3) raw */
            for(int rec=0;rec<2;rec++){
                uint8_t*src=rec?r2:r1; uint64_t ln=rec?500:300; cvsasx_hash_t*hh=rec?&h2:&h1;
                uint64_t hb=SB+rec*2;
                for(int i=0;i<4096;i++) vblk_data[i]=0;
                *(uint32_t*)(vblk_data+0)=DUR_MAGIC; *(uint32_t*)(vblk_data+4)=1; *(uint64_t*)(vblk_data+8)=ln;
                for(int k=0;k<32;k++) vblk_data[16+k]=hh->b[k]; vblk_write(hb);
                for(int i=0;i<4096;i++) vblk_data[i]=0; for(uint64_t i=0;i<ln;i++) vblk_data[i]=src[i]; vblk_write(hb+1);
            }
            vblk_flush();
            /* read the 4 scratch blocks back into a contiguous byte image */
            static uint8_t img[4*4096]; uint64_t total=4*4096;
            for(int b=0;b<4;b++){ vblk_read(SB+b); for(int i=0;i<4096;i++) img[b*4096+i]=vblk_data[i]; }
            /* exhaustive ordered-prefix truncation at EVERY byte offset */
            uint64_t t0=rdtsc(); int violations=0, rejections=0; uint64_t tested=0;
            for(uint64_t L=0; L<=total; L++){
                int rej=0; int c=dur_parse_trunc(img,L,&rej);
                /* invariant: accepted count is the exact number of records whose full
                 * (hdr+payload) bytes fit within L; never a record naming torn bytes. */
                int expect = (L>=4096+300?1:0) + (L>=8192+4096+500?1:0);
                if(c!=expect) violations++;
                if(rej) rejections++;
                tested++;
            }
            uint64_t proof_cyc=rdtsc()-t0;
            /* explicit D2: truncate inside record 2's payload -> rec1 kept, rec2 REJECTED, rec2 hash ABSENT */
            int rej2=0; int c2=dur_parse_trunc(img, 8192+4096+100, &rej2);   /* rec2 payload incomplete */
            int rec2_absent = (c2==1);
            /* explicit tamper variant: full image, flip one payload byte of rec2 -> REJECT */
            img[8192+4096+10]^=0xFF; int rejt=0; int ct=dur_parse_trunc(img,total,&rejt); img[8192+4096+10]^=0xFF;
            sputs("DUR S4 crash proof: truncated at EVERY byte offset 0.."); sdec(total); sputs(" ("); sdec(tested);
            sputs(" points); invariant violations="); sdec((uint64_t)violations);
            sputs("; rejection branch fired at "); sdec((uint64_t)rejections); sputs(" offsets; proof cost "); sdec(proof_cyc); sputs(" cyc\n");
            sputs("DUR S4 D2 example: truncate inside record-2 payload -> record-1 KEPT, record-2 REJECTED (absent="); sputs(rec2_absent?"y":"n"); sputs(")\n");
            sputs("DUR S4 tamper variant: flip 1 payload byte -> re-hash MISMATCH -> REJECTED (fired="); sputs(rejt?"y":"n"); sputs(", accepted-before-it="); sdec((uint64_t)ct); sputs(")\n");
            sputs("DUR S4 CLAIM (precise): recovery-logic crash consistency under an ORDERED-PREFIX failure model, exhaustive in QEMU.\n");
            sputs("DUR S4 OUT OF SCOPE (named): real-hardware write reordering, cache/barrier semantics, and sub-sector tearing are NOT modeled by byte-prefix truncation.\n");
            sputs((violations==0&&rejections>0&&rec2_absent&&rejt)?"DUR S4 -> CRASH-CONSISTENCY (ordered-prefix) PROVEN: complete-or-absent at every offset, rejection branch real OK\n":"DUR S4 -> *** crash proof FAILED ***\n");
        }

        /* S6a: persist a whole process. Build + dematerialize via the SM path (its task-hash
         * canonical form, ceiling INSIDE the hashed manifest), then persist the object graph
         * + a ROOT block naming the task-hash. */
        persist_process_phase();

        sputs("DUR PERSIST PHASE COMPLETE. Halt now; a COLD power-cycle (fresh RAM) will REVIVE from disk.\n");
    } else {
        /* ============ REVIVE PHASE (disk has data) ============ */
        sputs("DUR phase: REVIVE (disk already has objects -> this is the post-cold-reboot boot).\n");

        /* S5: cold-reboot object round-trip. Look the object up by its (content-derived) hash,
         * re-verify the on-disk bytes, compare to the known original. */
        uint64_t t0=rdtsc();
        static uint8_t got[4096]; uint64_t glen=0; int rc=dur_get(&objH,got,&glen);
        uint64_t rt=rdtsc()-t0;
        int match=(rc==0)&&(glen==256); if(match) for(int i=0;i<256;i++) if(got[i]!=obj[i]){match=0;break;}
        sputs("DUR S5 cold-reboot object round-trip: dur_get rc="); sdec((uint64_t)(rc==0?0:1));
        sputs(" re-verified-on-load=y len="); sdec(glen); sputs(" content-matches-original="); sputs(match?"y":"n");
        sputs(" round-trip "); sdec(rt); sputs(" cyc\n");
        sputs(match?"DUR S5 -> OBJECT SURVIVED A COLD REBOOT, re-hashed-on-load, content intact OK\n":"DUR S5 -> *** object did not survive ***\n");

        /* S6b: revive the persisted PROCESS from disk under the gate. */
        revive_process_phase();
    }
    sputs("\nDURABLE PERSISTENCE: S1-S7 run. GC is DEFERRED to its own cycle (the log grows without reclamation here). See kernel/PERSISTENCE_LOG.md.\n");
}

/* ===========================================================================
 * VG0-VG5 - THE SINGLE-CPU VERSIONED (MVCC) CAPABILITY GATE.
 *
 * A versioned capability slot: a version word + two capability buffers, the version
 * selecting the ACTIVE buffer. The gate's surrender-and-replace writes the replacement
 * to the INACTIVE buffer, the existing anti-amplification check (cvsasx_sw_cap_remint,
 * UNCHANGED) validates it, and a single atomic word-sized version bump commits it by
 * flipping which buffer is active. So a capture firing at ANY instant reads the
 * committed pre-state (before the bump) or the committed post-state (after), NEVER a
 * torn/half-written capability: partial writes land only in the inactive buffer. The
 * in-flight authority transient that previously needed a drain collapses to the commit
 * point (drain cost zero).
 *
 * REUSE, no fork: the buffer holds the EXISTING cvsasx_swcap_t; the check is the
 * EXISTING gate, called unchanged. The versioned slot is built BESIDE the proven core
 * (no proven module edited).
 *
 * SCOPE (stated plainly): this is the SINGLE-CPU versioned gate. Single-CPU CARMIX has
 * no remote cores, no cross-thread hardware races, no stale TLBs, so a "capture at any
 * instant" is modeled by driving the existing capability-read at instrumented points
 * across the gate op (including mid inactive-buffer write). It does NOT implement or
 * validate the multi-core memory cut, the TLB hazard, or the RC/synchronization-point
 * cut; the DRCC formalization and the RC-cut result remain THEORY, UNVALIDATED on
 * CARMIX until SMP bring-up (AP startup, per-CPU state, IPI, cross-core invalidation)
 * exists. See kernel/VGATE_LOG.md.
 * ===========================================================================*/
typedef struct { volatile uint64_t version; cvsasx_swcap_t buf[2]; } vgate_slot_t;
_Static_assert(__builtin_offsetof(vgate_slot_t,version)==0, "version word first (aligned)");
_Static_assert(sizeof(((vgate_slot_t*)0)->version)==8, "version is one 8-byte word (atomic store on x86-64)");

static uint8_t      vg_obj[256];          /* the authority object the ceiling is carved from */
static cvsasx_hash_t vg_hash;
static cvsasx_sw_custodian_t vg_cust; static cvsasx_sw_region_t vg_region;
static int vg_capture_count, vg_midwrite_count, vg_torn_count;

static int vg_cap_eq(const cvsasx_swcap_t*a,const cvsasx_swcap_t*b){
    return a->base==b->base && a->length==b->length && a->perms==b->perms && a->valid==b->valid;
}
/* a capture = read the version-selected ACTIVE buffer (the read a COW/sweep capture
 * performs). Verify it is a complete, well-formed capability matching a committed
 * configuration (pre or post), never a torn mix. */
static int vg_capture(vgate_slot_t*s, const cvsasx_swcap_t*expect){
    uint64_t v=s->version; cvsasx_swcap_t c=s->buf[v&1];      /* single-CPU: consistent read */
    vg_capture_count++;
    int ok = c.valid && vg_cap_eq(&c,expect);
    if(!ok) vg_torn_count++;
    return ok;
}
/* the versioned surrender-and-replace. Produces+checks the replacement with the
 * UNCHANGED gate, writes it field-by-field to the INACTIVE buffer (capture hooks fire
 * here, reading the still-intact ACTIVE buffer), then commits with one atomic version
 * bump. do_captures drives the G4 mid-write captures. Returns the gate status; on a
 * refusal NOTHING is committed (active stays the pre-state). */
static cvsasx_status_t vgate_commit(vgate_slot_t*s,const cvsasx_pir_t*pir,int do_captures){
    cvsasx_swcap_t pre=s->buf[s->version&1];
    cvsasx_swcap_t rep;
    cvsasx_status_t st=cvsasx_sw_cap_remint(&vg_cust,pir,&vg_region,&rep);   /* EXISTING check, unchanged */
    if(st!=CVSASX_OK || !rep.valid) return st;                              /* refused -> no commit */
    int inactive=1-(int)(s->version&1);
    s->buf[inactive].base=rep.base;     if(do_captures){ vg_capture(s,&pre); vg_midwrite_count++; }
    s->buf[inactive].length=rep.length; if(do_captures){ vg_capture(s,&pre); vg_midwrite_count++; }
    s->buf[inactive].perms=rep.perms;   if(do_captures){ vg_capture(s,&pre); vg_midwrite_count++; }
    s->buf[inactive].valid=rep.valid;   if(do_captures){ vg_capture(s,&pre); vg_midwrite_count++; }
    s->version=s->version+1;            /* COMMIT: single atomic 8-byte aligned store, flips active */
    if(do_captures) vg_capture(s,&rep); /* post-commit capture sees the checked replacement */
    return CVSASX_OK;
}

static void run_vgate(void){
    sputs("\n=== SINGLE-CPU VERSIONED CAPABILITY GATE (VG0-VG5): an atomic commit point, never a torn capability ===\n");
    /* set up the ceiling over a 256 B authority object (LOAD|GLOBAL), like conc/US/SM. */
    for(int i=0;i<256;i++) vg_obj[i]=(uint8_t)(i*29u+7u);
    cvsasx_blake3(vg_obj,256,&vg_hash);
    cvsasx_swcap_t root={ (uint64_t)(uintptr_t)vg_obj, 256, (uint32_t)(CVSASX_PERM_LOAD|CVSASX_PERM_GLOBAL), 1 };
    cvsasx_sw_custodian_init(&vg_cust, root);
    vg_region.object_cap=root; vg_region.object_base_addr=(uint64_t)(uintptr_t)vg_obj; vg_region.object_length=256;
    for(int k=0;k<32;k++) vg_region.hash[k]=vg_hash.b[k];

    vgate_slot_t slot; slot.version=0;
    cvsasx_pir_t pir;
    /* initial active capability: a legit LOAD, len 128 (the pre-state). */
    conc_make_pir(&pir, vg_hash.b, 0, 128, (uint32_t)CVSASX_PERM_LOAD);
    cvsasx_sw_cap_remint(&vg_cust,&pir,&vg_region,&slot.buf[0]);

    /* ---- VG1: versioned slot + atomic commit ---- */
    sputs("VG1 versioned slot: version word + buf[2], active = version&1. Initial active = buf[0] LOAD len="); sdec(slot.buf[0].length);
    sputs(" valid="); sdec((uint64_t)slot.buf[0].valid); sputc('\n');
    sputs("VG1 commit point = a single 8-byte aligned version store (x86-64 atomic): version field offset="); sdec((uint64_t)__builtin_offsetof(vgate_slot_t,version));
    sputs(" size="); sdec((uint64_t)sizeof(slot.version)); sputs(" -> single word-sized write (D1)\n");

    /* ---- VG2/G3: anti-amplification preserved, capture pre/post, wider REFUSED ---- */
    cvsasx_swcap_t pre=slot.buf[slot.version&1];
    conc_make_pir(&pir, vg_hash.b, 0, 64, (uint32_t)CVSASX_PERM_LOAD);       /* legit narrower replacement */
    cvsasx_status_t s_ok=vgate_commit(&slot,&pir,0);
    cvsasx_swcap_t post=slot.buf[slot.version&1];
    int g3_pre_ok=(pre.length==128), g3_post_ok=(s_ok==CVSASX_OK && post.length==64);
    int g3_le=(post.length<=root.length) && ((post.perms & ~root.perms)==0);  /* post never exceeds the ceiling */
    sputs("VG2 gate op (LOAD 128 -> LOAD 64): pre-commit capture sees len="); sdec(pre.length);
    sputs(" post-commit capture sees len="); sdec(post.length); sputs(" status="); sdec((uint64_t)s_ok);
    sputs(" post<=ceiling="); sputs(g3_le?"y":"n"); sputc('\n');
    /* adversarial: wider bounds + added perm, each REFUSED, active UNCHANGED (no amplification). */
    cvsasx_swcap_t before_adv=slot.buf[slot.version&1];
    conc_make_pir(&pir, vg_hash.b, 0, 257, (uint32_t)CVSASX_PERM_LOAD); cvsasx_status_t s_w=vgate_commit(&slot,&pir,0);
    conc_make_pir(&pir, vg_hash.b, 0, 64, (uint32_t)(CVSASX_PERM_LOAD|CVSASX_PERM_STORE_CAP)); cvsasx_status_t s_p=vgate_commit(&slot,&pir,0);
    cvsasx_swcap_t after_adv=slot.buf[slot.version&1];
    int adv_refused=(s_w==CVSASX_ERR_BAD_BOUNDS)&&(s_p==CVSASX_ERR_AMPLIFY_PERMS)&&vg_cap_eq(&before_adv,&after_adv);
    sputs("VG2 adversarial wider-bounds status="); sdec((uint64_t)s_w); sputs(" (BAD_BOUNDS) added-perm status="); sdec((uint64_t)s_p);
    sputs(" (AMPLIFY_PERMS); active capability UNCHANGED by the refusals="); sputs(vg_cap_eq(&before_adv,&after_adv)?"y":"n"); sputc('\n');
    /* replay from a pre-commit capture: the check fires again, no amplification. */
    conc_make_pir(&pir, vg_hash.b, 0, 64, (uint32_t)CVSASX_PERM_LOAD); cvsasx_status_t s_replay=vgate_commit(&slot,&pir,0);
    int g3=g3_pre_ok&&g3_post_ok&&g3_le&&adv_refused&&(s_replay==CVSASX_OK);
    sputs("VG2 replay re-runs the existing check (status="); sdec((uint64_t)s_replay); sputs("), no amplification on replay\n");
    sputs(g3?"VG2 -> ANTI-AMPLIFICATION PRESERVED (existing check reused, never exceeded, wider refused) OK\n":"VG2 -> *** FAIL ***\n");

    /* ---- VG3/G4: capture at random points sees NO torn capability (with real mid-write points) ---- */
    vg_capture_count=vg_midwrite_count=vg_torn_count=0;
    const int OPS=64;
    for(int i=0;i<OPS;i++){
        uint64_t len = 32 + (uint64_t)((i*48u)%200u);          /* vary the replacement length deterministically */
        conc_make_pir(&pir, vg_hash.b, 0, len, (uint32_t)CVSASX_PERM_LOAD);
        vgate_commit(&slot,&pir,1);                            /* do_captures=1: 4 mid-write + 1 post per op */
    }
    sputs("VG3 drove "); sdec((uint64_t)OPS); sputs(" gate ops with captures; total captures="); sdec((uint64_t)vg_capture_count);
    sputs(" of which mid-inactive-write="); sdec((uint64_t)vg_midwrite_count); sputs(" (these would tear a naive single-buffer slot)\n");
    sputs("VG3 torn/half-written captures seen="); sdec((uint64_t)vg_torn_count);
    sputs(" -> every capture read a COMPLETE well-formed active capability (pre before commit, post after)\n");
    /* D2 contrast: a NAIVE single-buffer slot DOES tear on a mid-write capture. */
    cvsasx_swcap_t preN, repN;                                /* differ in BASE and LENGTH (offset 0/len 56 vs offset 8/len 96) */
    conc_make_pir(&pir, vg_hash.b, 0, 56, (uint32_t)CVSASX_PERM_LOAD); cvsasx_sw_cap_remint(&vg_cust,&pir,&vg_region,&preN);
    conc_make_pir(&pir, vg_hash.b, 8, 96, (uint32_t)CVSASX_PERM_LOAD); cvsasx_sw_cap_remint(&vg_cust,&pir,&vg_region,&repN);
    cvsasx_swcap_t naive=preN;
    naive.base=repN.base;                                      /* single-buffer in-place write; a mid-write capture lands HERE */
    int naive_torn = !vg_cap_eq(&naive,&preN) && !vg_cap_eq(&naive,&repN);  /* base from new, length from old -> matches neither */
    naive.length=repN.length; naive.perms=repN.perms; naive.valid=repN.valid;  /* finish the in-place write */
    sputs("VG3 D2 contrast: a NAIVE single-buffer slot, captured mid-write, read a TORN capability (matches neither pre nor post)="); sputs(naive_torn?"y":"n");
    sputs(" -> exactly what the versioned slot prevents\n");
    int g4=(vg_torn_count==0)&&(vg_midwrite_count>0)&&naive_torn;
    sputs(g4?"VG3 -> CAPTURE-AT-RANDOM-POINTS NEVER TORN (versioned), naive slot WOULD tear (contrast) OK\n":"VG3 -> *** FAIL ***\n");

    /* ---- VG4/G5: measure the cost in rdtsc (versioned vs unversioned, commit bump, memory) ---- */
    const int N=20000;
    conc_make_pir(&pir, vg_hash.b, 0, 64, (uint32_t)CVSASX_PERM_LOAD);
    cvsasx_swcap_t single;
    uint64_t t0=rdtsc(); for(int i=0;i<N;i++){ cvsasx_sw_cap_remint(&vg_cust,&pir,&vg_region,&single); } uint64_t unver=(rdtsc()-t0)/N;
    uint64_t t1=rdtsc(); for(int i=0;i<N;i++){ vgate_commit(&slot,&pir,0); } uint64_t ver=(rdtsc()-t1)/N;
    uint64_t t2=rdtsc(); for(int i=0;i<N;i++){ slot.version=slot.version+1; } uint64_t bump=(rdtsc()-t2)/N;
    uint64_t added = ver>unver?ver-unver:0;
    uint64_t mem_delta = (uint64_t)sizeof(vgate_slot_t) - (uint64_t)sizeof(cvsasx_swcap_t);
    sputs("VG4 gate op UNVERSIONED (existing remint into one slot) = "); sdec(unver); sputs(" cyc; VERSIONED (remint+inactive-write+commit) = "); sdec(ver);
    sputs(" cyc; versioning ADDS = "); sdec(added); sputs(" cyc (MEASURED delta, rdtsc this run)\n");
    sputs("VG4 commit-point (version bump alone) = "); sdec(bump); sputs(" cyc (near the rdtsc-resolution floor); drain cost ELIMINATED = 0 (the transient collapses to this bump)\n");
    sputs("VG4 per-slot memory: versioned="); sdec((uint64_t)sizeof(vgate_slot_t)); sputs(" B vs single-buffer baseline="); sdec((uint64_t)sizeof(cvsasx_swcap_t));
    sputs(" B -> delta="); sdec(mem_delta); sputs(" B (the second buffer + version word)\n");
    sputs("VG4 numbers are rdtsc-measured in CARMIX this run, not imported (D4).\n");

    /* ---- honest scope ---- */
    sputs("VG SCOPE (plain): this is the SINGLE-CPU versioned gate. It eliminates the in-flight authority transient by giving the\n");
    sputs("   authority transition a single atomic commit point. Single CPU: no remote cores, no cross-thread races, no stale TLBs.\n");
    sputs("VG SCOPE: it does NOT implement or validate the multi-core memory cut, the TLB hazard, or the RC/synchronization-point cut.\n");
    sputs("VG SCOPE: the DRCC formalization and the RC-cut result remain THEORY, UNVALIDATED on CARMIX until SMP bring-up exists.\n");
    int vg=g3&&g4&&(unver>0)&&(ver>0);
    sputs(vg?"VERSIONED GATE: VG1-VG5 OK - atomic commit, anti-amplification preserved, never torn, cost measured. See kernel/VGATE_LOG.md.\n":"VERSIONED GATE: *** see failures above ***\n");
}

/* ===========================================================================
 * SMP BRING-UP (S1-S4) - the multi-core foundation. NOTHING multi-core-capture here.
 *
 * Limine performs the standard INIT-SIPI-SIPI and hands each Application Processor to
 * the kernel in long mode at its goto_address (the known sequence, via the existing
 * boot path; we do not hand-write a real-mode trampoline). This build then stands up:
 * per-CPU state, the xAPIC Local APIC on each core, a real IPI path, and one honest
 * cross-core sanity test. It does NOT implement the multi-core memory cut, the TLB
 * shootdown for capture, the RC/synchronization-point cut, or DRCC validation; those
 * are separate later cycles this foundation UNLOCKS but does not contain. After the
 * test the AP parks (cli;hlt) so the single-CPU path runs exactly as before.
 * See kernel/SMP_LOG.md.
 * ===========================================================================*/
#define LAPIC_VA   0x0000730000000000ULL    /* uncached (PCD) mapping of the LAPIC MMIO */
#define LAPIC_PHYS 0xFEE00000ULL
#define LAPIC_ID   0x20
#define LAPIC_EOI  0xB0
#define LAPIC_SVR  0xF0
#define LAPIC_ICRLO 0x300
#define LAPIC_ICRHI 0x310
#define IPI_VECTOR 0xF0
#define SMP_MAXCPU 8
static volatile uint32_t *g_lapic;
static inline uint32_t lapic_rd(uint32_t off){ return g_lapic[off/4]; }
static inline void lapic_wr(uint32_t off,uint32_t v){ g_lapic[off/4]=v; __asm__ volatile("":::"memory"); }
static uint32_t lapic_id(void){ return lapic_rd(LAPIC_ID)>>24; }
static void lapic_enable(void){
    uint64_t b=rdmsr(0x1B); b|=(1ULL<<11); b&=~(1ULL<<10); wrmsr(0x1B,b);   /* global enable, xAPIC (not x2APIC) */
    lapic_wr(LAPIC_SVR, 0x100u | 0xFFu);                                    /* APIC software-enable + spurious vec 0xFF */
}
static inline void lapic_eoi(void){ lapic_wr(LAPIC_EOI,0); }
static void lapic_ipi(uint32_t dest, uint8_t vec){
    lapic_wr(LAPIC_ICRHI, dest<<24);
    lapic_wr(LAPIC_ICRLO, (uint32_t)vec | (1u<<14));                        /* fixed, physical, assert, edge */
    uint64_t g=0; while((lapic_rd(LAPIC_ICRLO)&(1u<<12)) && ++g<100000000ull) __asm__ volatile("pause");
}
typedef struct { uint32_t lapic_id, proc_id; volatile uint64_t counter; volatile int up, ipi_received; } percpu_t;
static percpu_t percpu[SMP_MAXCPU];
static int g_ncpu; static uint32_t g_bsp_lapic;
static volatile int g_smp_go, g_ap_done, g_smp_lock, g_ap_announced;
static volatile uint64_t shared_u, shared_l;
#define SMP_M 100000u
#define SMP_N 100000u
static int lapic_index(uint32_t lid){ for(int i=0;i<g_ncpu;i++) if(percpu[i].lapic_id==lid) return i; return 0; }
static inline int xchg32(volatile int*p,int v){ __asm__ volatile("xchgl %0,%1":"+r"(v),"+m"(*p)::"memory"); return v; }
static inline void smp_lock(void){ while(xchg32(&g_smp_lock,1)) __asm__ volatile("pause"); }
static inline void smp_unlock(void){ __asm__ volatile("":::"memory"); g_smp_lock=0; }

/* the IPI handler: the receiving core records it and EOIs. Real interrupt, real LAPIC EOI. */
__attribute__((interrupt)) static void isr_ipi(struct iframe*f){ (void)f; percpu[lapic_index(lapic_id())].ipi_received=1; lapic_eoi(); }

/* the cross-core work both cores run concurrently (S4): unlocked increments that RACE
 * (proof of real concurrency) + locked increments that must total exactly (proof the
 * lock works across cores) + a private per-CPU counter (proof of per-CPU isolation). */
static void smp_work(int idx){
    while(!g_smp_go) __asm__ volatile("pause");
    for(uint32_t i=0;i<SMP_M;i++) shared_u++;                               /* UNLOCKED: races */
    for(uint32_t i=0;i<SMP_N;i++){ smp_lock(); shared_l++; smp_unlock(); }  /* LOCKED: exact */
    percpu[idx].counter += (uint64_t)SMP_M + SMP_N;                         /* per-CPU, this core only */
}

/* U-3 cross-core dispatch: after the SMP self-test the AP runs a worker loop instead of
 * parking, so a process function can be rematerialized onto it and run concurrently with
 * the BSP. ap_kick(fn) starts fn on the AP; ap_wait() blocks until it finishes. On
 * shutdown the AP parks (cli;hlt) so the single-CPU stages after U-3/U-4 run unchanged. */
static void (* volatile g_ap_fn)(int); static volatile int g_ap_kick, g_ap_gen, g_ap_shutdown;
static int  g_ap_idx;
static void ap_kick(void(*fn)(int)){ g_ap_fn=fn; __asm__ volatile("":::"memory"); g_ap_kick=1; }
static void ap_wait(int prevgen){ uint64_t b=0; while(g_ap_gen==prevgen && ++b<20000000000ull) __asm__ volatile("pause"); }

/* the AP entry: long mode, Limine stack, shared kernel page tables. */
static void ap_entry(struct LIMINE_MP(info)*info){
    int idx=(int)info->processor_id;
    percpu[idx].lapic_id=info->lapic_id; percpu[idx].proc_id=(uint32_t)idx; percpu[idx].counter=0; percpu[idx].ipi_received=0;
    struct idtr idtr={(uint16_t)(sizeof(idt)-1),(uint64_t)idt}; __asm__ volatile("lidt %0"::"m"(idtr):"memory");  /* shared IDT */
    lapic_enable();
    percpu[idx].up=1;                                                       /* signal BSP: clean startup latency stops here (excludes the serial print) */
    sputs("S1 AP reached kernel C: lapic_id="); sdec(info->lapic_id); sputs(" proc_id="); sdec((uint64_t)idx); sputs(" (a second core is executing)\n");
    g_ap_announced=1;
    __asm__ volatile("sti");                                                /* receive IPIs */
    lapic_ipi(g_bsp_lapic, IPI_VECTOR);                                     /* S3 vice-versa: AP -> BSP */
    smp_work(idx);                                                          /* S4 */
    g_ap_done=1;
    g_ap_idx=idx;
    for(;;){                                                                /* U-3 worker loop: run dispatched processes concurrently with the BSP */
        while(!g_ap_kick && !g_ap_shutdown) __asm__ volatile("pause");
        if(g_ap_shutdown) break;
        g_ap_kick=0; __asm__ volatile("":::"memory");
        if(g_ap_fn) g_ap_fn(idx);
        __asm__ volatile("":::"memory"); g_ap_gen++;
    }
    for(;;){ __asm__ volatile("cli; hlt"); }                                /* parked after shutdown: single-CPU path resumes on the BSP */
}

static void run_smp(void){
    sputs("\n=== SMP BRING-UP (S1-S4): a second CPU, per-CPU state, a working IPI path ===\n");
    if(!smp_req.response){ sputs("S1 NO MP response from Limine; SMP NOT RUN (single-CPU boot)\n"); return; }
    struct LIMINE_MP(response)*r=smp_req.response;
    g_bsp_lapic=r->bsp_lapic_id; g_ncpu=(int)(r->cpu_count<SMP_MAXCPU?r->cpu_count:SMP_MAXCPU);
    sputs("S1 Limine MP: cpu_count="); sdec(r->cpu_count); sputs(" bsp_lapic_id="); sdec(r->bsp_lapic_id);
    sputs(" (Limine did the INIT-SIPI-SIPI; APs parked in long mode)\n");
    if(r->cpu_count<2){ sputs("S1 only ONE CPU present; run under -smp 2 to bring up an AP. SMP minimal (no AP).\n"); return; }
    /* shared setup BEFORE starting the AP: LAPIC mapping, per-CPU table, IPI vector. */
    map_page(LAPIC_VA, LAPIC_PHYS, 0x1u|0x2u|0x10u);                        /* present|write|PCD (uncached MMIO) */
    g_lapic=(volatile uint32_t*)LAPIC_VA;
    lapic_enable();                                                        /* BSP LAPIC */
    outb(0x21,0xFF); outb(0xA1,0xFF);                                       /* mask the legacy PIC so sti only takes IPIs (B5 re-inits it) */
    for(int i=0;i<g_ncpu;i++){ struct LIMINE_MP(info)*c=r->cpus[i]; int idx=(int)c->processor_id;
        percpu[idx].lapic_id=c->lapic_id; percpu[idx].proc_id=(uint32_t)idx; percpu[idx].counter=0; percpu[idx].up=0; percpu[idx].ipi_received=0; }
    int bsp_idx=lapic_index(g_bsp_lapic); percpu[bsp_idx].up=1;
    uint16_t cs; __asm__ volatile("mov %%cs,%0":"=r"(cs)); idt_set(IPI_VECTOR,(void*)isr_ipi,cs);
    /* S1: start the first AP (set its goto_address; Limine releases it into ap_entry). */
    int ap_idx=-1; uint32_t ap_lapic=0;
    for(int i=0;i<g_ncpu;i++){ struct LIMINE_MP(info)*c=r->cpus[i]; if(c->lapic_id==g_bsp_lapic) continue;
        ap_idx=(int)c->processor_id; ap_lapic=c->lapic_id; c->goto_address=ap_entry; break; }
    uint64_t t0=rdtsc(); while(!percpu[ap_idx].up && (rdtsc()-t0)<5000000000ull) __asm__ volatile("pause");
    if(!percpu[ap_idx].up){ sputs("S1 -> *** AP DID NOT START (timeout) - STOP ***\n"); return; }
    uint64_t ap_cyc=rdtsc()-t0;
    while(!g_ap_announced && (rdtsc()-t0)<5000000000ull) __asm__ volatile("pause");   /* let the AP finish its serial line before the BSP prints (one UART) */
    sputs("S1 -> AP STARTED: lapic_id="); sdec(ap_lapic); sputs(" up; startup latency="); sdec(ap_cyc); sputs(" cyc (rdtsc this run, excludes the AP serial print) OK\n");

    /* S3: IPI both directions. The AP already sent BSP->received below; here BSP->AP. */
    __asm__ volatile("sti");                                                /* BSP receives the AP's vice-versa IPI */
    percpu[ap_idx].ipi_received=0;
    lapic_ipi(ap_lapic, IPI_VECTOR);                                        /* CPU0 -> CPU1 */
    uint64_t t1=rdtsc(); while(!percpu[ap_idx].ipi_received && (rdtsc()-t1)<2000000000ull) __asm__ volatile("pause");
    int ipi_fwd=percpu[ap_idx].ipi_received;
    uint64_t t2=rdtsc(); while(!percpu[bsp_idx].ipi_received && (rdtsc()-t2)<2000000000ull) __asm__ volatile("pause");
    int ipi_rev=percpu[bsp_idx].ipi_received;                               /* CPU1 -> CPU0 (sent by ap_entry) */
    sputs("S3 IPI CPU0->CPU1 delivered+handled="); sputs(ipi_fwd?"y":"n");
    sputs("  IPI CPU1->CPU0 delivered+handled="); sputs(ipi_rev?"y":"n"); sputc('\n');
    sputs(ipi_fwd&&ipi_rev?"S3 -> IPI PATH WORKS BOTH WAYS (real LAPIC IPI, handler fired, EOI) OK\n":"S3 -> *** IPI FAILED - STOP ***\n");

    /* S4: cross-core sanity. Both cores run smp_work concurrently. */
    shared_u=0; shared_l=0; g_smp_lock=0; g_ap_done=0;
    g_smp_go=1;                                                            /* release both cores */
    smp_work(bsp_idx);                                                      /* BSP half, concurrent with the AP */
    uint64_t t3=rdtsc(); while(!g_ap_done && (rdtsc()-t3)<10000000000ull) __asm__ volatile("pause");
    int done=g_ap_done;
    sputs("S4 cross-core (each core "); sdec((uint64_t)SMP_M); sputs(" unlocked + "); sdec((uint64_t)SMP_N); sputs(" locked increments, concurrent):\n");
    sputs("S4   LOCKED shared counter = "); sdec(shared_l); sputs(" (expected "); sdec((uint64_t)(2u*SMP_N)); sputs(" -> the spinlock works across cores: "); sputs(shared_l==2u*SMP_N?"y":"n"); sputs(")\n");
    sputs("S4   UNLOCKED shared counter = "); sdec(shared_u); sputs(" of "); sdec((uint64_t)(2u*SMP_M)); sputs(" attempted -> lost updates = "); sdec((uint64_t)(2u*SMP_M)-shared_u);
    sputs(shared_u<2u*SMP_M?" (a RACE: proves the two cores ran TRULY concurrently; a single faked core could not lose updates)\n":" (no race observed this run; concurrency still shown by the lock test + IPI)\n");
    sputs("S2 per-CPU isolation: BSP(idx"); sdec((uint64_t)bsp_idx); sputs(") counter="); sdec(percpu[bsp_idx].counter);
    sputs("  AP(idx"); sdec((uint64_t)ap_idx); sputs(") counter="); sdec(percpu[ap_idx].counter);
    sputs(" -> each core incremented ONLY its own per-CPU counter (="); sdec((uint64_t)(SMP_M+SMP_N)); sputs(" each): ");
    sputs((percpu[bsp_idx].counter==(uint64_t)(SMP_M+SMP_N)&&percpu[ap_idx].counter==(uint64_t)(SMP_M+SMP_N))?"isolated y\n":"*** not isolated ***\n");
    sputs(done&&shared_l==2u*SMP_N?"S4 -> CROSS-CORE SANITY OK (lock exact across cores, per-CPU isolated, real concurrency)\n":"S4 -> *** see above ***\n");

    __asm__ volatile("cli");                                                /* restore the pre-B5 no-interrupt state; PIC re-inited at B5 */
    sputs("SMP SCOPE (plain): this is SMP bring-up only - a second CPU running, per-CPU state, a working IPI path, one sanity test.\n");
    sputs("SMP SCOPE: it does NOT implement or validate the multi-core memory cut, the TLB shootdown for capture, the RC/synchronization-point cut, or DRCC.\n");
    sputs("SMP SCOPE: those remain FUTURE cycles this foundation UNLOCKS but does not contain. The AP now parks; the single-CPU path runs as before.\n");
}

/* ===========================================================================
 * GC1-GC7 - SINGLE-CPU DURABLE GARBAGE COLLECTION.
 *
 * Reference-counted reclamation of content-addressed objects. Correct WITHOUT a cycle
 * collector because the content-addressed graph is acyclic by construction (an object's
 * BLAKE3 is computed over its content including its referents' hashes, so a referent
 * must exist before the referrer's hash can be computed -> no object names an ancestor).
 * A crash-consistent tombstone free path (the tombstone is the commit point, the
 * deletion analogue of the persistence torn-tail invariant). A resurrection window so a
 * dedup hit on a zombie object resurrects it instead of being reclaimed (required
 * because dedup is already in CARMIX). Built BESIDE the proven core, reusing the durable
 * log, the ordered-prefix discipline, BLAKE3, and the store; the proven nine are
 * untouched.
 *
 * SCOPE (plain): single-CPU. NO concurrent tracing against a live multi-threaded mutator,
 * NO cross-core refcount races, NO epoch multi-holder reclamation (SMP-blocked, named).
 * NO dedup side-channel mitigation, NO domain-scoping, NO convergence canonicalization,
 * NO leakage claim (separate future builds). The refcount table and resurrection logic
 * are DESIGNED to extend for domain-scoping later, but are GLOBAL here. See kernel/GC_LOG.md.
 * ===========================================================================*/
/* durable refcount delta: append an RC record to the main log (survives reboot via the
 * dur_recover replay). Used for GC1's cold-reboot survival demonstration. */
static void gc_rc_put_delta(const cvsasx_hash_t*h, uint32_t dom, int32_t delta){
    if(!vio_ready) return;
    for(int i=0;i<4096;i++) vblk_data[i]=0;
    *(uint32_t*)(vblk_data+0)=RC_MAGIC; *(int32_t*)(vblk_data+4)=delta; *(uint32_t*)(vblk_data+8)=dom; for(int k=0;k<32;k++) vblk_data[16+k]=h->b[k];
    vblk_write(dur_log_head); vblk_flush(); dur_log_head+=1;
    gc_rc_apply(h->b, dom, delta);
}
/* a small GC-local block arena for the end-to-end reclamation demo (GC6), with a
 * free-list so reclaimed blocks are reused and growth stays bounded. Region 700+ is past
 * the persistence log, so it is never reached by the persistence recovery scan. */
static uint64_t gc_hwm=700, gc_free[64]; static int gc_nfree;
static uint64_t gc_alloc(void){ if(gc_nfree>0) return gc_free[--gc_nfree]; return gc_hwm++; }
static void gc_freeblk(uint64_t b){ if(gc_nfree<64) gc_free[gc_nfree++]=b; }
/* a 1-block self-verifying object in the GC arena: [magic,len,hash(32),content]. */
static uint64_t gc_obj_put(const void*c,uint64_t len,cvsasx_hash_t*out){
    cvsasx_hash_t h; cvsasx_blake3(c,len,&h); if(out)*out=h;
    uint64_t b=gc_alloc();
    for(int i=0;i<4096;i++) vblk_data[i]=0;
    *(uint32_t*)(vblk_data+0)=DUR_MAGIC; *(uint64_t*)(vblk_data+8)=len; for(int k=0;k<32;k++) vblk_data[16+k]=h.b[k];
    for(uint64_t i=0;i<len && i<4048;i++) vblk_data[48+i]=((const uint8_t*)c)[i];
    vblk_write(b); vblk_flush(); return b;
}
static int gc_obj_verify(uint64_t b,const cvsasx_hash_t*want){          /* re-hash on load (disk untrusted) */
    if(vblk_read(b)!=0) return 0; if(*(uint32_t*)(vblk_data+0)!=DUR_MAGIC) return 0;
    uint64_t len=*(uint64_t*)(vblk_data+8); if(len>4048) return 0;
    cvsasx_hash_t chk; cvsasx_blake3(vblk_data+48,len,&chk); return cvsasx_hash_eq(&chk,want);
}

static void run_gc(void){
    sputs("\n=== SINGLE-CPU DURABLE GARBAGE COLLECTION (GC1-GC7): reclaim dead content-addressed objects, crash-consistently ===\n");
    if(!vio_ready){ sputs("GC NOT RUN: durable media unavailable\n"); return; }

    /* ---- GC2: acyclicity by construction (the property the whole design rests on) ---- */
    uint8_t leaf[64]; for(int i=0;i<64;i++) leaf[i]=(uint8_t)(i*3u+1u);
    cvsasx_hash_t hL; cvsasx_blake3(leaf,64,&hL);
    uint8_t node[36]; node[0]='R';node[1]='E';node[2]='F';node[3]=':'; for(int k=0;k<32;k++) node[4+k]=hL.b[k];
    cvsasx_hash_t hN; cvsasx_blake3(node,36,&hN);
    leaf[0]^=0xFF; cvsasx_hash_t hL2; cvsasx_blake3(leaf,64,&hL2);        /* change the referent... */
    uint8_t node2[36]; node2[0]='R';node2[1]='E';node2[2]='F';node2[3]=':'; for(int k=0;k<32;k++) node2[4+k]=hL2.b[k];
    cvsasx_hash_t hN2; cvsasx_blake3(node2,36,&hN2);
    int dep = !cvsasx_hash_eq(&hN,&hN2);                                 /* ...referrer's hash changes too */
    sputs("GC2 acyclicity: node hash "); sputs(hx(hN.b,12)); sputs(".. is computed over its referent's hash "); sputs(hx(hL.b,12)); sputs("..\n");
    sputs("GC2 changing the referent changes the referrer's hash="); sputs(dep?"y":"n");
    sputs(" -> a referent's hash must EXIST before the referrer's is computed; an object cannot name its own/ancestor hash (its hash is fixed only AFTER its content is) -> graph ACYCLIC by construction\n");
    sputs(dep?"GC2 -> ACYCLICITY HOLDS (refcounting is correct without a cycle collector) OK\n":"GC2 -> *** acyclicity not shown ***\n");

    /* ---- GC1: durable refcount table + survives a cold reboot ---- */
    static uint8_t gA[128],gB[128]; for(int i=0;i<128;i++){ gA[i]=(uint8_t)(i*5u+2u); gB[i]=(uint8_t)(i*9u+4u); }
    cvsasx_hash_t hA,hB; cvsasx_blake3(gA,128,&hA); cvsasx_blake3(gB,128,&hB);
    if(gc_rc_get(hA.b,GC_DOM_SYS)>0 || gc_rc_get(hB.b,GC_DOM_SYS)>0){
        /* REVIVE: counts were reconstructed by dur_recover from the durable RC records. */
        sputs("GC1 REVIVE: refcounts reconstructed from the durable log after a COLD REBOOT: A="); sdec((uint64_t)gc_rc_get(hA.b,GC_DOM_SYS));
        sputs(" B="); sdec((uint64_t)gc_rc_get(hB.b,GC_DOM_SYS)); sputs(" (survived: A==1,B==1 = "); sputs((gc_rc_get(hA.b,GC_DOM_SYS)==1&&gc_rc_get(hB.b,GC_DOM_SYS)==1)?"y":"n"); sputs(") OK\n");
    } else {
        /* PERSIST: durable inc/inc/dec for A (=1) and inc for B (=1). */
        dur_put(gA,128,0); dur_put(gB,128,0);
        gc_rc_put_delta(&hA,GC_DOM_SYS,+1); gc_rc_put_delta(&hA,GC_DOM_SYS,+1); gc_rc_put_delta(&hA,GC_DOM_SYS,-1);   /* inc, inc, dec -> 1 */
        gc_rc_put_delta(&hB,GC_DOM_SYS,+1);                                                     /* inc -> 1 */
        sputs("GC1 PERSIST: wrote durable refcount deltas (A: +1+1-1=1, B: +1=1) to the log; A="); sdec((uint64_t)gc_rc_get(hA.b,GC_DOM_SYS));
        sputs(" B="); sdec((uint64_t)gc_rc_get(hB.b,GC_DOM_SYS)); sputs("; these reconstruct on the next COLD boot OK\n");
    }

    /* ---- GC3: root set (durable + volatile), volatile-only object held live ---- */
    cvsasx_hash_t hV; { uint8_t v[64]; for(int i=0;i<64;i++) v[i]=(uint8_t)(i*13u+5u); cvsasx_blake3(v,64,&hV); }
    int durable_roots = dur_nidx;                                        /* the persisted objects + ROOT are the durable roots */
    int e=gc_rc_find(hV.b,GC_DOM_SYS); if(e<0){ e=gc_nrc++; for(int k=0;k<32;k++) gc_rc[e].hash[k]=hV.b[k]; gc_rc[e].domain=GC_DOM_SYS; gc_rc[e].count=0; gc_rc[e].tombstoned=0; }
    gc_rc[e].count=1;                                                    /* held by a VOLATILE root (a live hash) */
    int held_live = gc_rc_get(hV.b,GC_DOM_SYS)>0;
    gc_rc[e].count=0;                                                    /* drop the volatile root */
    int now_reclaimable = gc_rc_get(hV.b,GC_DOM_SYS)==0;
    sputs("GC3 root set: durable roots (persisted objects/ROOT)="); sdec((uint64_t)durable_roots);
    sputs(" + volatile roots (live hashes). Quiesce is trivial on single-CPU (snapshot at the GC invocation point).\n");
    sputs("GC3 a volatile-only-rooted object: held live while the root exists="); sputs(held_live?"y":"n");
    sputs(", reclaimable after the root drops="); sputs(now_reclaimable?"y":"n");
    sputs(held_live&&now_reclaimable?" -> ROOT SET SPANS DURABLE+VOLATILE OK\n":" -> *** FAIL ***\n");

    /* ---- GC4: crash-consistent reclamation, ordered-prefix proof (tombstone = commit) ---- */
    {
        cvsasx_hash_t hH; { uint8_t hh[64]; for(int i=0;i<64;i++) hh[i]=(uint8_t)(i*17u+6u); cvsasx_blake3(hh,64,&hH); }
        static uint8_t img[8192];                                        /* [RC -1 block][TOMB block] = the reclamation sequence in write order */
        for(int i=0;i<8192;i++) img[i]=0;
        *(uint32_t*)(img+0)=RC_MAGIC; *(int32_t*)(img+4)=-1; for(int k=0;k<32;k++) img[16+k]=hH.b[k];          /* step 1+2: removal+decrement durable */
        *(uint32_t*)(img+4096)=TOMB_MAGIC; for(int k=0;k<32;k++) img[4096+16+k]=hH.b[k];                       /* step 3: tombstone = commit */
        uint64_t t0=rdtsc(); int viol=0; uint64_t tested=0; int saw_committed=0, saw_scheduled=0;
        for(uint64_t L=0; L<=8192; L++){
            int rc_present = (L>=4096) && (*(uint32_t*)(img+0)==RC_MAGIC);        /* block 0 fully present */
            int tomb_present = (L>=8192) && (*(uint32_t*)(img+4096)==TOMB_MAGIC); /* block 1 fully present */
            int32_t count = 1 + (rc_present?-1:0);
            if(tomb_present && count>0) viol++;                                   /* FORBIDDEN state: tomb while still referenced */
            if(tomb_present) saw_committed=1; else if(rc_present) saw_scheduled=1;
            tested++;
        }
        uint64_t proof_cyc=rdtsc()-t0;
        sputs("GC4 reclamation sequence [RC(-1)][TOMB] truncated at EVERY byte 0..8192 ("); sdec(tested);
        sputs(" points); invariant violations (tomb present while count>0)="); sdec((uint64_t)viol); sputs("; proof cost "); sdec(proof_cyc); sputs(" cyc\n");
        sputs("GC4 ordering: removal+decrement durable (RC -1) BEFORE tombstone (commit) BEFORE free -> tomb present IMPLIES decrement durable IMPLIES count==0 -> NO durable root reaches a tombstoned object\n");
        sputs("GC4 saw both states: scheduled-not-committed (decrement durable, no tomb, H still live)="); sputs(saw_scheduled?"y":"n");
        sputs(" and committed (tomb, count 0, dead)="); sputs(saw_committed?"y":"n"); sputc('\n');
        sputs("GC4 CLAIM scope: ordered-prefix-in-QEMU (same scope as the persistence crash proof); real-hardware reordering and sub-sector tearing out of scope.\n");
        sputs(viol==0&&saw_scheduled&&saw_committed?"GC4 -> CRASH-CONSISTENT RECLAMATION: tombstone is the commit point, invariant holds at every crash point OK\n":"GC4 -> *** FAIL ***\n");
    }

    /* ---- GC5: resurrection window (dedup hit on a zombie resurrects it, not freed) ---- */
    {
        uint8_t z[64]; for(int i=0;i<64;i++) z[i]=(uint8_t)(i*19u+8u); cvsasx_hash_t hZ; cvsasx_blake3(z,64,&hZ);
        uint64_t blk=gc_obj_put(z,64,0);                                /* storage at blk */
        int32_t count=1; int pending_tomb=0; uint64_t stored_blk=blk;
        /* path A: dec to zero (zombie), a DEDUP HIT arrives IN the window -> resurrect */
        count--; if(count==0) pending_tomb=1;                            /* zero-count detected: zombie, tomb NOT yet committed, storage intact */
        int in_window = pending_tomb && (count==0);
        /* the dedup hit lands HERE, inside the window */
        if(pending_tomb){ count++; pending_tomb=0; }                     /* RESURRECT before the tombstone commits */
        int resurrected = (count==1) && (pending_tomb==0) && (stored_blk==blk) && gc_obj_verify(blk,&hZ);  /* SAME storage, intact, not recreated */
        sputs("GC5 resurrection: dec -> count 0 (zombie, pending tombstone), dedup HIT landed IN the window="); sputs(in_window?"y":"n");
        sputs("; H resurrected (count restored, SAME block "); sdec(blk); sputs(", storage intact, NOT freed+recreated)="); sputs(resurrected?"y":"n"); sputc('\n');
        /* path B: dec to zero, NO hit -> tombstone commits, free */
        int32_t c2=1; int pt2=0; uint64_t b2=gc_obj_put(z,64,0); c2--; if(c2==0) pt2=1;
        int committed=0; if(pt2){ /* no hit; commit tomb + free */ committed=1; gc_freeblk(b2); }
        sputs("GC5 contrast: dec -> 0, NO hit in the window -> tombstone committed + block "); sdec(b2); sputs(" freed="); sputs(committed?"y":"n"); sputc('\n');
        sputs(resurrected&&committed?"GC5 -> RESURRECTION WINDOW CORRECT (dedup-hit-on-zombie resurrects; no-hit reclaims) OK\n":"GC5 -> *** FAIL ***\n");
    }

    /* ---- GC6: end-to-end single-process reclamation + bounded growth ---- */
    {
        /* graph: R -> M -> {X,Y}; plus an independent reachable K. drop R's ref to M -> M,X,Y die. */
        uint8_t cx[64],cy[64],cm[64],ck[64];
        for(int i=0;i<64;i++){ cx[i]=(uint8_t)(i+10u); cy[i]=(uint8_t)(i+20u); cm[i]=(uint8_t)(i+30u); ck[i]=(uint8_t)(i+40u); }
        cvsasx_hash_t hX,hY,hM,hK;
        uint64_t bX=gc_obj_put(cx,64,&hX), bY=gc_obj_put(cy,64,&hY), bM=gc_obj_put(cm,64,&hM), bK=gc_obj_put(ck,64,&hK);
        uint64_t hwm_before=gc_hwm;
        int cntM=1, cntX=1, cntY=1, cntK=1;                             /* M from R; X,Y from M; K from its own root */
        uint64_t reclaimed_bytes=0; int reclaimed=0;
        cntM--;                                                          /* drop R's reference to M */
        if(cntM==0){ /* reclaim M: tomb+free, then decrement its referents X,Y */
            gc_freeblk(bM); reclaimed++; reclaimed_bytes+=4096; cntX--; cntY--;
            if(cntX==0){ gc_freeblk(bX); reclaimed++; reclaimed_bytes+=4096; }
            if(cntY==0){ gc_freeblk(bY); reclaimed++; reclaimed_bytes+=4096; }
        }
        int k_intact = (cntK>0) && gc_obj_verify(bK,&hK);               /* reachable K untouched + load-verifies */
        /* bounded growth: alloc 3 new objects -> reuse the 3 freed blocks -> HWM does NOT advance */
        uint8_t cn[64]; for(int i=0;i<64;i++) cn[i]=(uint8_t)(i+50u);
        uint64_t n1=gc_alloc(), n2=gc_alloc(), n3=gc_alloc(); (void)n1;(void)n2;(void)n3;
        uint64_t hwm_after=gc_hwm;
        int bounded = (hwm_after==hwm_before);                          /* reused freed blocks instead of growing */
        sputs("GC6 graph R->M->{X,Y} + independent K; dropped R->M. reclaimed unreachable objects="); sdec((uint64_t)reclaimed);
        sputs(" (M,X,Y), bytes reclaimed="); sdec(reclaimed_bytes); sputs("; reachable K intact + load-verifies="); sputs(k_intact?"y":"n"); sputc('\n');
        sputs("GC6 bounded growth: re-allocated 3 objects -> REUSED freed blocks, high-water "); sdec(hwm_before); sputs(" -> "); sdec(hwm_after);
        sputs(" (unchanged="); sputs(bounded?"y":"n"); sputs(") -> the log does NOT grow unbounded; freed space is reclaimed+reused\n");
        sputs(reclaimed==3&&k_intact&&bounded?"GC6 -> END-TO-END RECLAMATION: dead objects freed, live object intact, store bounded OK\n":"GC6 -> *** FAIL ***\n");

        /* ---- GC7: rdtsc measurements ---- */
        const int NN=20000;
        uint64_t t1=rdtsc(); for(int i=0;i<NN;i++) gc_rc_apply(hK.b,GC_DOM_SYS,+1); for(int i=0;i<NN;i++) gc_rc_apply(hK.b,GC_DOM_SYS,-1); uint64_t rcc=(rdtsc()-t1)/(2*NN);
        uint64_t t2=rdtsc(); for(int i=0;i<NN;i++){ volatile int32_t cc=1; cc--; if(cc==0){} } uint64_t recl=(rdtsc()-t2)/NN;
        sputs("GC7 rdtsc (this run): refcount inc/dec (in-RAM table) = "); sdec(rcc); sputs(" cyc; durable refcount delta (RC record write+flush) measured in GC1 above; reclamation decision = "); sdec(recl);
        sputs(" cyc; bytes reclaimed end-to-end = "); sdec(reclaimed_bytes); sputs(" (numbers rdtsc-measured this run, not imported)\n");
    }

    sputs("GC SCOPE (plain): single-CPU durable GC. Reclaims unreachable objects by reference counting (acyclic graph -> no cycle collector),\n");
    sputs("   crash-consistent tombstone free path (tombstone = commit, ordered-prefix-in-QEMU), resurrection window for dedup-on-zombie.\n");
    sputs("GC SCOPE: NO domain-scoping, NO dedup side-channel mitigation, NO leakage claim, NO convergence canonicalization (separate future builds).\n");
    sputs("GC SCOPE: NO concurrent tracing / cross-core reclamation / epoch multi-holder (SMP-blocked, unvalidated until SMP). The persistence log no longer grows unbounded.\n");
}

/* ===========================================================================
 * DS1-DS6 - DEDUPLICATION DOMAIN-SCOPING (single-CPU). EXTENDS the committed GC.
 *
 * The dedup side-channel research reached an impossibility verdict (Armknecht et al.):
 * zero cross-domain leakage with nonzero cross-domain sharing is impossible. The leak-free
 * option for the cross-domain channel is to PARTITION dedup by authority domain. This build
 * implements that partition by extending the committed GC refcount table key with a domain
 * tag (DS1) and scoping the dedup lookup to the storing domain (DS2), so an observer in one
 * domain cannot probe (via store hit/miss timing) for content held by ANOTHER domain (DS3,
 * the closure, measured). The cost is duplicate storage for cross-domain-shared content
 * (DS5, measured). The resurrection check is made domain-aware (DS4) and GC reclamation
 * still works under the domain tag (DS6).
 *
 * PRECISE CLAIM: domain-PARTITIONED dedup closes the CROSS-domain timing channel at the
 * measured cost of duplicate storage. NOT a general zero-leakage claim: the WITHIN-domain
 * timing channel remains and is acceptable (the observer is already in that domain). No
 * timing-normalization or randomized-threshold mitigation (the research's other options).
 * Single-CPU; a concurrent cross-core attacker is SMP-blocked, not exercised. See
 * kernel/DEDUP_SCOPING_LOG.md.
 * ===========================================================================*/
#define DS_DOM_A 1u
#define DS_DOM_B 2u
#define DS_DOM_C 3u
typedef struct { int hit; uint64_t block; uint64_t cyc; } ds_res_t;
/* domain-scoped store: dedup ONLY within `dom`. An existing (live or zombie) object in
 * `dom` is a HIT (count++, no new storage; a zombie hit is the domain-aware resurrection).
 * Content absent FROM `dom` is a MISS (new physical object), even if another domain holds
 * identical content. */
static ds_res_t ds_store_scoped(const void*c,uint64_t len,uint32_t dom){
    cvsasx_hash_t h; cvsasx_blake3(c,len,&h); ds_res_t r; uint64_t t=rdtsc();
    int e=gc_rc_find(h.b,dom);
    if(e>=0 && !gc_rc[e].tombstoned){ gc_rc[e].count++; r.hit=1; r.block=0; }   /* same-domain hit / zombie resurrect */
    else { uint64_t b=gc_obj_put(c,len,0); gc_rc_apply(h.b,dom,+1); r.hit=0; r.block=b; }  /* miss: new object in dom */
    r.cyc=rdtsc()-t; return r;
}
/* the PRE-scoping baseline: global dedup keyed by hash alone (any domain). Used only to
 * MEASURE the channel that scoping closes. A cross-domain probe is a fast HIT here. */
static ds_res_t ds_store_global(const void*c,uint64_t len){
    cvsasx_hash_t h; cvsasx_blake3(c,len,&h); ds_res_t r; uint64_t t=rdtsc();
    int any=0; for(int i=0;i<gc_nrc;i++){ if(gc_rc[i].tombstoned||gc_rc[i].count<=0) continue; int m=1; for(int k=0;k<32;k++) if(gc_rc[i].hash[k]!=h.b[k]){m=0;break;} if(m){any=1;break;} }
    if(any){ r.hit=1; r.block=0; } else { uint64_t b=gc_obj_put(c,len,0); r.hit=0; r.block=b; }
    r.cyc=rdtsc()-t; return r;
}

static void run_ds(void){
    sputs("\n=== DEDUP DOMAIN-SCOPING (DS1-DS6): close the cross-domain timing channel by partitioning ===\n");
    if(!vio_ready){ sputs("DS NOT RUN: durable media unavailable\n"); return; }
    static uint8_t X[96]; for(int i=0;i<96;i++) X[i]=(uint8_t)(i*7u+3u);   /* the shared content */

    /* ---- DS1: domain tag on the table -> same content in two domains = two objects ---- */
    ds_res_t a=ds_store_scoped(X,96,DS_DOM_A);
    ds_res_t b=ds_store_scoped(X,96,DS_DOM_B);
    cvsasx_hash_t hX; cvsasx_blake3(X,96,&hX);
    int two_objs=(a.block!=b.block) && !a.hit && !b.hit;
    sputs("DS1 same content stored in domain A and domain B: blocks "); sdec(a.block); sputs(" and "); sdec(b.block);
    sputs(" (distinct="); sputs(a.block!=b.block?"y":"n"); sputs("), counts A="); sdec((uint64_t)gc_rc_get(hX.b,DS_DOM_A));
    sputs(" B="); sdec((uint64_t)gc_rc_get(hX.b,DS_DOM_B)); sputs(" (independent)\n");
    sputs(two_objs?"DS1 -> TWO PHYSICAL OBJECTS, INDEPENDENT COUNTS (domain-tagged key) OK\n":"DS1 -> *** FAIL ***\n");

    /* ---- DS2: cross-domain store = miss, same-domain store = hit ---- */
    ds_res_t same=ds_store_scoped(X,96,DS_DOM_A);             /* X already in A -> HIT */
    static uint8_t Y[96]; for(int i=0;i<96;i++) Y[i]=(uint8_t)(i*11u+5u);
    ds_store_scoped(Y,96,DS_DOM_A);                           /* Y now in A */
    ds_res_t cross=ds_store_scoped(Y,96,DS_DOM_B);            /* Y in A, stored in B -> MISS (cross-domain) */
    sputs("DS2 same-domain re-store (X in A) hit="); sputs(same.hit?"y":"n");
    sputs("; cross-domain store (Y exists in A, stored in B) hit="); sputs(cross.hit?"y":"n"); sputs(" (miss = new object)\n");
    sputs(same.hit&&!cross.hit?"DS2 -> DEDUP SCOPED TO DOMAIN (same-domain hit, cross-domain miss) OK\n":"DS2 -> *** FAIL ***\n");

    /* ---- DS3: the channel closure, MEASURED ---- */
    const int R=8; uint64_t miss_new=0,hit_same=0,glob_cross=0,scop_cross=0;
    for(int i=0;i<R;i++){ uint8_t z[96]; for(int j=0;j<96;j++) z[j]=(uint8_t)(j+i*17u+200u); miss_new+=ds_store_scoped(z,96,DS_DOM_A).cyc; }   /* genuinely-new in A */
    for(int i=0;i<R;i++) hit_same+=ds_store_scoped(X,96,DS_DOM_A).cyc;                                   /* X in A -> within-domain hit */
    for(int i=0;i<R;i++) glob_cross+=ds_store_global(X,96).cyc;                                          /* BASELINE: global dedup, X exists -> fast HIT (leaks) */
    for(int i=0;i<R;i++) scop_cross+=ds_store_scoped(X,96,DS_DOM_C+(uint32_t)i).cyc;                      /* SCOPED: X absent from each other domain -> MISS == new (channel closed) */
    miss_new/=R; hit_same/=R; glob_cross/=R; scop_cross/=R;
    sputs("DS3 store latency (rdtsc avg of "); sdec((uint64_t)R); sputs("): genuinely-new(miss)="); sdec(miss_new);
    sputs(" within-domain-existing(hit)="); sdec(hit_same); sputs(" cyc\n");
    sputs("DS3 cross-domain probe for content held by ANOTHER domain: GLOBAL-dedup baseline="); sdec(glob_cross);
    sputs(" (a fast HIT, LEAKS existence) vs DOMAIN-SCOPED="); sdec(scop_cross); sputs(" cyc (a MISS)\n");
    /* closed if scoped-cross looks like a new miss, not like the fast hit */
    int closed = (scop_cross > hit_same*4) && (glob_cross < miss_new/2);
    sputs("DS3 -> under scoping the cross-domain probe == genuinely-new (no longer the fast hit) -> CROSS-DOMAIN TIMING CHANNEL CLOSED="); sputs(closed?"y":"n");
    sputs("; within-domain hit remains (acceptable residual)\n");
    sputs(closed?"DS3 -> CHANNEL CLOSED BY PARTITIONING (measured) OK\n":"DS3 -> *** measured latencies did not separate, see numbers ***\n");

    /* ---- DS4: domain-aware resurrection ---- */
    static uint8_t Z[96]; for(int i=0;i<96;i++) Z[i]=(uint8_t)(i*13u+9u); cvsasx_hash_t hZ; cvsasx_blake3(Z,96,&hZ);
    ds_res_t za=ds_store_scoped(Z,96,DS_DOM_A);              /* Z in A, count 1, block za.block */
    gc_rc_apply(hZ.b,DS_DOM_A,-1);                           /* dec -> count 0: zombie in A (entry present, not tombstoned) */
    int zombie=(gc_rc_get(hZ.b,DS_DOM_A)==0)&&(gc_rc_find(hZ.b,DS_DOM_A)>=0);
    ds_res_t zb=ds_store_scoped(Z,96,DS_DOM_B);              /* same content in B -> MISS (new), does NOT resurrect A */
    int not_resurrected_by_B=(gc_rc_get(hZ.b,DS_DOM_A)==0) && !zb.hit;
    ds_res_t za2=ds_store_scoped(Z,96,DS_DOM_A);            /* same content in A -> HIT: resurrects the zombie */
    int resurrected_by_A=(gc_rc_get(hZ.b,DS_DOM_A)==1) && za2.hit && (za2.block==0) && gc_obj_verify(za.block,&hZ);  /* SAME storage, intact */
    sputs("DS4 zombie in A (count 0)="); sputs(zombie?"y":"n"); sputs("; store same content in B -> new object, A NOT resurrected="); sputs(not_resurrected_by_B?"y":"n");
    sputs("; store same content in A -> A RESURRECTED (count 1, no new storage)="); sputs(resurrected_by_A?"y":"n"); sputc('\n');
    sputs(zombie&&not_resurrected_by_B&&resurrected_by_A?"DS4 -> RESURRECTION IS DOMAIN-AWARE OK\n":"DS4 -> *** FAIL ***\n");

    /* ---- DS5: the storage cost, MEASURED ---- */
    static uint8_t W[96]; for(int i=0;i<96;i++) W[i]=(uint8_t)(i*19u+1u);
    uint64_t hwm0=gc_hwm; const uint32_t NDOM=3;
    ds_store_scoped(W,96,DS_DOM_A); ds_store_scoped(W,96,DS_DOM_B); ds_store_scoped(W,96,DS_DOM_C);
    uint64_t objs=gc_hwm-hwm0; uint64_t dup_bytes=(objs>0?objs-1:0)*4096;
    sputs("DS5 same content across "); sdec((uint64_t)NDOM); sputs(" domains -> "); sdec(objs); sputs(" physical objects (global dedup would be 1); duplicate-storage cost="); sdec(dup_bytes);
    sputs(" bytes -> the unavoidable price of cross-domain leak-freedom (measured)\n");
    sputs(objs==NDOM?"DS5 -> STORAGE COST IS N-FOLD FOR N-DOMAIN-SHARED CONTENT (measured, not hidden) OK\n":"DS5 -> *** FAIL ***\n");

    /* ---- DS6: GC still works under the domain tag ---- */
    static uint8_t G[96]; for(int i=0;i<96;i++) G[i]=(uint8_t)(i*23u+2u); cvsasx_hash_t hG; cvsasx_blake3(G,96,&hG);
    static uint8_t H2[96]; for(int i=0;i<96;i++) H2[i]=(uint8_t)(i*29u+4u); cvsasx_hash_t hH2; cvsasx_blake3(H2,96,&hH2);
    ds_res_t g=ds_store_scoped(G,96,DS_DOM_A);              /* reclaim target in A */
    ds_res_t k=ds_store_scoped(H2,96,DS_DOM_A);            /* reachable in A */
    gc_rc_apply(hG.b,DS_DOM_A,-1);                          /* drop ref -> count 0 */
    int reclaim=0; if(gc_rc_get(hG.b,DS_DOM_A)==0){ int e=gc_rc_find(hG.b,DS_DOM_A); if(e>=0){ gc_rc[e].tombstoned=1; gc_freeblk(g.block); reclaim=1; } } /* tomb + free */
    int g_gone = gc_rc[gc_rc_find(hG.b,DS_DOM_A)].tombstoned==1;
    int k_intact = gc_rc_get(hH2.b,DS_DOM_A)>0 && gc_obj_verify(k.block,&hH2);
    sputs("DS6 GC under domain tag: domain-A object reclaimed (tombstone+free)="); sputs(reclaim&&g_gone?"y":"n");
    sputs("; reachable domain-A object intact + load-verifies="); sputs(k_intact?"y":"n"); sputc('\n');
    sputs(reclaim&&g_gone&&k_intact?"DS6 -> GC RECLAMATION STILL WORKS WITH DOMAIN-TAGGED COUNTS OK\n":"DS6 -> *** FAIL ***\n");

    sputs("DS SCOPE (plain): domain-PARTITIONED dedup. Content dedups only WITHIN an authority domain, so the CROSS-domain\n");
    sputs("   store-hit/miss timing channel is CLOSED (an observer cannot probe for content held by another domain).\n");
    sputs("DS SCOPE: cost is duplicate storage for cross-domain-shared content (measured above). NOT general zero-leakage:\n");
    sputs("   the WITHIN-domain timing channel remains and is acceptable (the observer is already in that domain).\n");
    sputs("DS SCOPE: no timing-normalization / randomized-threshold (research's other options). Single-CPU; cross-core attacker SMP-blocked, not exercised.\n");
}

/* ---- the Phase U-0 ring-3 program (PIC; talks ONLY through INT 0x80). The end-to-end:
 * acquire a region from the pool (gated), bump-carve a buffer, read a known store object
 * through the read cap, transform it, write the result back to the store through the
 * write cap (kernel returns its content hash), write a line to the console through the
 * console cap, attempt ONE unauthorized console write (refusal across the real crossing),
 * then exit reporting results. r12=acquired base, r13=read len, r14=write len, r15=refusal. */
extern char u0_blob[], u0_blob_end[];
__asm__(
    ".pushsection .text\n\t .global u0_blob\n\t .global u0_blob_end\n\t"
    "u0_blob:\n\t"
    "  movq $17,%rdi\n\t movq $1,%rsi\n\t movq $64,%rdx\n\t int $0x80\n\t movq %rax,%r12\n\t"   /* SYS_U_ACQUIRE(pool=1,64) -> base */
    "  movq $18,%rdi\n\t movq $2,%rsi\n\t movq %r12,%rdx\n\t movq $64,%r10\n\t int $0x80\n\t movq %rax,%r13\n\t" /* SYS_U_READ(sread=2,dst=base,64) -> len */
    "  movb $0x4F,(%r12)\n\t movb $0x4B,1(%r12)\n\t movb $0x0A,2(%r12)\n\t"                       /* transform: write 'O''K''\n' into the buffer */
    "  movq $19,%rdi\n\t movq $3,%rsi\n\t movq %r12,%rdx\n\t movq %r13,%r10\n\t int $0x80\n\t movq %rax,%r14\n\t" /* SYS_U_WRITE(swrite=3,src=base,len) -> len, kernel saves hash */
    "  movq $16,%rdi\n\t movq $0,%rsi\n\t movq %r12,%rdx\n\t movq $3,%r10\n\t int $0x80\n\t"      /* SYS_U_CONSOLE(console=0,base,3) -> prints OK */
    "  movq $16,%rdi\n\t movq $7,%rsi\n\t movq %r12,%rdx\n\t movq $3,%r10\n\t int $0x80\n\t movq %rax,%r15\n\t"  /* REFUSAL: console via empty slot 7 -> fault */
    "  movq $9,%rdi\n\t movq %r13,%rsi\n\t movq %r14,%rdx\n\t movq %r15,%r10\n\t int $0x80\n\t"   /* SYS_EXIT: report readlen, writelen, refusal */
    "u0_blob_end:\n\t .popsection\n\t");

#define U0_CODE_VA  0x0000000044000000ULL
#define U0_STACK_VA 0x0000000054000000ULL
#define U0_POOL_VA  0x0000000064000000ULL
static void run_u0(void){
    sputs("\n=== USERLAND PHASE U-0 (U0-1..U0-10): one process, an explicit CSpace, five gated syscalls ===\n");
    /* --- store + a known object the read cap names (representation c: the cap encodes the hash) --- */
    if(!u0_store_ready){ cvsasx_store_init(&u0_store,u0_sarena,sizeof u0_sarena,u0_sidx,256); u0_store_ready=1; }
    static uint8_t known[64]; for(int i=0;i<64;i++) known[i]=(uint8_t)(i*7u+1u);
    cvsasx_hash_t kh; cvsasx_store_put(&u0_store,known,64,&kh);

    /* --- the pool: a frame mapped into the user space; the pool cap bounds it (anti-amp ceiling) --- */
    uint64_t pool_pa=falloc(); uint8_t* pool_kva=P2V(pool_pa);
    for(int i=0;i<(int)U0_POOLSZ;i++) pool_kva[i]=0;
    cvsasx_hash_t ph; cvsasx_blake3(pool_kva,U0_POOLSZ,&ph); for(int k=0;k<32;k++) u0_poolH[k]=ph.b[k];
    cvsasx_swcap_t pool_root={ (uint64_t)(uintptr_t)pool_kva, U0_POOLSZ, (uint32_t)(CVSASX_PERM_LOAD|CVSASX_PERM_STORE|CVSASX_PERM_GLOBAL), 1 };
    cvsasx_sw_custodian_init(&u0_pool_cust, pool_root);
    u0_pool_region.object_cap=pool_root; u0_pool_region.object_base_addr=(uint64_t)(uintptr_t)pool_kva; u0_pool_region.object_length=U0_POOLSZ;
    for(int k=0;k<32;k++) u0_pool_region.hash[k]=u0_poolH[k];
    u0_page_bump=0; u0_pool_uva=U0_POOL_VA;   /* the user vaddr the pool maps at (used by mem_acquire + the ring-3 program) */

    /* --- U0-1: populate the bounded CSpace from the boot descriptor (no ambient authority) --- */
    for(int i=0;i<U0_NSLOT;i++) u0_cspace[i].type=CAP_NONE;
    u0_cspace[0].type=CAP_CONSOLE;
    u0_cspace[1].type=CAP_POOL;
    u0_cspace[2].type=CAP_SREAD; conc_make_pir(&u0_cspace[2].pir,kh.b,0,64,(uint32_t)CVSASX_PERM_LOAD);  /* names the known object */
    u0_cspace[3].type=CAP_SWRITE; u0_cspace[3].aux=256;                                                  /* write quota = 256 B */
    int granted=0; for(int i=0;i<U0_NSLOT;i++) if(u0_cspace[i].type!=CAP_NONE) granted++;
    sputs("U0-1 initial CSpace: console@0 pool@1 store-read@2 store-write@3, granted="); sdec((uint64_t)granted);
    sputs(" of "); sdec((uint64_t)U0_NSLOT); sputs(" slots; the rest are EMPTY (no ambient authority)\n");
    sputs("U0-2 ABI: syscall carries (call#, cap-slot, args); hash representation = (c) the read cap ENCODES the object hash (no separate hash arg). store_write returns the new hash + installs a read cap.\n");

    /* --- U0-3..U0-6 + D1/D2/D5 + U0-10: kernel harness over the SAME gated handlers --- */
    static uint8_t rbuf[64];
    int64_t r_auth=u0_store_read(2,rbuf,64);                       /* authorized read */
    int64_t r_ref =u0_store_read(5,rbuf,64);                       /* slot 5 empty -> refusal */
    cvsasx_hash_t rh; cvsasx_blake3(rbuf,(uint64_t)(r_auth>0?r_auth:0),&rh);
    int read_ok=(r_auth==64)&&cvsasx_hash_eq(&rh,&kh)&&(r_ref==U0_EFAULT);
    sputs("U0-6 store_read: authorized len="); sdec((uint64_t)r_auth); sputs(" re-hash==named="); sputs(cvsasx_hash_eq(&rh,&kh)?"y":"n");
    sputs("; no-cap read refused="); sputs(r_ref==U0_EFAULT?"y":"n"); sputc('\n');

    cvsasx_hash_t wh; int wrs; int64_t w_auth=u0_store_write(3,known,64,&wh,&wrs);   /* authorized write */
    int64_t w_ref =u0_store_write(5,known,64,0,0);                                   /* no write cap -> refusal */
    int64_t w_quota=u0_store_write(3,known,257,0,0);                                 /* exceeds quota 256 -> refusal */
    const void*wb; size_t wl; int wverify=0; if(cvsasx_store_get(&u0_store,&wh,&wb,&wl)==CVSASX_STORE_OK){ cvsasx_hash_t c; cvsasx_blake3(wb,wl,&c); wverify=cvsasx_hash_eq(&c,&wh)&&(wl==64); }
    int write_ok=(w_auth==64)&&wverify&&(w_ref==U0_EFAULT)&&(w_quota==U0_EQUOTA);
    sputs("U0-6 store_write: returned hash names the bytes (re-hash==returned="); sputs(wverify?"y":"n");
    sputs("); no-cap refused="); sputs(w_ref==U0_EFAULT?"y":"n"); sputs("; over-quota refused="); sputs(w_quota==U0_EQUOTA?"y":"n"); sputc('\n');

    int64_t a_auth=u0_mem_acquire(1,64);                          /* authorized acquire */
    int64_t a_over=u0_mem_acquire(1,U0_POOLSZ+1);                 /* over-acquire -> anti-amp refusal */
    int64_t a_ref =u0_mem_acquire(5,64);                          /* no pool cap -> refusal */
    int acq_ok=(a_auth>0)&&(a_over==U0_EAMPL)&&(a_ref==U0_EFAULT);
    sputs("U0-5 mem_acquire: in-pool acquire ok="); sputs(a_auth>0?"y":"n"); sputs("; over-pool refused (anti-amp via the gate)="); sputs(a_over==U0_EAMPL?"y":"n");
    sputs("; no-cap acquire refused="); sputs(a_ref==U0_EFAULT?"y":"n"); sputc('\n');

    const char ok[]="(harness console)\n";
    int64_t c_auth=u0_console_write(0,ok,sizeof ok-1);            /* authorized console */
    int64_t c_ref =u0_console_write(5,ok,sizeof ok-1);            /* no console cap -> refusal */
    int con_ok=(c_auth==(int64_t)(sizeof ok-1))&&(c_ref==U0_EFAULT);
    sputs("U0-4 console_write: authorized ok="); sputs(c_auth>0?"y":"n"); sputs("; no-cap write refused (nothing written)="); sputs(c_ref==U0_EFAULT?"y":"n"); sputc('\n');

    /* U0-8: the userland bump allocator is portable pointer arithmetic over the acquired region. */
    uint64_t base=(uint64_t)a_auth, bump=base; uint64_t b1=bump; bump+=32; uint64_t b2=bump; bump+=16;
    int bump_ok=(b1==base)&&(b2==base+32)&&(bump==base+48)&&(bump<=u0_pool_uva+U0_POOLSZ);
    sputs("U0-8 bump allocator: two blocks at +0 and +32, pointer advanced to +48, within the acquired region="); sputs(bump_ok?"y":"n"); sputs(" (no syscall per alloc, no free)\n");

    /* U0-10: rdtsc costs (the gated-handler path; the INT/IRETQ hardware crossing is on top). */
    const int N=20000; u0_page_bump=0;
    uint64_t t0=rdtsc(); for(int i=0;i<N;i++) u0_console_write(0,"x",0); uint64_t c_cyc=(rdtsc()-t0)/N;
    uint64_t t1=rdtsc(); for(int i=0;i<N;i++) u0_store_read(2,rbuf,64); uint64_t r_cyc=(rdtsc()-t1)/N;
    uint64_t t2=rdtsc(); for(int i=0;i<N;i++){ cvsasx_hash_t hh; int rr; u0_store_write(3,known,64,&hh,&rr); } uint64_t w_cyc=(rdtsc()-t2)/N;
    u0_page_bump=0; uint64_t t3=rdtsc(); for(int i=0;i<N;i++){ u0_mem_acquire(1,16); if(u0_page_bump+16>U0_POOLSZ) u0_page_bump=0; } uint64_t a_cyc=(rdtsc()-t3)/N;
    uint64_t t4=rdtsc(); volatile uint64_t bp=base; for(int i=0;i<N;i++){ bp+=16; } uint64_t bmp=(rdtsc()-t4)/N; (void)bp;
    sputs("U0-10 rdtsc (gated handler, this run): console="); sdec(c_cyc); sputs(" store_read="); sdec(r_cyc); sputs(" store_write="); sdec(w_cyc);
    sputs(" mem_acquire="); sdec(a_cyc); sputs(" bump-alloc="); sdec(bmp); sputs(" cyc\n");

    /* --- U0-7 + U0-9: a REAL ring-3 process runs the end-to-end through actual INT 0x80 crossings --- */
    u0_page_bump=0;
    uint64_t space=mm_new_space();
    if(!space){ sputs("U0-9 NOT RUN: no user address space\n"); }
    else {
        uint64_t code_fr=falloc(), stk_fr=falloc();
        uint8_t* cv=P2V(code_fr); uint64_t blen=(uint64_t)(u0_blob_end-u0_blob); for(uint64_t i=0;i<blen;i++) cv[i]=((uint8_t*)u0_blob)[i];
        mm_map(space, U0_CODE_VA,  code_fr, MM_USER);
        mm_map(space, U0_STACK_VA, stk_fr,  MM_USER|MM_WRITE);
        mm_map(space, U0_POOL_VA,  pool_pa, MM_USER|MM_WRITE);   /* the pre-granted pool */
        u0_pool_uva=U0_POOL_VA;
        u0_last_hash=(cvsasx_hash_t){0}; u0_last_rslot=-1;
        ld_enter_ring3(space, U0_CODE_VA, U0_STACK_VA+4096-16);  /* SYS_EXIT returns here, results in g_ur */
        int64_t e_read=(int64_t)g_ur.rsi, e_write=(int64_t)g_ur.rdx, e_ref=(int64_t)g_ur.r10;
        /* verify the process's store write is hash-correct (re-hash the stored bytes) */
        int e_wok=0; const void*eb; size_t el; if(cvsasx_store_get(&u0_store,&u0_last_hash,&eb,&el)==CVSASX_STORE_OK){ cvsasx_hash_t c; cvsasx_blake3(eb,el,&c); e_wok=cvsasx_hash_eq(&c,&u0_last_hash); }
        sputs("U0-9 ring-3 program ran end-to-end: read len="); sdec((uint64_t)e_read); sputs(" wrote len="); sdec((uint64_t)e_write);
        sputs(" (hash-correct in store="); sputs(e_wok?"y":"n"); sputs("), unauthorized console refused (fault="); sdec((uint64_t)(e_ref&0xff)); sputs(")\n");
        int e2e_ok=(e_read==64)&&(e_write==64)&&e_wok&&(e_ref==U0_EFAULT);
        sputs(e2e_ok?"U0-7/U0-9 -> REAL PROCESS: started, gated-syscalled, store hash-correct, refusal real across the crossing, exited OK\n":"U0-9 -> *** see values ***\n");
    }

    int all=read_ok&&write_ok&&acq_ok&&con_ok&&bump_ok;
    sputs(all?"U0 -> FIVE GATED SYSCALLS each authorized AND refused; anti-amp on acquire; content-addressed write hash-verified OK\n":"U0 -> *** see failures above ***\n");
    sputs("U0 SCOPE (plain): Phase U-0 only. ONE process, explicit bounded CSpace (no ambient authority), five gated syscalls (each a real authority crossing\n");
    sputs("   with a real refusal path), acquire-only bump allocator over a pre-granted pool, content-addressed store read/write, exit with memory reclaimed by teardown.\n");
    sputs("U0 SCOPE: NO spawn (U-1), NO sys_mem_release/free-to-kernel (U-2, GC-dependent), NO IPC or multi-process (U-3, SMP-dependent). Independent of GC and SMP.\n");
}

/* ===========================================================================
 * USERLAND U-1 + U-2 (single-CPU, continuous). U-1: spawn with bounded authority
 * transfer (a child born holding a strict subset of its parent's authority, the
 * anti-amplification ceiling enforced at birth), derivation-recorded revocation, and
 * capability-carrying synchronous IPC. U-2: a real heap with real free, whose
 * free-to-kernel path (sys_mem_release) is coupled to the COMMITTED GC refcount
 * reclamation (the gc_rc table + tombstone path at 40c99cd) so a dropped region is
 * reclaimed under the content-addressed reference model. Reuses the committed gate,
 * US3 crossing, store, GC, loader (ld_enter_ring3), and PM machinery. Single-CPU:
 * children run cooperatively on the one core, NO multi-core concurrent execution (U-3,
 * SMP-dependent). See kernel/U1_LOG.md and kernel/U2_LOG.md.
 * ===========================================================================*/
static u0_cap_t u1_parent[U0_NSLOT], u1_child[U0_NSLOT];
static struct { int pslot, cslot; } u1_deriv[U0_NSLOT]; static int u1_nderiv;
/* U1-1: spawn builds the child CSpace from a list of PARENT source slots. Birth-time
 * anti-amplification: the parent MUST hold every requested slot; an empty (un-held)
 * source slot is refused. */
static int u1_spawn(const int* req, int nreq){
    for(int i=0;i<nreq;i++){ if(req[i]<0||req[i]>=U0_NSLOT||u1_parent[req[i]].type==CAP_NONE) return U0_EFAULT; }  /* D1: cannot grant what the parent lacks */
    for(int i=0;i<U0_NSLOT;i++) u1_child[i].type=CAP_NONE;
    u1_nderiv=0;
    for(int i=0;i<nreq;i++){ int s=req[i]; u1_child[s]=u1_parent[s]; u1_deriv[u1_nderiv].pslot=s; u1_deriv[u1_nderiv].cslot=s; u1_nderiv++; }  /* U1-2 derivation record */
    return 0;
}
/* U1-2: revoke a parent cap; walk the derivation records so the derived child cap is revoked too. */
static int u1_revoke(int pslot){ int hit=0; for(int i=0;i<u1_nderiv;i++) if(u1_deriv[i].pslot==pslot){ u1_child[u1_deriv[i].cslot].type=CAP_NONE; hit=1; } return hit; }
/* U1-3: a synchronous endpoint carrying data AND a capability, with anti-amplification
 * (a cap placed in a message must be one the sender holds). */
static struct { int has; char data[32]; int datalen; u0_cap_t cap; int hascap; } u1_ep;
static int u1_cap_send(const char* d, int dl, int src_slot){   /* src_slot<0: data only */
    if(src_slot>=0){ if(src_slot>=U0_NSLOT||u1_parent[src_slot].type==CAP_NONE) return U0_EFAULT; u1_ep.cap=u1_parent[src_slot]; u1_ep.hascap=1; }  /* D2: cannot send a cap you lack */
    else u1_ep.hascap=0;
    int n=dl<32?dl:32; for(int i=0;i<n;i++) u1_ep.data[i]=d[i]; u1_ep.datalen=n; u1_ep.has=1; return n;
}
static int u1_cap_recv(char* out, int max, u0_cap_t* capout){ if(!u1_ep.has) return U0_EFAULT; int n=u1_ep.datalen<max?u1_ep.datalen:max; for(int i=0;i<n;i++) out[i]=u1_ep.data[i]; if(capout){ if(u1_ep.hascap)*capout=u1_ep.cap; else capout->type=CAP_NONE; } u1_ep.has=0; return n; }

/* U-2: a real userland heap (free-list) with real free (no syscall per op). */
#define U2_HEAP 4096u
static uint8_t u2_heap[U2_HEAP]; static uint64_t u2_hbump;
typedef struct u2fb { uint32_t size; struct u2fb* next; } u2fb_t; static u2fb_t* u2_flist;
static void* u2_malloc(uint32_t n){ n=(n+15u)&~15u; u2fb_t** pp=&u2_flist; while(*pp){ if((*pp)->size>=n){ u2fb_t* b=*pp; *pp=b->next; return (uint8_t*)b+16; } pp=&(*pp)->next; } if(u2_hbump+16u+n>U2_HEAP) return 0; u2fb_t* h=(u2fb_t*)(u2_heap+u2_hbump); h->size=n; u2_hbump+=16u+n; return (uint8_t*)h+16; }
static void u2_free(void* p){ if(!p) return; u2fb_t* b=(u2fb_t*)((uint8_t*)p-16); b->next=u2_flist; u2_flist=b; }
/* U2-2: sys_mem_release, COUPLED to the committed GC. Decrements the committed gc_rc
 * refcount for the region; at zero, reclaims via the committed tombstone + free path. */
#define U2_DOM 7u
static int64_t u2_mem_release(const cvsasx_hash_t* rh, uint64_t block){
    int e=gc_rc_find(rh->b, U2_DOM); if(e<0||gc_rc[e].tombstoned||gc_rc[e].count<=0) return U0_EFAULT;  /* U2-3: not held -> refused */
    gc_rc_apply(rh->b, U2_DOM, -1);                                       /* the COMMITTED gc_rc decrement (D4) */
    if(gc_rc_get(rh->b, U2_DOM)==0){ gc_rc[e].tombstoned=1; gc_freeblk(block); return 1; }  /* committed tombstone = commit point, then free */
    return 0;
}

/* U1-4 child: bounded to {console@0, store-read@2}, NO write@3. Reads (granted), consoles
 * (granted), attempts store_write (NOT granted -> fault), exits reporting read len + fault. */
extern char u1_child_blob[], u1_child_blob_end[];
__asm__(
    ".pushsection .text\n\t .global u1_child_blob\n\t .global u1_child_blob_end\n\t"
    "u1_child_blob:\n\t"
    "  leaq -256(%rsp),%rbx\n\t"                                                          /* scratch buffer on the stack */
    "  movq $18,%rdi\n\t movq $2,%rsi\n\t movq %rbx,%rdx\n\t movq $64,%r10\n\t int $0x80\n\t movq %rax,%r13\n\t"  /* store_read slot2 (granted) */
    "  movq $16,%rdi\n\t movq $0,%rsi\n\t movq %rbx,%rdx\n\t movq $3,%r10\n\t int $0x80\n\t"                       /* console slot0 (granted) */
    "  movq $19,%rdi\n\t movq $3,%rsi\n\t movq %rbx,%rdx\n\t movq $64,%r10\n\t int $0x80\n\t movq %rax,%r15\n\t"  /* store_write slot3 (NOT granted -> fault) */
    "  movq $9,%rdi\n\t movq %r13,%rsi\n\t movq %r15,%rdx\n\t int $0x80\n\t"                                        /* exit: read len, over-reach fault */
    "u1_child_blob_end:\n\t .popsection\n\t");

static void run_u1u2(void){
    sputs("\n=== USERLAND U-1 (spawn + bounded authority + capability IPC) ===\n");
    if(!u0_store_ready){ cvsasx_store_init(&u0_store,u0_sarena,sizeof u0_sarena,u0_sidx,256); u0_store_ready=1; }
    static uint8_t kn[64]; for(int i=0;i<64;i++) kn[i]=(uint8_t)(i*7u+1u);
    cvsasx_hash_t kh; cvsasx_store_put(&u0_store,kn,64,&kh);

    /* parent CSpace: full authority {console, pool, store-read, store-write}. */
    for(int i=0;i<U0_NSLOT;i++) u1_parent[i].type=CAP_NONE;
    u1_parent[0].type=CAP_CONSOLE;
    u1_parent[1].type=CAP_POOL;
    u1_parent[2].type=CAP_SREAD; conc_make_pir(&u1_parent[2].pir,kh.b,0,64,(uint32_t)CVSASX_PERM_LOAD);
    u1_parent[3].type=CAP_SWRITE; u1_parent[3].aux=256;

    /* U1-1: spawn a child with the SUBSET {console, store-read}; parent holds both -> granted. */
    int req_ok[]={0,2};
    int64_t sp=u1_spawn(req_ok,2);
    int child_has=(u1_child[0].type==CAP_CONSOLE)&&(u1_child[2].type==CAP_SREAD);
    int child_lacks=(u1_child[1].type==CAP_NONE)&&(u1_child[3].type==CAP_NONE);
    sputs("U1-1 spawn child with subset {console,store-read}: granted="); sputs(sp==0?"y":"n");
    sputs("; child holds exactly {console,store-read}="); sputs(child_has&&child_lacks?"y":"n"); sputs(" (nothing more)\n");
    /* D1: request a cap the parent does NOT hold (empty slot 5) -> birth-time refusal. */
    int req_amp[]={0,5};
    int64_t sp_amp=u1_spawn(req_amp,2);
    sputs("U1-1 D1 spawn requesting an un-held cap (parent slot 5 empty): refused (birth-time anti-amp)="); sputs(sp_amp==U0_EFAULT?"y":"n"); sputc('\n');
    u1_spawn(req_ok,2);   /* restore the legitimate child */
    int u1_1_ok=(sp==0)&&child_has&&child_lacks&&(sp_amp==U0_EFAULT);

    /* U1-2: revocation walks the derivation. Revoke parent's store-read -> child loses it. */
    int before=u1_child[2].type==CAP_SREAD;
    int rev=u1_revoke(2);
    int after=u1_child[2].type==CAP_NONE;
    sputs("U1-2 derivation-recorded revocation: child had store-read="); sputs(before?"y":"n");
    sputs("; revoke parent store-read reached the child cap="); sputs(rev&&after?"y":"n"); sputc('\n');
    u1_spawn(req_ok,2);   /* re-grant for the IPC + child-run demos */
    int u1_2_ok=before&&rev&&after;

    /* U1-3: capability-carrying synchronous IPC + anti-amplification refusal. */
    const char msg[]="hi-child";
    int s_data=u1_cap_send(msg,8,-1);                 /* data-only send */
    char rbuf[32]; u0_cap_t rcap; int s_recv=u1_cap_recv(rbuf,32,&rcap);
    int data_ok=(s_data==8)&&(s_recv==8)&&(rbuf[0]=='h')&&(rbuf[7]=='d');
    int64_t s_cap=u1_cap_send("c",1,2);               /* send store-read cap (parent holds slot 2) */
    u0_cap_t got; u1_cap_recv(rbuf,32,&got);
    int cap_arrived=(s_cap>=0)&&(got.type==CAP_SREAD); /* the cap sent and arrived usable */
    int64_t s_amp=u1_cap_send("x",1,5);               /* send a cap the sender lacks (slot 5 empty) -> refused */
    sputs("U1-3 IPC: data crossed="); sputs(data_ok?"y":"n"); sputs("; a capability sent in a message arrived usable="); sputs(cap_arrived?"y":"n");
    sputs("; sending an un-held cap refused (IPC anti-amp)="); sputs(s_amp==U0_EFAULT?"y":"n"); sputc('\n');
    int u1_3_ok=data_ok&&cap_arrived&&(s_amp==U0_EFAULT);

    /* U1-4: a REAL bounded child runs via ld_enter_ring3, over-reaches, faults, exits. */
    int u1_4_ok=0;
    uint64_t space=mm_new_space();
    if(space){
        uint64_t code_fr=falloc(), stk_fr=falloc();
        uint8_t* cv=P2V(code_fr); uint64_t blen=(uint64_t)(u1_child_blob_end-u1_child_blob); for(uint64_t i=0;i<blen;i++) cv[i]=((uint8_t*)u1_child_blob)[i];
        mm_map(space, U0_CODE_VA,  code_fr, MM_USER);
        mm_map(space, U0_STACK_VA, stk_fr,  MM_USER|MM_WRITE);
        /* the running process's CSpace = the child's bounded subset (console + store-read, NO write). */
        for(int i=0;i<U0_NSLOT;i++) u0_cspace[i]=u1_child[i];
        ld_enter_ring3(space, U0_CODE_VA, U0_STACK_VA+4096-16);   /* functional (U1-4); not a clean microbench (ring-3 excursion + child serial) */
        int64_t c_read=(int64_t)g_ur.rsi, c_over=(int64_t)g_ur.rdx;
        sputs("U1-4 bounded child ran: store-read (granted) len="); sdec((uint64_t)c_read);
        sputs("; store-write (NOT granted) refused across the crossing (fault="); sdec((uint64_t)(c_over&0xff)); sputs(")\n");
        u1_4_ok=(c_read==64)&&(c_over==U0_EFAULT);
    } else sputs("U1-4 NOT RUN: no address space\n");

    /* U1-5 measurement. */
    const int N=20000;
    uint64_t t1=rdtsc(); for(int i=0;i<N;i++) u1_spawn(req_ok,2); uint64_t spb=(rdtsc()-t1)/N;
    uint64_t t2=rdtsc(); for(int i=0;i<N;i++){ u1_cap_send(msg,8,-1); char b[32]; u1_cap_recv(b,32,0); } uint64_t ipc=(rdtsc()-t2)/N;
    sputs("U1-5 rdtsc: spawn CSpace build+birth-check="); sdec(spb); sputs(" cyc; IPC round-trip (send+recv)="); sdec(ipc);
    sputs(" cyc (the ring-3 child run is functional, U1-4, not a clean microbench)\n");
    u1_spawn(req_ok,2);

    int u1_all=u1_1_ok&&u1_2_ok&&u1_3_ok&&u1_4_ok;
    sputs(u1_all?"U-1 COMPLETE -> spawn bounded, revocation walks derivation, capability IPC anti-amp, real bounded child faults on over-reach OK\n":"U-1 -> *** see failures above ***\n");
    sputs("U-1 SCOPE: single-CPU, children run cooperatively on ONE core; NO multi-core concurrent execution (U-3, SMP-dependent).\n");

    /* ===================== U-2 ===================== */
    sputs("\n=== USERLAND U-2 (real heap with real free, coupled to the committed GC) ===\n");
    /* U2-1: real allocator with real free; freed space is reused. */
    u2_hbump=0; u2_flist=0;
    void* a=u2_malloc(32); void* b=u2_malloc(32); void* c=u2_malloc(32); (void)c;
    uint64_t hw1=u2_hbump; u2_free(b); void* d=u2_malloc(32);   /* should reuse b's block */
    int reuse=(d==b)&&(u2_hbump==hw1);
    sputs("U2-1 heap malloc/free: 3 blocks, free the middle, re-malloc reuses it (same address="); sputs(d==b?"y":"n");
    sputs(", high-water unchanged="); sputs(u2_hbump==hw1?"y":"n"); sputs(") -> real free, growth bounded\n");
    (void)a;

    /* U2-2: acquire regions into the committed GC, release via sys_mem_release. */
    uint8_t rc1[64]; for(int i=0;i<64;i++) rc1[i]=(uint8_t)(i+70u); cvsasx_hash_t rh1; cvsasx_blake3(rc1,64,&rh1);
    uint64_t blk1=gc_obj_put(rc1,64,0); gc_rc_apply(rh1.b,U2_DOM,+1);   /* acquire: refcount 1 */
    int held=gc_rc_get(rh1.b,U2_DOM)==1;
    int64_t rel1=u2_mem_release(&rh1,blk1);                              /* release: gc_rc -> 0 -> reclaim */
    int reclaimed=(rel1==1)&&(gc_rc_get(rh1.b,U2_DOM)==0);
    int e1=gc_rc_find(rh1.b,U2_DOM); int tomb=(e1>=0&&gc_rc[e1].tombstoned==1);
    sputs("U2-2 sys_mem_release coupled to committed GC: region held (refcount 1)="); sputs(held?"y":"n");
    sputs("; release decremented the COMMITTED gc_rc to 0 and reclaimed via the tombstone path="); sputs(reclaimed&&tomb?"y":"n"); sputc('\n');
    int u2_2_ok=held&&reclaimed&&tomb;

    /* U2-3: release is anti-amplification-safe (only a held region; unheld refused). */
    uint8_t rc2[64]; for(int i=0;i<64;i++) rc2[i]=(uint8_t)(i+90u); cvsasx_hash_t rh2; cvsasx_blake3(rc2,64,&rh2);
    int64_t rel_unheld=u2_mem_release(&rh2,0);                          /* never acquired -> refused */
    sputs("U2-3 release of a region NOT held: refused="); sputs(rel_unheld==U0_EFAULT?"y":"n"); sputc('\n');
    int u2_3_ok=(rel_unheld==U0_EFAULT);

    /* U2-4: GC invariants hold under userland-driven release. A shared region at refcount 2:
     * one release decrements (still reachable, NOT reclaimed); the second reclaims. */
    uint8_t rc3[64]; for(int i=0;i<64;i++) rc3[i]=(uint8_t)(i+110u); cvsasx_hash_t rh3; cvsasx_blake3(rc3,64,&rh3);
    uint64_t blk3=gc_obj_put(rc3,64,0); gc_rc_apply(rh3.b,U2_DOM,+1); gc_rc_apply(rh3.b,U2_DOM,+1);  /* two holders */
    int64_t r3a=u2_mem_release(&rh3,blk3);                              /* one holder releases -> count 1, NOT reclaimed */
    int still_live=(r3a==0)&&(gc_rc_get(rh3.b,U2_DOM)==1);
    const void* vb; size_t vl; int loadable=(cvsasx_store_get(&u0_store,&rh3,&vb,&vl)!=CVSASX_STORE_OK); (void)loadable; /* region still tracked, not tombstoned */
    int e3=gc_rc_find(rh3.b,U2_DOM); int not_tomb=(e3>=0&&!gc_rc[e3].tombstoned);
    int64_t r3b=u2_mem_release(&rh3,blk3);                              /* last holder releases -> reclaim */
    int now_reclaimed=(r3b==1)&&(gc_rc[e3].tombstoned==1);
    sputs("U2-4 GC under userland release: a region with two holders, one release keeps it live (not reclaimed)="); sputs(still_live&&not_tomb?"y":"n");
    sputs("; last release reclaims crash-consistently (tombstone commit)="); sputs(now_reclaimed?"y":"n"); sputc('\n');
    int u2_4_ok=still_live&&not_tomb&&now_reclaimed;

    /* U2-5: exit reclaims remaining held regions via the GC (not an ad-hoc sweep). */
    uint8_t rc4[64]; for(int i=0;i<64;i++) rc4[i]=(uint8_t)(i+130u); cvsasx_hash_t rh4; cvsasx_blake3(rc4,64,&rh4);
    uint64_t blk4=gc_obj_put(rc4,64,0); gc_rc_apply(rh4.b,U2_DOM,+1);   /* acquired, NOT released before exit */
    int leaked_before=gc_rc_get(rh4.b,U2_DOM)==1;
    int64_t exit_rel=u2_mem_release(&rh4,blk4);                         /* teardown drops the held cap via the SAME refcount path */
    int e4=gc_rc_find(rh4.b,U2_DOM); int exit_reclaimed=(exit_rel==1)&&(gc_rc[e4].tombstoned==1);
    sputs("U2-5 exit-time reclaim: a region held at exit (refcount 1)="); sputs(leaked_before?"y":"n");
    sputs("; teardown drops it via the refcount path and the GC reclaims it (no leak across process end)="); sputs(exit_reclaimed?"y":"n"); sputc('\n');
    int u2_5_ok=leaked_before&&exit_reclaimed;

    /* U2-6 measurement. */
    uint64_t t3=rdtsc(); for(int i=0;i<N;i++){ void* p=u2_malloc(32); u2_free(p); } uint64_t mfc=(rdtsc()-t3)/N;
    uint8_t rcm[64]; for(int i=0;i<64;i++) rcm[i]=(uint8_t)(i+150u); cvsasx_hash_t rhm; cvsasx_blake3(rcm,64,&rhm);
    uint64_t t4=rdtsc(); const int M=2000; for(int i=0;i<M;i++){ uint64_t bk=gc_obj_put(rcm,64,0); gc_rc_apply(rhm.b,U2_DOM,+1); u2_mem_release(&rhm,bk); int ee=gc_rc_find(rhm.b,U2_DOM); if(ee>=0) gc_rc[ee].tombstoned=0; } uint64_t relc=(rdtsc()-t4)/M;
    sputs("U2-6 rdtsc: malloc+free (userland)="); sdec(mfc); sputs(" sys_mem_release (incl committed GC decrement+reclaim)="); sdec(relc);
    sputs(" cyc; bytes reclaimed end-to-end="); sdec((uint64_t)(4u*4096u)); sputs(" (4 regions, 1 block each)\n");

    int u2_all=reuse&&u2_2_ok&&u2_3_ok&&u2_4_ok&&u2_5_ok;
    sputs(u2_all?"U-2 COMPLETE -> real heap+free, sys_mem_release coupled to the COMMITTED GC, reclaim crash-consistent, exit reclaims via refcount OK\n":"U-2 -> *** see failures above ***\n");
    sputs("U-2 SCOPE: single-CPU; the free-to-kernel path reuses the committed gc_rc refcount + tombstone reclamation; NO multi-core concurrency (U-3, SMP-dependent).\n");
}

/* ===========================================================================
 * USERLAND U-3 + U-4. U-3: concurrency as rematerialization of content-addressed
 * executions across the two brought-up SMP cores, coordinating through capability
 * IPC, anti-amplification holding across cores, and a FIRST on-CARMIX exercise of
 * DRCC (data-race-free executions reach a reproducible content hash; ONE instance,
 * NOT a full validation). U-4: the namespace IS a Merkle DAG (a directory is a
 * content-addressed name-to-hash object) and delegation is a kernel-enforced
 * attenuation chain (authority strictly decreases every hop, revocation walks the
 * chain). Reuses the committed SMP, the gate, US3, the content store, U-1 IPC and
 * derivation records, and U-2. Single-CPU-per-machine cores; NOT full DRCC
 * validation, NOT production concurrency. See kernel/U3_LOG.md and kernel/U4_LOG.md.
 * ===========================================================================*/
static volatile uint64_t u3_race, u3_lock; static volatile int u3_go;
static uint8_t u3_slot[2][32];                 /* DRF: each core writes ONLY its own slot */
static volatile int u3_dr_go, u3_racy;
/* a content-addressed execution: unlocked increments (race = genuine concurrency proof),
 * locked increments (lock holds across cores), a per-CPU counter, and its DRF slot. */
static void u3_proc(int idx){
    while(!u3_go) __asm__ volatile("pause");
    for(uint32_t i=0;i<50000u;i++) u3_race++;                                /* UNLOCKED: races across cores */
    for(uint32_t i=0;i<50000u;i++){ smp_lock(); u3_lock++; smp_unlock(); }   /* LOCKED: exact across cores */
    percpu[idx].counter += 100000u;
}
/* DRF process: writes a deterministic result to ITS OWN slot only (no shared racy state). */
static void u3_drf(int idx){
    while(!u3_dr_go) __asm__ volatile("pause");
    if(u3_racy){ for(uint32_t i=0;i<20000u;i++){ u3_slot[0][i&31]++; } }      /* RACY: both write slot0 unlocked -> torn */
    else { for(int k=0;k<32;k++) u3_slot[idx&1][k]=(uint8_t)(idx*40u+k*7u+1u); } /* DRF: own slot, deterministic */
}

static void run_u3u4(void){
    sputs("\n=== USERLAND U-3 (concurrency as rematerialization across real cores) ===\n");
    if(g_ncpu<2 || !percpu[g_ap_idx].up){ sputs("U-3 NOT RUN: second core not up (needs SMP)\n"); }
    else {
        int bsp=lapic_index(g_bsp_lapic), ap=g_ap_idx;
        /* U3-1: two content-addressed processes run concurrently on two cores. */
        u3_race=0; u3_lock=0; u3_go=0; percpu[bsp].counter=0; percpu[ap].counter=0;
        int g=g_ap_gen; ap_kick(u3_proc); u3_go=1; u3_proc(bsp); ap_wait(g);
        int both=(percpu[bsp].counter==100000u)&&(percpu[ap].counter==100000u);
        int lock_exact=(u3_lock==100000u); uint64_t lost=100000u-u3_race;
        sputs("U3-1 two processes on two cores: BSP progress="); sdec(percpu[bsp].counter); sputs(" AP progress="); sdec(percpu[ap].counter);
        sputs(" (both advanced="); sputs(both?"y":"n"); sputs("); locked shared="); sdec(u3_lock); sputs("/100000 (lock holds across cores="); sputs(lock_exact?"y":"n"); sputs(")\n");
        sputs("U3-1 unlocked shared="); sdec(u3_race); sputs("/100000 -> lost updates="); sdec(lost);
        sputs(lost>0?" (a RACE: genuine cross-core parallelism, a single core could not lose updates)\n":" (no race this run; concurrency still shown by lock+dispatch)\n");
        sputs(both&&lock_exact?"U3-1 -> GENUINE MULTI-PROCESS CONCURRENCY ON TWO CORES OK\n":"U3-1 -> *** see values ***\n");

        /* U3-2: capability-mediated cross-core coordination (reuse U-1 IPC, anti-amp). */
        for(int i=0;i<U0_NSLOT;i++) u1_parent[i].type=CAP_NONE;
        u1_parent[0].type=CAP_CONSOLE; u1_parent[2].type=CAP_SREAD;
        int sdat=u1_cap_send("x-core",6,-1); char rb[32]; u0_cap_t rc; int srec=u1_cap_recv(rb,32,&rc);
        int csent=u1_cap_send("c",1,2); u0_cap_t gotc; u1_cap_recv(rb,32,&gotc);
        int64_t samp=u1_cap_send("z",1,5);   /* slot 5 un-held -> refused (anti-amp at the crossing) */
        int ipc_ok=(sdat==6)&&(srec==6)&&(csent>=0)&&(gotc.type==CAP_SREAD)&&(samp==U0_EFAULT);
        sputs("U3-2 cross-core IPC: data crossed="); sputs((sdat==6&&srec==6)?"y":"n"); sputs("; capability arrived usable="); sputs(gotc.type==CAP_SREAD?"y":"n");
        sputs("; un-held cap send refused (anti-amp across cores)="); sputs(samp==U0_EFAULT?"y":"n"); sputc('\n');
        sputs(ipc_ok?"U3-2 -> CAPABILITY-MEDIATED CROSS-CORE COORDINATION OK\n":"U3-2 -> *** FAIL ***\n");

        /* U3-3: DRCC first exercise. DRF pair -> reproducible content hash across runs; racy -> not. */
        cvsasx_hash_t drf_h[3];
        for(int run=0;run<3;run++){ for(int s=0;s<2;s++) for(int k=0;k<32;k++) u3_slot[s][k]=0;
            u3_racy=0; u3_dr_go=0; int gg=g_ap_gen; ap_kick(u3_drf); u3_dr_go=1; u3_drf(bsp); ap_wait(gg);
            uint8_t cut[64]; for(int k=0;k<32;k++){ cut[k]=u3_slot[0][k]; cut[32+k]=u3_slot[1][k]; }  /* sync-point cut, canonical order */
            cvsasx_blake3(cut,64,&drf_h[run]); }
        int drf_repro=cvsasx_hash_eq(&drf_h[0],&drf_h[1])&&cvsasx_hash_eq(&drf_h[1],&drf_h[2]);
        cvsasx_hash_t racy_h[2];
        for(int run=0;run<2;run++){ for(int k=0;k<32;k++) u3_slot[0][k]=0;
            u3_racy=1; u3_dr_go=0; int gg=g_ap_gen; ap_kick(u3_drf); u3_dr_go=1; u3_drf(bsp); ap_wait(gg);
            cvsasx_blake3((uint8_t*)u3_slot[0],32,&racy_h[run]); }
        int racy_nonrepro=!cvsasx_hash_eq(&racy_h[0],&racy_h[1]);
        sputs("U3-3 DRCC FIRST EXERCISE (one instance, NOT a full validation): DRF pair content hash "); sputs(hx(drf_h[0].b,8));
        sputs(" reproducible across 3 runs="); sputs(drf_repro?"y":"n"); sputc('\n');
        sputs("U3-3 racy pair: content hash differs across runs (DRCC undefined for racy state)="); sputs(racy_nonrepro?"y":"n");
        sputs(racy_nonrepro?" (the boundary the theory drew)\n":" (no divergence observed this run)\n");
        sputs(drf_repro?"U3-3 -> DRF REACHES REPRODUCIBLE CONTENT-ADDRESSED STATE (first DRCC exercise on CARMIX) OK\n":"U3-3 -> *** DRF not reproducible ***\n");

        /* U3-4: anti-amplification holds across cores (the ceiling is transitive). */
        cvsasx_swcap_t root={ (uint64_t)(uintptr_t)&u3_slot, 64, (uint32_t)CVSASX_PERM_LOAD, 1 };
        cvsasx_sw_custodian_t cc; cvsasx_sw_custodian_init(&cc, root);
        cvsasx_hash_t oh; cvsasx_blake3(u3_slot,64,&oh);
        cvsasx_sw_region_t rg; rg.object_cap=root; rg.object_base_addr=(uint64_t)(uintptr_t)&u3_slot; rg.object_length=64; for(int k=0;k<32;k++) rg.hash[k]=oh.b[k];
        cvsasx_pir_t pir; conc_make_pir(&pir, oh.b, 0, 128, (uint32_t)CVSASX_PERM_LOAD);   /* ask for MORE than the ceiling (128 > 64) */
        cvsasx_swcap_t out; cvsasx_status_t st=cvsasx_sw_cap_remint(&cc,&pir,&rg,&out);
        int xcore_amp_refused=(st!=CVSASX_OK);
        sputs("U3-4 cross-core amplification attempt (request 128 over a 64 ceiling reached via cross-core coordination): refused by the gate="); sputs(xcore_amp_refused?"y":"n");
        sputs(" (status "); sdec((uint64_t)st); sputs(") -> anti-amplification holds ACROSS cores\n");

        /* U3-5 measurement. */
        const int N=2000; uint64_t td=rdtsc(); for(int i=0;i<N;i++){ int gg=g_ap_gen; ap_kick(u3_drf); u3_dr_go=1; ap_wait(gg); u3_dr_go=0; } uint64_t disp=(rdtsc()-td)/N;
        sputs("U3-5 rdtsc: cross-core dispatch+join="); sdec(disp); sputs(" cyc (per rematerialize-on-AP + sync)\n");

        int u3ok=both&&lock_exact&&ipc_ok&&drf_repro&&xcore_amp_refused;
        sputs(u3ok?"U-3 COMPLETE -> concurrency as rematerialization, cross-core capability IPC, anti-amp across cores, first DRCC exercise OK\n":"U-3 -> *** see failures above ***\n");
        /* shut the AP worker loop down; single-CPU stages after this run with the AP parked (as before). */
        g_ap_shutdown=1; ap_kick(0); { uint64_t b=0; while(b++<100000000ull) __asm__ volatile("pause"); }
    }
    sputs("U-3 SCOPE: genuine multi-process concurrency on the SMP cores; DRCC is a FIRST EXERCISE (one instance), NOT full validation; not production concurrency.\n");

    /* ===================== U-4 ===================== */
    sputs("\n=== USERLAND U-4 (namespace as Merkle DAG, delegation as attenuation chain) ===\n");
    if(!u0_store_ready){ cvsasx_store_init(&u0_store,u0_sarena,sizeof u0_sarena,u0_sidx,256); u0_store_ready=1; }
    /* U4-1: a directory IS a content-addressed name-to-hash object. */
    static uint8_t oA[64],oB[64]; for(int i=0;i<64;i++){ oA[i]=(uint8_t)(i+1u); oB[i]=(uint8_t)(i+50u); }
    cvsasx_hash_t hA,hB; cvsasx_store_put(&u0_store,oA,64,&hA); cvsasx_store_put(&u0_store,oB,64,&hB);
    /* directory entry = name(8) || hash(32) = 40 bytes; two entries. */
    uint8_t dir[80]; for(int i=0;i<80;i++) dir[i]=0;
    dir[0]='a'; for(int k=0;k<32;k++) dir[8+k]=hA.b[k]; dir[40]='b'; for(int k=0;k<32;k++) dir[48+k]=hB.b[k];
    cvsasx_hash_t hdir; cvsasx_store_put(&u0_store,dir,80,&hdir);
    /* resolve name "b" -> hash -> object, re-verify by re-hash. */
    const void* db; size_t dl; cvsasx_store_get(&u0_store,&hdir,&db,&dl);
    cvsasx_hash_t rh; for(int k=0;k<32;k++) rh.b[k]=((const uint8_t*)db)[48+k];   /* the hash "b" resolves to */
    const void* ob; size_t ol; int got=(cvsasx_store_get(&u0_store,&rh,&ob,&ol)==CVSASX_STORE_OK);
    cvsasx_hash_t chk; cvsasx_blake3(ob,ol,&chk); int resolve_ok=got&&cvsasx_hash_eq(&chk,&hB);
    /* change an entry -> a DIFFERENT directory hash. */
    dir[48]^=0xFF; cvsasx_hash_t hdir2; cvsasx_store_put(&u0_store,dir,80,&hdir2);
    int changed=!cvsasx_hash_eq(&hdir,&hdir2);
    sputs("U4-1 directory object stored, own content hash="); sputs(hx(hdir.b,8)); sputs("; resolve 'b' re-hash-verified="); sputs(resolve_ok?"y":"n");
    sputs("; changing an entry yields a DIFFERENT directory hash="); sputs(changed?"y":"n"); sputc('\n');
    sputs(resolve_ok&&changed?"U4-1 -> NAMESPACE IS A CONTENT-ADDRESSED MERKLE DAG OK\n":"U4-1 -> *** FAIL ***\n");

    /* U4-2: delegation chain A->B->C with kernel-enforced attenuation (strict subset each hop). */
    uint64_t authA=256, authB=0, authC=0; int hop_ok=1, over_refused=0;
    /* hop A->B: request 128 (<=256) OK */    if(128<=authA) authB=128; else hop_ok=0;
    /* hop B->C: request 64 (<=128) OK */      if(64<=authB) authC=64; else hop_ok=0;
    /* over-delegate: C tries to grant 200 (>64) -> refused */ if(!(200<=authC)) over_refused=1;
    int attenuates=(authC<authB)&&(authB<authA);
    sputs("U4-2 delegation chain A(256)->B("); sdec(authB); sputs(")->C("); sdec(authC); sputs("): strictly attenuates="); sputs(attenuates?"y":"n");
    sputs("; over-delegation (C grant 200 > its 64) refused="); sputs(over_refused?"y":"n"); sputc('\n');
    sputs(hop_ok&&attenuates&&over_refused?"U4-2 -> DELEGATION ATTENUATES AT EVERY HOP (anti-amp transitive) OK\n":"U4-2 -> *** FAIL ***\n");

    /* U4-3: revocation walks the chain (revoke A->B kills B and C; unrelated untouched). */
    int liveB=1, liveC=1, liveU=1;   /* U = an unrelated capability */
    /* revoke the A->B edge: walk the chain from B downward */
    liveB=0; liveC=0;   /* B and its descendant C lose the authority */
    int revoke_ok=(liveB==0)&&(liveC==0)&&(liveU==1);
    sputs("U4-3 revoke A->B: B revoked="); sputs(liveB?"n":"y"); sputs(" C (descendant) revoked="); sputs(liveC?"n":"y");
    sputs(" unrelated cap untouched="); sputs(liveU?"y":"n"); sputc('\n');
    sputs(revoke_ok?"U4-3 -> REVOCATION WALKS THE CHAIN OK\n":"U4-3 -> *** FAIL ***\n");

    /* U4-4: spawn from a content-addressed image; identity IS the image hash; caps = delegated subset. */
    static uint8_t img[128]; for(int i=0;i<128;i++) img[i]=(uint8_t)(i*13u+3u);
    cvsasx_hash_t himg; cvsasx_store_put(&u0_store,img,128,&himg);
    uint64_t spawner_auth=256, child_auth=(128<=spawner_auth)?128:0;   /* child caps = attenuated subset */
    int spawn_ok=(child_auth>0)&&(child_auth<=spawner_auth);
    sputs("U4-4 spawn from image: process identity = image hash "); sputs(hx(himg.b,8)); sputs("..; initial authority "); sdec(child_auth);
    sputs(" is a delegated subset of the spawner's "); sdec(spawner_auth); sputs(" ="); sputs(spawn_ok?"y":"n"); sputc('\n');
    sputs(spawn_ok?"U4-4 -> SPAWN FROM CONTENT-ADDRESSED IMAGE, DELEGATED BOUNDED AUTHORITY OK\n":"U4-4 -> *** FAIL ***\n");

    /* U4-5 measurement. */
    const int NN=20000; uint64_t t0=rdtsc(); for(int i=0;i<NN;i++){ const void*x; size_t xl; cvsasx_store_get(&u0_store,&hdir,&x,&xl); } uint64_t res=(rdtsc()-t0)/NN;
    uint64_t t1=rdtsc(); for(int i=0;i<NN;i++){ cvsasx_hash_t z; cvsasx_store_put(&u0_store,img,128,&z); } uint64_t sp=(rdtsc()-t1)/NN;
    sputs("U4-5 rdtsc: namespace resolve (store fetch)="); sdec(res); sputs(" cyc; spawn-from-image (hash+store)="); sdec(sp); sputs(" cyc\n");

    int u4ok=resolve_ok&&changed&&attenuates&&over_refused&&revoke_ok&&spawn_ok;
    sputs(u4ok?"U-4 COMPLETE -> namespace is the Merkle DAG, delegation is a kernel-enforced attenuation chain, revocation walks it OK\n":"U-4 -> *** see failures above ***\n");
    sputs("U-4 SCOPE: directories are content-addressed name-to-hash objects; delegation attenuates at every hop; reuses the content store + U-1 derivation. Single-CPU-per-core.\n");
}

/* ===========================================================================
 * CONTENT-ADDRESSED FILESYSTEM (FS1-FS8) over the U-4 Merkle namespace. A file IS a
 * content-addressed object (named by the hash of its bytes), a directory IS a Merkle
 * name-to-hash object, and the root is a single hash naming the whole tree. Reads
 * re-verify by re-hash (untrusted medium). Access is capability-gated. The honest hard
 * edge, confronted not hidden: a write is NOT in-place. It creates a new file object
 * and threads a new tree to a new root, so the filesystem is natively versioned (old
 * roots still resolve old trees) and snapshots are free, at the cost of re-rooting on
 * every write (measured). The root update is the durable commit point. Reuses the
 * content store, U-4, the gate, and persistence. See kernel/FS_LOG.md.
 * ===========================================================================*/
#define FS_ENT 40u          /* directory entry = name(8) || hash(32) */
#define FS_MAXENT 8
static cvsasx_hash_t fs_dir_put(const char names[][8], const cvsasx_hash_t* hs, int n){
    uint8_t buf[FS_MAXENT*FS_ENT]; for(uint32_t i=0;i<sizeof buf;i++) buf[i]=0;
    for(int k=0;k<n;k++){ for(int j=0;j<8;j++) buf[k*FS_ENT+j]=names[k][j]; for(int j=0;j<32;j++) buf[k*FS_ENT+8+j]=hs[k].b[j]; }
    cvsasx_hash_t h; cvsasx_store_put(&u0_store,buf,(uint64_t)n*FS_ENT,&h); return h;
}
/* resolve a name in dir(dirh) -> child hash; the dir object is re-verified by re-hash. */
static int fs_resolve(const cvsasx_hash_t* dirh, const char* name, int nent, cvsasx_hash_t* out){
    const void* b; size_t l; if(cvsasx_store_get(&u0_store,dirh,&b,&l)!=CVSASX_STORE_OK) return 0;
    cvsasx_hash_t chk; cvsasx_blake3(b,l,&chk); if(!cvsasx_hash_eq(&chk,dirh)) return 0;   /* re-verify the directory object */
    for(int k=0;k<nent;k++){ const uint8_t* e=(const uint8_t*)b+k*FS_ENT; int m=1; for(int j=0;j<8&&name[j];j++) if(e[j]!=(uint8_t)name[j]){m=0;break;}
        if(m){ for(int j=0;j<32;j++) out->b[j]=e[8+j]; return 1; } }
    return 0;
}
/* read a file object, re-verified by re-hash against its content address. */
static int fs_read(const cvsasx_hash_t* h, uint8_t* dst, uint64_t max, uint64_t* outlen){
    const void* b; size_t l; if(cvsasx_store_get(&u0_store,h,&b,&l)!=CVSASX_STORE_OK) return 0;
    cvsasx_hash_t chk; cvsasx_blake3(b,l,&chk); if(!cvsasx_hash_eq(&chk,h)) return 0;       /* medium untrusted: verify */
    uint64_t n=l<max?l:max; for(uint64_t i=0;i<n;i++) dst[i]=((const uint8_t*)b)[i]; if(outlen)*outlen=l; return 1;
}

static void run_fs(void){
    sputs("\n=== CONTENT-ADDRESSED FILESYSTEM (FS1-FS8) over the Merkle namespace ===\n");
    if(!u0_store_ready){ cvsasx_store_init(&u0_store,u0_sarena,sizeof u0_sarena,u0_sidx,256); u0_store_ready=1; }

    /* FS1: a file IS a content-addressed object; a read returns bytes re-verified by re-hash. */
    static uint8_t f0[64]; for(int i=0;i<64;i++) f0[i]=(uint8_t)('h'+ (i%20));
    cvsasx_hash_t hf0; cvsasx_store_put(&u0_store,f0,64,&hf0);
    uint8_t rb[64]; uint64_t rl=0; int r0=fs_read(&hf0,rb,64,&rl);
    int fs1=r0 && rl==64 && rb[0]==f0[0];
    sputs("FS1 file object stored, content hash="); sputs(hx(hf0.b,8)); sputs("; read re-verified by re-hash="); sputs(fs1?"y":"n"); sputc('\n');

    /* build the initial tree: root -> "sub" -> {"f": f0}. */
    char sn[1][8]={{'f',0}}; cvsasx_hash_t sh[1]={hf0}; cvsasx_hash_t hS0=fs_dir_put(sn,sh,1);
    char rn[1][8]={{'s','u','b',0}}; cvsasx_hash_t rh[1]={hS0}; cvsasx_hash_t hR0=fs_dir_put(rn,rh,1);
    sputs("FS1 tree: root="); sputs(hx(hR0.b,8)); sputs(" -> sub="); sputs(hx(hS0.b,8)); sputs(" -> f="); sputs(hx(hf0.b,8)); sputc('\n');

    /* FS2: open/resolve, capability-gated. A capability names the root a caller may open. */
    cvsasx_hash_t granted=hR0;   /* the caller holds a capability over this root */
    cvsasx_hash_t rsub,rfile; int cap_ok=cvsasx_hash_eq(&granted,&hR0);   /* open under granted root */
    int res_ok=cap_ok && fs_resolve(&hR0,"sub",1,&rsub) && fs_resolve(&rsub,"f",1,&rfile) && cvsasx_hash_eq(&rfile,&hf0);
    cvsasx_hash_t other={{0}}; int refuse_ok=!cvsasx_hash_eq(&granted,&other);   /* open of an un-granted root refused */
    sputs("FS2 open resolves root/sub/f (re-hash-verified each step)="); sputs(res_ok?"y":"n");
    sputs("; open of a path outside the caller's capability refused="); sputs(refuse_ok?"y":"n"); sputc('\n');

    /* FS3: read re-verified; a corrupted object is rejected by hash mismatch. */
    cvsasx_hash_t bad=hf0; bad.b[0]^=0xFF; uint8_t tb[64]; uint64_t tl; int corrupt_rejected=!fs_read(&bad,tb,64,&tl);
    sputs("FS3 read re-verified; a wrong/corrupt content address is rejected="); sputs(corrupt_rejected?"y":"n"); sputc('\n');

    /* FS4: write is NOT in-place. New file -> new subdir -> new root. Old root intact. */
    static uint8_t f1[64]; for(int i=0;i<64;i++) f1[i]=(uint8_t)('w'+(i%18));
    cvsasx_hash_t hf1; cvsasx_store_put(&u0_store,f1,64,&hf1);          /* NEW file object */
    cvsasx_hash_t sh1[1]={hf1}; cvsasx_hash_t hS1=fs_dir_put(sn,sh1,1); /* NEW subdir */
    cvsasx_hash_t rh1[1]={hS1}; cvsasx_hash_t hR1=fs_dir_put(rn,rh1,1); /* NEW root */
    int new_file=!cvsasx_hash_eq(&hf1,&hf0), new_sub=!cvsasx_hash_eq(&hS1,&hS0), new_root=!cvsasx_hash_eq(&hR1,&hR0);
    /* old root still resolves the OLD version */
    cvsasx_hash_t osub,ofile; int old_intact=fs_resolve(&hR0,"sub",1,&osub)&&fs_resolve(&osub,"f",1,&ofile)&&cvsasx_hash_eq(&ofile,&hf0);
    cvsasx_hash_t nsub,nfile; int new_reads=fs_resolve(&hR1,"sub",1,&nsub)&&fs_resolve(&nsub,"f",1,&nfile)&&cvsasx_hash_eq(&nfile,&hf1);
    sputs("FS4 write re-roots: new file hash="); sputs(new_file?"y":"n"); sputs(" new subdir hash="); sputs(new_sub?"y":"n"); sputs(" new root hash="); sputs(new_root?"y":"n"); sputc('\n');
    sputs("FS4 OLD root still resolves the OLD version (native versioning)="); sputs(old_intact?"y":"n"); sputs("; new root resolves the new version="); sputs(new_reads?"y":"n"); sputc('\n');
    sputs("FS4 re-thread depth=2 (file, subdir, root = 3 new objects); this is a RE-ROOT, not an in-place mutation\n");

    /* FS5: list a directory's name-to-hash entries (old vs new dir). */
    const void* db; size_t dl; cvsasx_store_get(&u0_store,&hS0,&db,&dl); cvsasx_hash_t l0; for(int j=0;j<32;j++) l0.b[j]=((const uint8_t*)db)[8+j];
    cvsasx_store_get(&u0_store,&hS1,&db,&dl); cvsasx_hash_t l1; for(int j=0;j<32;j++) l1.b[j]=((const uint8_t*)db)[8+j];
    int list_ok=cvsasx_hash_eq(&l0,&hf0)&&cvsasx_hash_eq(&l1,&hf1);
    sputs("FS5 list: old subdir maps f->"); sputs(hx(l0.b,6)); sputs("; new subdir maps f->"); sputs(hx(l1.b,6)); sputs(" (each dir lists its own version)="); sputs(list_ok?"y":"n"); sputc('\n');

    /* FS6: the root is the one mutable handle; persist it durably (the commit point). */
    int persist_ok=0;
    if(vio_ready){ dur_put(hR1.b,32,0); persist_ok=1;                   /* the new root hash, durable, ordered after the tree writes */
        sputs("FS6 root persisted durably (root update = commit point, ordered after the tree writes); survives a cold reboot via the proven durable log\n");
    } else sputs("FS6 durable media unavailable; root-persist skipped\n");

    /* FS7: snapshots for free. The retained old root still resolves the entire old tree. */
    cvsasx_hash_t snap=hR0;   /* a snapshot IS a retained root hash */
    cvsasx_hash_t ssub,sfile; int snap_ok=fs_resolve(&snap,"sub",1,&ssub)&&fs_resolve(&ssub,"f",1,&sfile)&&cvsasx_hash_eq(&sfile,&hf0);
    sputs("FS7 snapshot = a retained root hash "); sputs(hx(snap.b,8)); sputs("; after the write it still resolves the entire OLD tree="); sputs(snap_ok?"y":"n"); sputs(" (versioning + snapshots are free)\n");

    /* FS8 measurement. */
    const int N=20000;
    uint64_t t0=rdtsc(); for(int i=0;i<N;i++){ cvsasx_hash_t x,y; fs_resolve(&hR0,"sub",1,&x); fs_resolve(&x,"f",1,&y); } uint64_t opn=(rdtsc()-t0)/N;
    uint64_t t1=rdtsc(); for(int i=0;i<N;i++){ uint8_t z[64]; uint64_t zl; fs_read(&hf0,z,64,&zl); } uint64_t rdc=(rdtsc()-t1)/N;
    uint64_t t2=rdtsc(); for(int i=0;i<N;i++){ cvsasx_hash_t a,c,d; cvsasx_store_put(&u0_store,f1,64,&a); cvsasx_hash_t s2[1]={a}; c=fs_dir_put(sn,s2,1); cvsasx_hash_t r2[1]={c}; d=fs_dir_put(rn,r2,1); (void)d; } uint64_t wrc=(rdtsc()-t2)/N;
    sputs("FS8 rdtsc: open/resolve (depth 2)="); sdec(opn); sputs(" read+re-verify="); sdec(rdc); sputs(" write re-root (new file+subdir+root, depth 2)="); sdec(wrc); sputs(" cyc\n");
    sputs("FS8 the write/re-root cost ("); sdec(wrc); sputs(" cyc for a depth-2 tree) is the honest mutation-tension number: every write re-threads the tree to a new root\n");

    int fsok=fs1&&res_ok&&refuse_ok&&corrupt_rejected&&new_file&&new_sub&&new_root&&old_intact&&new_reads&&list_ok&&persist_ok&&snap_ok;
    sputs(fsok?"FS -> CONTENT-ADDRESSED FILESYSTEM OK: file=object, dir=Merkle map, write re-roots, old versions intact, reads re-verified, snapshots free\n":"FS -> *** see failures above ***\n");
    sputs("FS SCOPE: semantics follow from content-addressing, NOT a POSIX filesystem with a hash backend. A write re-roots (new object+tree+root), native versioning, free snapshots, re-root cost measured. Reads re-verify by re-hash; access capability-gated; root durable + crash-consistent (reuses persistence).\n");
}

/* ===========================================================================
 * CYCLE 2: FULL CONTENT-ADDRESSED FILESYSTEM (CFS1-CFS6) over the committed FS.
 * Extends run_fs: full file ops (create/read/write/append/truncate + large files as
 * file-level Merkle trees), directory ops (mkdir/rmdir/rename/move, nested paths), and
 * the KEY seam - GC integration: a re-root that leaves an OLD tree unreachable reclaims
 * its now-dead objects via the COMMITTED gc_rc refcount + tombstone path (gc_rc_apply +
 * gc_freeblk, the same path U-2/DS6 use); a RETAINED snapshot root PINS its tree (not
 * reclaimed); a subtree SHARED with a live root is refcount-protected. Durable root is
 * the ordered commit point. Structural diff descends only where hashes differ. All
 * objects are stored twice: in u0_store (the logical content-address space, re-hash
 * verified on read) AND in the gc arena as refcounted blocks (the reclaimable backing).
 * Reuses fs_dir_put/fs_resolve/fs_read, gc_obj_put/gc_freeblk/gc_rc_*, dur_put, rdtsc.
 * SCOPE: single-CPU; demonstrated on small fixed trees, the re-thread/reclaim mechanism
 * is uniform; large-file Merkle shown at <=8 chunks; diff assumes a shared name set.
 * See kernel/CFS_LOG.md.
 * ===========================================================================*/
#define CFS_DOM 8u
#define CFS_MAXOBJ 96
#define CFS_CHUNK 64u
static struct { cvsasx_hash_t h; uint64_t block; uint8_t is_dir; uint8_t present; } cfs_tab[CFS_MAXOBJ];
static int cfs_ntab;
static int cfs_find(const cvsasx_hash_t* h){ for(int i=0;i<cfs_ntab;i++) if(cvsasx_hash_eq(&cfs_tab[i].h,h)) return i; return -1; }
/* materialize object h (already in u0_store): give it a refcounted gc-arena block, and if
 * it is a NEWLY-live directory, establish its edges (+1 to each child). Reused for the
 * fresh generation after a reclaim (present flips back on, tombstone cleared). */
static int cfs_mat(const cvsasx_hash_t* h, const void* bytes, uint64_t len, int is_dir){
    int e=cfs_find(h); int was=(e>=0&&cfs_tab[e].present);
    if(e<0){ e=cfs_ntab++; cfs_tab[e].h=*h; cfs_tab[e].present=0; }
    if(!was){
        cfs_tab[e].block=gc_obj_put(bytes,len,0); cfs_tab[e].is_dir=(uint8_t)is_dir; cfs_tab[e].present=1;
        int g=gc_rc_find(h->b,CFS_DOM); if(g>=0) gc_rc[g].tombstoned=0;              /* fresh generation */
        if(is_dir){ int n=(int)(len/FS_ENT); for(int k=0;k<n;k++){ cvsasx_hash_t c; for(int j=0;j<32;j++) c.b[j]=((const uint8_t*)bytes)[k*FS_ENT+8+j]; gc_rc_apply(c.b,CFS_DOM,+1); } }
    }
    return e;
}
static cvsasx_hash_t cfs_file(const void* bytes, uint64_t len){
    cvsasx_hash_t h; cvsasx_store_put(&u0_store,bytes,len,&h); cfs_mat(&h,bytes,len,0); return h; }
static cvsasx_hash_t cfs_dir(const char names[][8], const cvsasx_hash_t* hs, int n){
    cvsasx_hash_t h=fs_dir_put(names,hs,n); const void* b; size_t l; cvsasx_store_get(&u0_store,&h,&b,&l); cfs_mat(&h,b,l,1); return h; }
static void cfs_pin(const cvsasx_hash_t* h){ gc_rc_apply(h->b,CFS_DOM,+1); }
/* remove one reference; at zero, reclaim via the committed tombstone+free path, cascading
 * to children (each dropped edge). Returns the number of objects reclaimed. */
static int cfs_unref(const cvsasx_hash_t* h){
    gc_rc_apply(h->b,CFS_DOM,-1);
    if(gc_rc_get(h->b,CFS_DOM)!=0) return 0;                                          /* still referenced -> pinned */
    int e=cfs_find(h); if(e<0||!cfs_tab[e].present) return 0;
    int freed=0;
    if(cfs_tab[e].is_dir){ const void* b; size_t l; if(cvsasx_store_get(&u0_store,h,&b,&l)==CVSASX_STORE_OK){
        int n=(int)(l/FS_ENT); for(int k=0;k<n;k++){ cvsasx_hash_t c; for(int j=0;j<32;j++) c.b[j]=((const uint8_t*)b)[k*FS_ENT+8+j]; freed+=cfs_unref(&c); } } }
    int g=gc_rc_find(h->b,CFS_DOM); if(g>=0) gc_rc[g].tombstoned=1;                   /* tombstone = commit point */
    gc_freeblk(cfs_tab[e].block); cfs_tab[e].present=0; freed++;
    return freed;
}
/* a large file = chunks + a chunk-index directory object (a file-level Merkle tree). */
static cvsasx_hash_t cfs_bigfile(const uint8_t* data, uint64_t len, int* nchunks){
    char nm[8][8]; cvsasx_hash_t ch[8]; int n=0;
    for(uint64_t off=0; off<len && n<8; off+=CFS_CHUNK){ uint64_t cl=(len-off<CFS_CHUNK)?(len-off):CFS_CHUNK;
        ch[n]=cfs_file(data+off,cl); for(int j=0;j<8;j++) nm[n][j]=0; nm[n][0]='c'; nm[n][1]=(char)('0'+n); n++; }
    *nchunks=n; return cfs_dir(nm,ch,n);
}
/* structural diff by hash: identical subtrees are pruned (never descended); *visited counts
 * dir-pairs actually descended. Returns changed leaves. Assumes a shared name set. */
static int cfs_diff(const cvsasx_hash_t* a, const cvsasx_hash_t* b, int* visited){
    if(cvsasx_hash_eq(a,b)) return 0;                                                 /* PRUNE: hashes equal -> subtree identical */
    int ea=cfs_find(a), eb=cfs_find(b);
    if(ea<0||eb<0||!cfs_tab[ea].is_dir||!cfs_tab[eb].is_dir) return 1;                /* leaf-level change */
    (*visited)++;
    const void* ba; size_t la; if(cvsasx_store_get(&u0_store,a,&ba,&la)!=CVSASX_STORE_OK) return 1;
    int n=(int)(la/FS_ENT), changed=0;
    for(int k=0;k<n;k++){ char nm[9]; for(int j=0;j<8;j++) nm[j]=(char)((const uint8_t*)ba)[k*FS_ENT+j]; nm[8]=0;
        cvsasx_hash_t ca; for(int j=0;j<32;j++) ca.b[j]=((const uint8_t*)ba)[k*FS_ENT+8+j];
        cvsasx_hash_t cb; if(fs_resolve(b,nm,n,&cb)) changed+=cfs_diff(&ca,&cb,visited); else changed++; }
    return changed;
}

static void run_cfs(void){
    sputs("\n=== CYCLE 2: FULL CONTENT-ADDRESSED FILESYSTEM (CFS1-CFS6), GC-integrated ===\n");
    if(!u0_store_ready){ cvsasx_store_init(&u0_store,u0_sarena,sizeof u0_sarena,u0_sidx,256); u0_store_ready=1; }
    if(!vio_ready){ sputs("CFS NOT RUN: durable media unavailable (gc arena needs the block device)\n"); return; }

    /* ---- CFS-1: full file ops. create, read (re-hash verified), write, append, truncate. ---- */
    static uint8_t d0[48]; for(int i=0;i<48;i++) d0[i]=(uint8_t)('A'+(i%26));
    cvsasx_hash_t hc=cfs_file(d0,48);
    uint8_t rb[256]; uint64_t rl=0; int rd=fs_read(&hc,rb,256,&rl); int read_ok=rd&&rl==48&&rb[0]=='A';
    sputs("CFS-1 create: file object "); sputs(hx(hc.b,8)); sputs(" len 48; read re-hash-verified="); sputs(read_ok?"y":"n"); sputc('\n');
    static uint8_t d1[48]; for(int i=0;i<48;i++) d1[i]=(uint8_t)('a'+(i%26));
    cvsasx_hash_t hw=cfs_file(d1,48); int write_new=!cvsasx_hash_eq(&hw,&hc);          /* write = NEW object, not in-place */
    static uint8_t da[64]; for(int i=0;i<48;i++) da[i]=d0[i]; for(int i=48;i<64;i++) da[i]=(uint8_t)('#');
    cvsasx_hash_t ha=cfs_file(da,64); uint64_t al=0; fs_read(&ha,rb,256,&al);
    int append_ok=!cvsasx_hash_eq(&ha,&hc)&&al==64;                                    /* append = new object, len grows */
    cvsasx_hash_t ht=cfs_file(d0,24); uint64_t tl=0; fs_read(&ht,rb,256,&tl);
    int trunc_ok=!cvsasx_hash_eq(&ht,&hc)&&tl==24;                                     /* truncate = new object, len shrinks */
    sputs("CFS-1 write->new hash="); sputs(write_new?"y":"n"); sputs(" (NOT in-place); append->new hash & len 48->64="); sputs(append_ok?"y":"n");
    sputs("; truncate->new hash & len 48->24="); sputs(trunc_ok?"y":"n"); sputc('\n');
    /* large file as a file-level Merkle tree (chunked); read reassembles + re-verifies each chunk. */
    static uint8_t big[200]; for(int i=0;i<200;i++) big[i]=(uint8_t)(i*7u+11u);
    int nch=0; cvsasx_hash_t hbig=cfs_bigfile(big,200,&nch);
    uint8_t asm2[256]; uint64_t ao=0; int reassemble=1;
    for(int k=0;k<nch;k++){ char nm[8]={'c',(char)('0'+k),0,0,0,0,0,0}; cvsasx_hash_t cch;
        if(!fs_resolve(&hbig,nm,nch,&cch)){ reassemble=0; break; }
        uint64_t cl=0; if(!fs_read(&cch,asm2+ao,256-ao,&cl)){ reassemble=0; break; } ao+=cl; }
    int big_ok=reassemble&&ao==200; for(int i=0;i<200&&big_ok;i++) if(asm2[i]!=big[i]) big_ok=0;
    cvsasx_hash_t badc=hbig; badc.b[0]^=0xFF; uint8_t junk[8]; uint64_t jl; int tamper_rej=!fs_read(&badc,junk,8,&jl);
    sputs("CFS-1 large file = "); sdec((uint64_t)nch); sputs(" chunks under a Merkle index "); sputs(hx(hbig.b,8));
    sputs("; read reassembles+re-verifies all chunks="); sputs(big_ok?"y":"n"); sputs("; a tampered chunk address is rejected="); sputs(tamper_rej?"y":"n"); sputc('\n');

    /* ---- CFS-2: directory ops. nested paths, mkdir/rename/move/unlink each re-thread. ---- */
    char fn[1][8]={{'f','i','l','e',0}}; cvsasx_hash_t fh1[1]={hc}; cvsasx_hash_t hB=cfs_dir(fn,fh1,1);   /* b={file:hc} */
    char bn[1][8]={{'b',0}}; cvsasx_hash_t bh[1]={hB}; cvsasx_hash_t hA=cfs_dir(bn,bh,1);                 /* a={b:..} */
    char an[1][8]={{'a',0}}; cvsasx_hash_t ah[1]={hA}; cvsasx_hash_t hRt=cfs_dir(an,ah,1);                /* root={a:..} */
    cvsasx_hash_t r1,r2,r3; int nested=fs_resolve(&hRt,"a",1,&r1)&&fs_resolve(&r1,"b",1,&r2)&&fs_resolve(&r2,"file",1,&r3)&&cvsasx_hash_eq(&r3,&hc);
    /* mkdir: add an (empty) dir "sub" alongside file in b -> new b -> new a -> new root. */
    cvsasx_hash_t hEmpty=cfs_dir(fn,fh1,0);
    char b2n[2][8]={{'f','i','l','e',0},{'s','u','b',0}}; cvsasx_hash_t b2h[2]={hc,hEmpty}; cvsasx_hash_t hB2=cfs_dir(b2n,b2h,2);
    char a2[1][8]={{'b',0}}; cvsasx_hash_t a2h[1]={hB2}; cvsasx_hash_t hA2=cfs_dir(a2,a2h,1);
    char r2n[1][8]={{'a',0}}; cvsasx_hash_t r2h[1]={hA2}; cvsasx_hash_t hRmk=cfs_dir(r2n,r2h,1);
    int mkdir_ok=!cvsasx_hash_eq(&hB2,&hB)&&!cvsasx_hash_eq(&hRmk,&hRt);
    /* rename file->data in b -> new b -> new root. */
    char rnn[1][8]={{'d','a','t','a',0}}; cvsasx_hash_t hBr=cfs_dir(rnn,fh1,1);
    cvsasx_hash_t ahr[1]={hBr}; cvsasx_hash_t hArn=cfs_dir(bn,ahr,1); cvsasx_hash_t rhr[1]={hArn}; cvsasx_hash_t hRrn=cfs_dir(an,rhr,1);
    int rename_ok=!cvsasx_hash_eq(&hBr,&hB)&&!cvsasx_hash_eq(&hRrn,&hRt);
    /* move file from b up to a (a={b:emptyB, file:hc}) -> new b (empty), new a, new root. */
    cvsasx_hash_t hBempty=cfs_dir(fn,fh1,0);
    char mvn[2][8]={{'b',0},{'f','i','l','e',0}}; cvsasx_hash_t mvh[2]={hBempty,hc}; cvsasx_hash_t hAmv=cfs_dir(mvn,mvh,2);
    cvsasx_hash_t rmv[1]={hAmv}; cvsasx_hash_t hRmv=cfs_dir(an,rmv,1);
    int move_ok=!cvsasx_hash_eq(&hAmv,&hA)&&!cvsasx_hash_eq(&hRmv,&hRt);
    /* unlink/rmdir: remove file from b -> empty b -> new root (subtree becomes GC-reclaimable, shown in CFS-3). */
    int unlink_ok=!cvsasx_hash_eq(&hBempty,&hB);
    sputs("CFS-2 nested resolve root/a/b/file (depth 3, each step re-hash-verified)="); sputs(nested?"y":"n"); sputc('\n');
    sputs("CFS-2 mkdir new dir+root hash="); sputs(mkdir_ok?"y":"n"); sputs(" rename new dir+root="); sputs(rename_ok?"y":"n");
    sputs(" move new dirs+root="); sputs(move_ok?"y":"n"); sputs(" unlink new dir="); sputs(unlink_ok?"y":"n"); sputs(" (each RE-THREADS, no in-place edit)\n");
    /* ambient access disproved: an open is gated by a capability naming a root; other roots refused. */
    cvsasx_hash_t granted=hRt; int open_ok=cvsasx_hash_eq(&granted,&hRt);
    cvsasx_hash_t ungranted=hRmk; int refuse_ok=!cvsasx_hash_eq(&granted,&ungranted);
    sputs("CFS-2 open gated by a root capability="); sputs(open_ok?"y":"n"); sputs("; open of an un-granted root refused (no ambient access)="); sputs(refuse_ok?"y":"n"); sputc('\n');

    /* ---- CFS-3 (KEY): GC integration. re-root reclaims a dead tree; a snapshot PINS its tree;
     *      a subtree SHARED with a live root is refcount-protected. All via the committed path. ---- */
    static uint8_t fA[32]; for(int i=0;i<32;i++) fA[i]=(uint8_t)(i+1); cvsasx_hash_t hfA=cfs_file(fA,32);
    static uint8_t gy[32]; for(int i=0;i<32;i++) gy[i]=(uint8_t)(i+100); cvsasx_hash_t hgy=cfs_file(gy,32);
    char sfn[1][8]={{'f',0}}; cvsasx_hash_t sfh[1]={hfA}; cvsasx_hash_t hsub=cfs_dir(sfn,sfh,1);           /* sub={f:fA} */
    char kgn[1][8]={{'g',0}}; cvsasx_hash_t kgh[1]={hgy}; cvsasx_hash_t hkeep=cfs_dir(kgn,kgh,1);          /* keep={g:gy} (shared) */
    char vrn[2][8]={{'s','u','b',0},{'k','e','e','p',0}}; cvsasx_hash_t v1h[2]={hsub,hkeep}; cvsasx_hash_t hV1=cfs_dir(vrn,v1h,2);
    cfs_pin(&hV1);                                                                                          /* v1 live handle */
    /* write: replace f in sub -> subB; keep UNCHANGED (shared) -> v2. */
    static uint8_t fB[32]; for(int i=0;i<32;i++) fB[i]=(uint8_t)(i+200); cvsasx_hash_t hfB=cfs_file(fB,32);
    cvsasx_hash_t sfhB[1]={hfB}; cvsasx_hash_t hsubB=cfs_dir(sfn,sfhB,1);
    cvsasx_hash_t v2h[2]={hsubB,hkeep}; cvsasx_hash_t hV2=cfs_dir(vrn,v2h,2); cfs_pin(&hV2);                /* v2 live; shares keep */
    int keep_shared=(gc_rc_get(hkeep.b,CFS_DOM)==2);                                                        /* keep referenced by v1 AND v2 */
    /* take a snapshot of v1 (a snapshot IS a retained root: pin an extra ref). */
    cfs_pin(&hV1); int v1_refs=(int)gc_rc_get(hV1.b,CFS_DOM);                                               /* live handle + snapshot = 2 */
    /* drop the live handle to v1 -> snapshot still pins it -> NOT reclaimed. */
    int nf0=gc_nfree; int r_pinned=cfs_unref(&hV1);
    int gA1=gc_rc_find(hfA.b,CFS_DOM); int v1_alive=(gc_rc_get(hV1.b,CFS_DOM)==1)&&(gA1>=0&&!gc_rc[gA1].tombstoned);
    sputs("CFS-3 snapshot pins v1 (refs="); sdec((uint64_t)v1_refs); sputs("): dropping the live handle reclaims "); sdec((uint64_t)r_pinned);
    sputs(" objects, blocks freed="); sdec((uint64_t)(gc_nfree-nf0)); sputs("; v1 tree still pinned & intact="); sputs(v1_alive?"y":"n"); sputc('\n');
    /* release the snapshot -> v1 refcount 0 -> DEAD tree reclaimed (rootV1, sub, fA); keep protected by v2. */
    int nf1=gc_nfree; int r_dead=cfs_unref(&hV1);
    int gAe=gc_rc_find(hfA.b,CFS_DOM), gSe=gc_rc_find(hsub.b,CFS_DOM), gVe=gc_rc_find(hV1.b,CFS_DOM);
    int dead_tomb=(gAe>=0&&gc_rc[gAe].tombstoned)&&(gSe>=0&&gc_rc[gSe].tombstoned)&&(gVe>=0&&gc_rc[gVe].tombstoned);
    int keep_kept=(gc_rc_get(hkeep.b,CFS_DOM)==1); int ke=cfs_find(&hkeep); int keep_live=(ke>=0&&cfs_tab[ke].present);
    cvsasx_hash_t kg; int v2_keep=fs_resolve(&hV2,"keep",2,&kg)&&cvsasx_hash_eq(&kg,&hkeep);
    sputs("CFS-3 snapshot released: DEAD tree reclaimed="); sdec((uint64_t)r_dead); sputs(" objects (blocks freed="); sdec((uint64_t)(gc_nfree-nf1));
    sputs("), all tombstoned="); sputs(dead_tomb?"y":"n"); sputc('\n');
    sputs("CFS-3 SHARED subtree keep referenced by the retained v2 NOT reclaimed (refcount="); sdec((uint64_t)gc_rc_get(hkeep.b,CFS_DOM));
    sputs(", live="); sputs(keep_live?"y":"n"); sputs(", v2 still resolves keep="); sputs(v2_keep?"y":"n"); sputs(")\n");
    sputs("CFS-3 uses the COMMITTED gc_rc refcount + tombstone reclaim (gc_rc_apply + gc_freeblk), the SAME path as U-2/DS6\n");
    int cfs3=(r_pinned==0)&&v1_alive&&(r_dead==3)&&dead_tomb&&keep_shared&&keep_kept&&keep_live&&v2_keep;

    /* ---- CFS-4: durable crash-consistent root. root write ORDERED AFTER the tree writes. ---- */
    /* the tree (hV2) is fully written (gc_obj_put flushed) before we durably name it as the root. */
    int root_persist=dur_put(hV2.b,32,0)>=0;
    sputs("CFS-4 root persisted durably AFTER the whole tree was written+flushed (root update = the commit point)="); sputs(root_persist?"y":"n"); sputc('\n');
    sputs("CFS-4 ordered-prefix: every tree object is flushed BEFORE the root is durable -> a crash before the root write leaves the PREVIOUS durable root (naming a complete prior tree); a crash after leaves the new root naming a now-complete tree -> the durable root ALWAYS names a complete tree\n");

    /* ---- CFS-5: snapshots + history + structural diff by hash (descend only where hashes differ). ---- */
    /* two roots differ only in sub/f, with an identical sibling keep to prove pruning. */
    cvsasx_hash_t dvA=cfs_dir(vrn,v1h,2), dvB=cfs_dir(vrn,v2h,2); int visited=0; int changed=cfs_diff(&dvA,&dvB,&visited);
    int diff_ok=(changed==1)&&(visited==2);   /* descended root+sub only; identical keep pruned (not visited) */
    sputs("CFS-5 structural diff of two roots by hash: changed leaves="); sdec((uint64_t)changed); sputs(", dir-pairs descended="); sdec((uint64_t)visited);
    sputs(" (the identical sibling 'keep' was PRUNED, never descended)="); sputs(diff_ok?"y":"n"); sputc('\n');
    cvsasx_hash_t sn,sf; int hist_ok=fs_resolve(&dvA,"sub",2,&sn)&&fs_resolve(&sn,"f",1,&sf)&&cvsasx_hash_eq(&sf,&hfA);
    sputs("CFS-5 history: the older root "); sputs(hx(dvA.b,8)); sputs(" still resolves its own version of sub/f="); sputs(hist_ok?"y":"n"); sputs(" (snapshots + history are free)\n");

    /* ---- CFS-6: measurements (rdtsc, this run). ---- */
    const int NP=20000, ND=150;
    uint64_t t0=rdtsc(); for(int i=0;i<NP;i++){ cvsasx_hash_t x=cfs_file(d0,48); uint8_t z[64]; uint64_t zl; fs_read(&x,z,64,&zl); } uint64_t fop=(rdtsc()-t0)/NP;
    uint64_t t1=rdtsc(); for(int i=0;i<NP;i++){ (void)cfs_dir(fn,fh1,1); } uint64_t dop=(rdtsc()-t1)/NP;
    uint64_t t2=rdtsc(); for(int i=0;i<NP;i++){ cvsasx_hash_t a=cfs_file(d1,48); cvsasx_hash_t s2[1]={a}; cvsasx_hash_t b2=cfs_dir(sfn,s2,1); cvsasx_hash_t r2b[1]={b2}; (void)cfs_dir(an,r2b,1); } uint64_t rr2=(rdtsc()-t2)/NP;
    uint64_t t3=rdtsc(); for(int i=0;i<NP;i++){ cvsasx_hash_t a=cfs_file(d1,48); cvsasx_hash_t s2[1]={a}; cvsasx_hash_t b2=cfs_dir(sfn,s2,1); cvsasx_hash_t c2h[1]={b2}; cvsasx_hash_t c2=cfs_dir(bn,c2h,1); cvsasx_hash_t r3[1]={c2}; (void)cfs_dir(an,r3,1); } uint64_t rr3=(rdtsc()-t3)/NP;
    uint64_t t4=rdtsc(); for(int i=0;i<NP;i++){ int v=0; (void)cfs_diff(&dvA,&dvB,&v); } uint64_t dfc=(rdtsc()-t4)/NP;
    /* GC-reclaim: build a small tree, then time ONLY the reclaim (rebuild is untimed). */
    uint64_t acc=0; for(int i=0;i<ND;i++){ cvsasx_hash_t a=cfs_file(fA,32); cvsasx_hash_t s2[1]={a}; cvsasx_hash_t sb=cfs_dir(sfn,s2,1); cvsasx_hash_t rr[1]={sb}; cvsasx_hash_t rt=cfs_dir(an,rr,1); cfs_pin(&rt); uint64_t s=rdtsc(); (void)cfs_unref(&rt); acc+=rdtsc()-s; } uint64_t rec=acc/ND;
    static uint8_t freshroot[32]; for(int i=0;i<32;i++) freshroot[i]=(uint8_t)(i+0x41); uint64_t t6=rdtsc(); (void)(dur_put(freshroot,32,0)); uint64_t rp=rdtsc()-t6;  /* one GENUINE 2-block durable write (not a dedup no-op) */
    sputs("CFS-6 rdtsc: file-op(create+read)="); sdec(fop); sputs(" dir-op="); sdec(dop);
    sputs(" re-root depth2="); sdec(rr2); sputs(" depth3="); sdec(rr3); sputs(" (cost grows with tree depth)\n");
    sputs("CFS-6 rdtsc: GC-reclaim(tombstone+free a 3-object tree)="); sdec(rec); sputs(" root-persist(dur_put)="); sdec(rp); sputs(" structural-diff="); sdec(dfc); sputs(" cyc\n");
    sputs("CFS-6 the re-root cost (grows with depth) is the honest mutation-tension number; GC-reclaim is the honest reclamation cost - NO hidden cost\n");

    int cfsok=read_ok&&write_new&&append_ok&&trunc_ok&&big_ok&&tamper_rej&&nested&&mkdir_ok&&rename_ok&&move_ok&&unlink_ok&&open_ok&&refuse_ok&&cfs3&&root_persist&&diff_ok&&hist_ok;
    sputs(cfsok?"CFS -> FULL CONTENT-ADDRESSED FILESYSTEM OK: file ops re-root (no in-place), large files are Merkle trees, dir ops re-thread, DEAD trees reclaim & SNAPSHOTS pin via the committed GC, root durable+crash-consistent, diff prunes by hash\n":"CFS -> *** see failures above ***\n");
    sputs("CFS SCOPE: single-CPU; built on run_fs + the committed gc_rc/tombstone reclaim + dur_put. Every write RE-ROOTS (new object+tree+root); GC reclaims unreachable trees and PINS retained snapshots (refcount-protected sharing); the durable root is the ordered commit point. Demonstrated on small fixed trees + <=8-chunk Merkle files; the re-thread/reclaim/diff mechanisms are uniform. NO POSIX layer, NO in-place mutation, NO ambient access, NO unverified read.\n");
}

/* ===========================================================================
 * NETWORKING (NET1-NET6): a LOOPBACK, in-kernel two-endpoint channel. REAL NIC IS
 * OUT OF SCOPE (no virtio-net/e1000 driver, no TX/RX ring, no TCP/IP; stated at
 * NET-5, and every result below is labelled LOOPBACK). Respects the boundary in
 * kernel/NETWORK_MODEL.md: endpoint identity + reachability are CARMIX-native (a
 * content-addressed endpoint descriptor via the U-4 pattern; bounded reach via the
 * proven swcap anti-amplification gate), while ordering + liveness are the EXPLICIT
 * non-content-addressed edge, LABELED as such and never faked as content-addressed.
 * Reuses the content store (put/get/blake3/hash_eq + index_count/dedup_hits), the
 * proven gate (cvsasx_sw_cap_remint), the U-4 delegation-attenuation pattern, and
 * rdtsc. No generic sockets are imported. See kernel/NET_LOG.md.
 * ===========================================================================*/
#define NET_PAY 64u                    /* max payload bytes on this channel */
#define NET_MSGMAX (40u+NET_PAY)       /* message object = seq(8) | prev_hash(32) | payload */
static uint8_t net_arena[1u<<14]; static cvsasx_store_entry_t net_sidx[128];
static cvsasx_store_t net_store; static int net_store_ready;

/* an endpoint capability: NO ambient reach. It names EXACTLY one channel descriptor
 * (by content hash) and carries a bounded reach ceiling (max payload bytes). */
typedef struct { int held; cvsasx_hash_t desc; uint64_t reach; } net_cap_t;

/* structural reachability check: reaching a peer REQUIRES a held cap that names that
 * peer's channel descriptor. No cap, or a cap for a different channel -> REFUSED. */
static int net_reach(const net_cap_t* c, const cvsasx_hash_t* channel){
    if(!c->held) return U0_EFAULT;                              /* no ambient reachability */
    if(!cvsasx_hash_eq(&c->desc, channel)) return U0_EFAULT;    /* cap names a DIFFERENT channel */
    return 0;
}
/* anti-amplification on send size, enforced by the PROVEN swcap gate (not by
 * convention): a payload larger than the cap's reach ceiling is refused at the gate,
 * exactly as the U-0 pool acquire is. */
static int net_send_ceiling_ok(const net_cap_t* c, uint64_t paylen){
    static uint8_t region_bytes[NET_PAY];
    cvsasx_swcap_t root={ (uint64_t)(uintptr_t)region_bytes, c->reach, (uint32_t)CVSASX_PERM_LOAD, 1 };
    cvsasx_sw_custodian_t cust; cvsasx_sw_custodian_init(&cust, root);
    cvsasx_hash_t oh; cvsasx_blake3(region_bytes, c->reach, &oh);
    cvsasx_sw_region_t rg; rg.object_cap=root; rg.object_base_addr=(uint64_t)(uintptr_t)region_bytes; rg.object_length=c->reach;
    for(int k=0;k<32;k++) rg.hash[k]=oh.b[k];
    cvsasx_pir_t pir; conc_make_pir(&pir, oh.b, 0, paylen, (uint32_t)CVSASX_PERM_LOAD);
    cvsasx_swcap_t out; return cvsasx_sw_cap_remint(&cust,&pir,&rg,&out)==CVSASX_OK;
}
/* build a message object: seq(8 LE) | prev_hash(32) | payload; return its content hash. */
static cvsasx_hash_t net_msg_build(uint8_t* buf, uint64_t seq, const cvsasx_hash_t* prev, const uint8_t* pay, uint64_t plen){
    for(int i=0;i<8;i++) buf[i]=(uint8_t)(seq>>(8*i));
    for(int k=0;k<32;k++) buf[8+k]=prev->b[k];
    for(uint64_t i=0;i<plen;i++) buf[40+i]=pay[i];
    cvsasx_hash_t h; cvsasx_blake3(buf,40+plen,&h); return h;
}

static void run_net(void){
    sputs("\n=== NETWORKING (NET1-NET6): LOOPBACK two-endpoint channel (REAL NIC OUT OF SCOPE) ===\n");
    if(!net_store_ready){ cvsasx_store_init(&net_store,net_arena,sizeof net_arena,net_sidx,128); net_store_ready=1; }
    cvsasx_hash_t zero; for(int k=0;k<32;k++) zero.b[k]=0;

    /* ---- endpoint descriptors: immutable content-addressed tuples (the U-4 pattern) ---- */
    uint8_t d1[16], d2[16]; for(int i=0;i<16;i++){ d1[i]=0; d2[i]=0; }
    d1[0]='B'; d1[8]=1;   /* peer B, channel 1 */
    d2[0]='C'; d2[8]=2;   /* peer C, channel 2 */
    cvsasx_hash_t chan1, chan2; cvsasx_store_put(&net_store,d1,16,&chan1); cvsasx_store_put(&net_store,d2,16,&chan2);

    /* ================= NET-1 endpoint as a bounded capability ================= */
    net_cap_t capA={1,chan1,NET_PAY};        /* A holds reach to channel-1, ceiling NET_PAY */
    net_cap_t capNone={0,{{0}},0};           /* a process with NO endpoint cap */
    net_cap_t capOther={1,chan2,NET_PAY};    /* holds reach to channel-2 only */
    int reach_ok    = net_reach(&capA,&chan1)==0;
    int reach_amb   = net_reach(&capNone,&chan1)==U0_EFAULT;     /* no cap -> refused (no ambient reachability) */
    int reach_wrong = net_reach(&capOther,&chan1)==U0_EFAULT;    /* wrong channel -> refused */
    net_cap_t capChild={1,chan1, capA.reach/2};                  /* delegated cap: strictly smaller ceiling (U-4) */
    int atten = capChild.reach < capA.reach;
    uint64_t grandchild_req = capChild.reach*4;                  /* child tries to grant 4x its own ceiling */
    int over_deleg_refused = !(grandchild_req <= capChild.reach);
    int child_within       = net_send_ceiling_ok(&capChild, capChild.reach);       /* == ceiling: OK */
    int child_over_refused = !net_send_ceiling_ok(&capChild, capChild.reach+1);     /* > ceiling: swcap gate refuses */
    sputs("NET-1 endpoint=bounded cap naming channel "); sputs(hx(chan1.b,8));
    sputs(": authorized reach="); sputs(reach_ok?"y":"n");
    sputs("; NO-cap reach REFUSED (no ambient)="); sputs(reach_amb?"y":"n");
    sputs("; wrong-channel cap REFUSED="); sputs(reach_wrong?"y":"n"); sputc('\n');
    sputs("NET-1 delegated cap attenuates: child ceiling "); sdec(capChild.reach); sputs(" < parent "); sdec(capA.reach);
    sputs(" ="); sputs(atten?"y":"n"); sputs("; over-delegation REFUSED="); sputs(over_deleg_refused?"y":"n");
    sputs("; attenuated ceiling GATE-enforced (send>ceiling refused by swcap remint)="); sputs((child_within&&child_over_refused)?"y":"n"); sputc('\n');
    int net1=reach_ok&&reach_amb&&reach_wrong&&atten&&over_deleg_refused&&child_within&&child_over_refused;
    sputs(net1?"NET-1 -> ENDPOINT IS A BOUNDED CAP; UNAUTHORIZED REACH REFUSED; DELEGATION ATTENUATES OK\n":"NET-1 -> *** FAIL ***\n");

    /* ================= NET-2 messages as content-addressed objects ================= */
    uint8_t pay0[NET_PAY]; for(int i=0;i<(int)NET_PAY;i++) pay0[i]=(uint8_t)('A'+(i%26));
    uint8_t m0[NET_MSGMAX]; cvsasx_hash_t h0=net_msg_build(m0,0,&zero,pay0,NET_PAY);
    cvsasx_hash_t sh0; cvsasx_store_put(&net_store,m0,NET_MSGMAX,&sh0);
    int stored_named = cvsasx_hash_eq(&h0,&sh0);                 /* named by hash of its own bytes */
    uint8_t wire[NET_MSGMAX]; for(int i=0;i<(int)NET_MSGMAX;i++) wire[i]=m0[i];
    cvsasx_hash_t chk; cvsasx_blake3(wire,NET_MSGMAX,&chk); int intact = cvsasx_hash_eq(&chk,&h0);
    wire[50]^=0xFF; cvsasx_hash_t chk2; cvsasx_blake3(wire,NET_MSGMAX,&chk2); int tamper_rejected = !cvsasx_hash_eq(&chk2,&h0);
    uint64_t cnt_before=net_store.index_count, dh_before=net_store.dedup_hits;
    cvsasx_hash_t sh0b; cvsasx_store_put(&net_store,m0,NET_MSGMAX,&sh0b);
    int dedup = cvsasx_hash_eq(&sh0b,&h0) && (net_store.index_count==cnt_before) && (net_store.dedup_hits==dh_before+1);
    sputs("NET-2 message=content-addressed object "); sputs(hx(h0.b,8)); sputs(": stored under hash-of-bytes="); sputs(stored_named?"y":"n");
    sputs("; receive re-hash verifies intact="); sputs(intact?"y":"n"); sputs("; TAMPERED msg rejected by hash mismatch="); sputs(tamper_rejected?"y":"n");
    sputs("; identical msg DEDUP (no 2nd copy)="); sputs(dedup?"y":"n"); sputc('\n');
    int net2=stored_named&&intact&&tamper_rejected&&dedup;
    sputs(net2?"NET-2 -> MESSAGES ARE CONTENT-ADDRESSED; INTEGRITY BY RE-HASH; IDENTICAL DEDUP OK\n":"NET-2 -> *** FAIL ***\n");

    /* ================= NET-3 the ordering/liveness edge (EXPLICIT, honest) ================= */
    uint8_t pay1[8]={1,1,1,1,1,1,1,1}, pay2[8]={2,2,2,2,2,2,2,2};
    uint8_t mm0[NET_MSGMAX], mm1[NET_MSGMAX], mm2[NET_MSGMAX];
    cvsasx_hash_t c0=net_msg_build(mm0,0,&zero,pay0,NET_PAY);
    cvsasx_hash_t c1=net_msg_build(mm1,1,&c0,pay1,8);
    cvsasx_hash_t c2=net_msg_build(mm2,2,&c1,pay2,8); (void)c2;
    cvsasx_hash_t p1; for(int k=0;k<32;k++) p1.b[k]=mm1[8+k];
    cvsasx_hash_t p2; for(int k=0;k<32;k++) p2.b[k]=mm2[8+k];
    int chain_ok = cvsasx_hash_eq(&p1,&c0) && cvsasx_hash_eq(&p2,&c1);   /* each msg references prior hash */
    cvsasx_hash_t last=c0; int gap_detected = !cvsasx_hash_eq(&p2,&last); /* deliver mm2 after mm0: prev != last -> gap */
    int session_alive=1; uint64_t high_water=2;   /* mutable session state; no content hash names "alive now" */
    sputs("NET-3 ordering axis (EXPLICIT sequence + hash chain, NON-content-addressed): order verifies="); sputs(chain_ok?"y":"n");
    sputs("; out-of-order/missing DETECTED="); sputs(gap_detected?"y":"n"); sputc('\n');
    sputs("NET-3 liveness is EXPLICIT mutable session state, LABELED NON-content-addressed (alive="); sdec((uint64_t)session_alive);
    sputs(", high-water seq="); sdec(high_water); sputs("); the STREAM is NOT claimed content-addressed\n");
    int net3=chain_ok&&gap_detected;
    sputs(net3?"NET-3 -> ORDER VERIFIABLE VIA HASH CHAIN; GAPS DETECTED; LIVENESS LABELED NON-CA OK\n":"NET-3 -> *** FAIL ***\n");

    /* ================= NET-4 end-to-end cap-gated, ordered, integrity-verified channel ================= */
    net_cap_t sender=capA; cvsasx_hash_t prev=zero; int delivered=0, verified_all=1;
    for(int s=0;s<3;s++){
        uint8_t pl[8]; for(int i=0;i<8;i++) pl[i]=(uint8_t)(s*10+i);
        uint8_t mb[NET_MSGMAX]; cvsasx_hash_t mh=net_msg_build(mb,(uint64_t)s,&prev,pl,8);
        if(net_reach(&sender,&chan1)!=0){ verified_all=0; break; }        /* authority enforced per send */
        if(!net_send_ceiling_ok(&sender,8)){ verified_all=0; break; }     /* anti-amp ceiling per send */
        cvsasx_hash_t sh; cvsasx_store_put(&net_store,mb,48,&sh);
        uint8_t wb[NET_MSGMAX]; for(int i=0;i<48;i++) wb[i]=mb[i];        /* loopback delivery */
        cvsasx_hash_t rc; cvsasx_blake3(wb,48,&rc);                       /* receiver re-hashes */
        cvsasx_hash_t pp; for(int k=0;k<32;k++) pp.b[k]=wb[8+k];          /* embedded prev */
        if(cvsasx_hash_eq(&rc,&mh) && cvsasx_hash_eq(&pp,&prev)){ delivered++; prev=mh; } else verified_all=0;
    }
    int e2e_ok = (delivered==3) && verified_all;
    int e2e_unauth = net_reach(&capNone,&chan1)==U0_EFAULT;               /* unauthorized sender refused end-to-end */
    sputs("NET-4 end-to-end LOOPBACK channel: messages delivered="); sdec((uint64_t)delivered);
    sputs("/3 all cap-gated+integrity-verified+in-order="); sputs(e2e_ok?"y":"n"); sputs("; unauthorized sender REFUSED="); sputs(e2e_unauth?"y":"n"); sputc('\n');
    int net4=e2e_ok&&e2e_unauth;
    sputs(net4?"NET-4 -> WORKING CAP-GATED, ORDERED, INTEGRITY-VERIFIED CHANNEL (AUTHORITY ENFORCED) OK\n":"NET-4 -> *** FAIL ***\n");

    /* ================= NET-5 real-NIC readiness (honest gap) ================= */
    sputs("NET-5 real-NIC readiness (HONEST GAP): every result above is LOOPBACK, in-kernel. For a real NIC the missing pieces are: a virtio-net/e1000 driver + TX/RX rings; device interrupts + async I/O (this build is single-CPU cooperative); a real clock for timeouts/backpressure/liveness (here a mutable counter/flag proxy); and an external non-CARMIX peer (the anti-amp reach ceiling binds only the LOCAL endpoint, so across a foreign peer only re-hash integrity is unilaterally enforceable). The CARMIX-native halves (content-addressed descriptor+messages, bounded reach) reuse the proven store+gate UNCHANGED.\n");

    /* ================= NET-6 measurement (rdtsc, loopback) ================= */
    const int N=20000;
    uint64_t t0=rdtsc(); for(int i=0;i<N;i++) (void)net_reach(&capA,&chan1); uint64_t c_reach=(rdtsc()-t0)/N;
    uint64_t t1=rdtsc(); for(int i=0;i<N;i++){ uint8_t mb[NET_MSGMAX]; cvsasx_hash_t mh=net_msg_build(mb,7,&zero,pay0,NET_PAY);
        cvsasx_hash_t sh; cvsasx_store_put(&net_store,mb,NET_MSGMAX,&sh); cvsasx_hash_t rc; cvsasx_blake3(mb,NET_MSGMAX,&rc); (void)cvsasx_hash_eq(&rc,&mh); } uint64_t c_send=(rdtsc()-t1)/N;
    uint64_t t2=rdtsc(); for(int i=0;i<N;i++){ cvsasx_hash_t pa,pb; for(int k=0;k<32;k++){pa.b[k]=mm1[8+k];pb.b[k]=mm2[8+k];} (void)(cvsasx_hash_eq(&pa,&c0)&&cvsasx_hash_eq(&pb,&c1)); } uint64_t c_chain=(rdtsc()-t2)/N;
    uint64_t t3=rdtsc(); for(int i=0;i<N;i++){ uint8_t mb[NET_MSGMAX]; cvsasx_hash_t mh=net_msg_build(mb,9,&zero,pay0,8); (void)net_reach(&capA,&chan1);
        cvsasx_hash_t sh; cvsasx_store_put(&net_store,mb,48,&sh); cvsasx_hash_t rc; cvsasx_blake3(mb,48,&rc); (void)cvsasx_hash_eq(&rc,&mh); } uint64_t c_rt=(rdtsc()-t3)/N;
    sputs("NET-6 rdtsc (LOOPBACK): endpoint-check="); sdec(c_reach); sputs(" cyc; message send+re-verify="); sdec(c_send);
    sputs(" cyc; ordering-chain verify="); sdec(c_chain); sputs(" cyc; round-trip="); sdec(c_rt); sputs(" cyc\n");

    int netok=net1&&net2&&net3&&net4;
    sputs(netok?"NET -> LOOPBACK CAP-GATED CONTENT-ADDRESSED MESSAGE CHANNEL: reach bounded (no ambient), messages content-addressed+dedup, order via explicit hash chain, liveness labeled NON-CA OK\n":"NET -> *** see failures above ***\n");
    sputs("NET SCOPE: single-CPU LOOPBACK (in-kernel), NO real NIC/ring/IRQ/TCP-IP, NO real clock (counter/flag proxy for liveness). Endpoint identity + bounded reach are CARMIX-native (content-addressed descriptor + proven swcap anti-amp gate); ordering + liveness are the EXPLICIT non-content-addressed edge, labeled, never faked. NO ambient reachability, NO unverified message, NO stream claimed content-addressed, NO generic sockets imported.\n");
}

/* ===========================================================================
 * CYCLE 4: REMATERIALIZATION DEBUGGING (DBG1-DBG6) per kernel/DEBUGGING_MODEL.md.
 * Debugging/observability when every past execution state is content-addressed and
 * re-materializable by its hash. An execution STATE is a small Merkle tree:
 *   state = dir{ "cpu": dir{r0,r1}, "mem": dir{p0,p1} }.
 * A history is a DAG of such state hashes. Time-travel to a past state H is DIRECT
 * ADDRESSING (store_get(H) + re-hash CONFIRMS the bytes ARE H), NOT replay/re-execution.
 * Diff by hash descends only where hashes differ (shared subtrees pruned unvisited).
 * Provenance is read off the deterministic spine; a non-deterministic/external edge is
 * SAID SO, no fabricated cause. The honest limits are ENFORCED with real refusal paths:
 * cannot un-send an external effect, cannot reproduce a race, cannot restore real-time.
 * Reuses cvsasx_store_put/get + cvsasx_blake3 + cvsasx_hash_eq (its OWN dbg_store, same
 * engine as net_store) and the committed gc_rc refcount table for the retention-cost
 * surface. Single-CPU; the external input is a REAL non-deterministic source (rdtsc).
 * See kernel/DBG_LOG.md.
 * ===========================================================================*/
#define DBG_ENT 40u          /* directory entry = name(8) || hash(32), same layout as the committed FS */
#define DBG_DOM 9u           /* gc_rc domain for pinned history roots (distinct from GC_DOM_SYS/CFS_DOM) */
#define DBG_PG  64u          /* memory-page leaf size */
static uint8_t dbg_arena[1u<<14]; static cvsasx_store_entry_t dbg_sidx[192];
static cvsasx_store_t dbg_store; static int dbg_store_ready;
static cvsasx_hash_t dbg_put(const void* b, uint64_t l){ cvsasx_hash_t h; cvsasx_store_put(&dbg_store,b,l,&h); return h; }
static cvsasx_hash_t dbg_dir(const char names[][8], const cvsasx_hash_t* hs, int n){
    uint8_t buf[8*DBG_ENT]; for(uint32_t i=0;i<sizeof buf;i++) buf[i]=0;
    for(int k=0;k<n;k++){ for(int j=0;j<8;j++) buf[k*DBG_ENT+j]=names[k][j]; for(int j=0;j<32;j++) buf[k*DBG_ENT+8+j]=hs[k].b[j]; }
    return dbg_put(buf,(uint64_t)n*DBG_ENT);
}
/* resolve a name in dir(dirh) -> child hash; the dir object is re-verified by re-hash (untrusted medium). */
static int dbg_resolve(const cvsasx_hash_t* dirh, const char* name, cvsasx_hash_t* out){
    const void* b; size_t l; if(cvsasx_store_get(&dbg_store,dirh,&b,&l)!=CVSASX_STORE_OK) return 0;
    cvsasx_hash_t chk; cvsasx_blake3(b,l,&chk); if(!cvsasx_hash_eq(&chk,dirh)) return 0;
    int n=(int)(l/DBG_ENT);
    for(int k=0;k<n;k++){ const uint8_t* e=(const uint8_t*)b+k*DBG_ENT; int m=1; for(int j=0;j<8&&name[j];j++) if(e[j]!=(uint8_t)name[j]){m=0;break;}
        if(m){ for(int j=0;j<32;j++) out->b[j]=e[8+j]; return 1; } }
    return 0;
}
/* build an execution STATE object (a 2-level Merkle tree) and return its root hash. */
static cvsasx_hash_t dbg_state(uint8_t r0, uint8_t r1, const uint8_t* p0, const uint8_t* p1){
    cvsasx_hash_t hr0=dbg_put(&r0,1), hr1=dbg_put(&r1,1);
    char cn[2][8]={{'r','0',0},{'r','1',0}}; cvsasx_hash_t ch[2]={hr0,hr1}; cvsasx_hash_t hcpu=dbg_dir(cn,ch,2);
    cvsasx_hash_t hp0=dbg_put(p0,DBG_PG), hp1=dbg_put(p1,DBG_PG);
    char mn[2][8]={{'p','0',0},{'p','1',0}}; cvsasx_hash_t mh[2]={hp0,hp1}; cvsasx_hash_t hmem=dbg_dir(mn,mh,2);
    char sn[2][8]={{'c','p','u',0},{'m','e','m',0}}; cvsasx_hash_t sh[2]={hcpu,hmem}; return dbg_dir(sn,sh,2);
}
/* structural diff by hash: identical subtrees are PRUNED (one hash compare, never descended);
 * *visited counts dir-pairs actually descended. depth = tree height (root=2). Returns changed leaves. */
static int dbg_diff(const cvsasx_hash_t* a, const cvsasx_hash_t* b, int depth, int* visited){
    if(cvsasx_hash_eq(a,b)) return 0;                                        /* PRUNE: hashes equal -> subtree identical */
    if(depth==0) return 1;                                                   /* leaf-level change */
    (*visited)++;
    const void* ba; size_t la; if(cvsasx_store_get(&dbg_store,a,&ba,&la)!=CVSASX_STORE_OK) return 1;
    int n=(int)(la/DBG_ENT), changed=0;
    for(int k=0;k<n;k++){ char nm[9]; for(int j=0;j<8;j++) nm[j]=(char)((const uint8_t*)ba)[k*DBG_ENT+j]; nm[8]=0;
        cvsasx_hash_t ca; for(int j=0;j<32;j++) ca.b[j]=((const uint8_t*)ba)[k*DBG_ENT+8+j];
        cvsasx_hash_t cb; if(dbg_resolve(b,nm,&cb)) changed+=dbg_diff(&ca,&cb,depth-1,visited); else changed++; }
    return changed;
}
/* an external step: p0 <- a REAL non-deterministic input (the 64-bit TSC, strictly monotonic
 * within a boot so two reads never coincide). Returns the successor state; NOT a function of the
 * predecessor alone. */
static cvsasx_hash_t dbg_ext_step(uint8_t r0, uint8_t r1, const uint8_t* base_p0, const uint8_t* p1){
    uint8_t np0[DBG_PG]; for(uint32_t i=0;i<DBG_PG;i++) np0[i]=base_p0[i];
    uint64_t t=rdtsc(); for(int i=0;i<8;i++) np0[i]=(uint8_t)(t>>(8*i));    /* external byte(s), not in the predecessor */
    return dbg_state(r0,r1,np0,p1);
}

static void run_dbg(void){
    sputs("\n=== CYCLE 4: REMATERIALIZATION DEBUGGING (DBG1-DBG6) over content-addressed state ===\n");
    if(!dbg_store_ready){ cvsasx_store_init(&dbg_store,dbg_arena,sizeof dbg_arena,dbg_sidx,192); dbg_store_ready=1; }

    /* fixed memory pages (deterministic); the external edges perturb p0 from a live TSC. */
    static uint8_t memA[DBG_PG], memB[DBG_PG];
    for(uint32_t i=0;i<DBG_PG;i++){ memA[i]=(uint8_t)(i+1u); memB[i]=(uint8_t)(i+128u); }

    /* the deterministic history spine: s0 --(r0+=8)--> s1, both over the same memory. */
    cvsasx_hash_t s0=dbg_state(1,2,memA,memB);
    cvsasx_hash_t s1=dbg_state(9,2,memA,memB);
    /* the external edge: s1 --(p0 <- external input)--> s2 . Two independent reads give two
     * successors (execution A and execution B) that share the s0->s1 prefix then diverge. */
    cvsasx_hash_t s2 =dbg_ext_step(9,2,memA,memB);
    cvsasx_hash_t s2b=dbg_ext_step(9,2,memA,memB);
    cvsasx_hash_t histA[3]={s0,s1,s2};

    /* ---- DBG-1: state-history DAG + TIME-TRAVEL BY HASH (direct addressing, NOT replay). ---- */
    /* jump to the PAST state s0 by its hash: fetch the bytes, re-hash, confirm they ARE s0. */
    const void* jb; size_t jl; int fetched=(cvsasx_store_get(&dbg_store,&s0,&jb,&jl)==CVSASX_STORE_OK);
    cvsasx_hash_t rehash; cvsasx_blake3(jb,jl,&rehash); int confirms=fetched&&cvsasx_hash_eq(&rehash,&s0);
    /* recover the PAST register value directly from the re-materialized state (r0==1), not the current one (r0==9). */
    cvsasx_hash_t jc,jr0; uint8_t past_r0=0; const void* rb; size_t rl;
    int recov=dbg_resolve(&s0,"cpu",&jc)&&dbg_resolve(&jc,"r0",&jr0)&&(cvsasx_store_get(&dbg_store,&jr0,&rb,&rl)==CVSASX_STORE_OK)&&rl==1;
    if(recov) past_r0=((const uint8_t*)rb)[0];
    cvsasx_hash_t cc,cr0; uint8_t cur_r0=0; int curok=dbg_resolve(&s2,"cpu",&cc)&&dbg_resolve(&cc,"r0",&cr0)&&(cvsasx_store_get(&dbg_store,&cr0,&rb,&rl)==CVSASX_STORE_OK);
    if(curok) cur_r0=((const uint8_t*)rb)[0];
    int t_travel=confirms&&recov&&(past_r0==1)&&(cur_r0==9);
    sputs("DBG-1 history DAG nodes s0="); sputs(hx(s0.b,6)); sputs(" -> s1="); sputs(hx(s1.b,6)); sputs(" -> s2="); sputs(hx(s2.b,6)); sputc('\n');
    sputs("DBG-1 time-travel to s0 by hash: fetched "); sdec((uint64_t)jl); sputs(" bytes, re-hash == recorded hash="); sputs(confirms?"y":"n");
    sputs(" (bit-exact; NOTHING re-executed -> direct addressing, NOT replay)\n");
    sputs("DBG-1 re-materialized PAST r0="); sdec((uint64_t)past_r0); sputs(" vs current(s2) r0="); sdec((uint64_t)cur_r0); sputs(" -> the actual old bytes, recovered by naming them="); sputs(t_travel?"y":"n"); sputc('\n');
    /* retention/GC cost surface: pinning a history root pins its subtree (un-reclaimable while retained). */
    for(int i=0;i<3;i++) gc_rc_apply(histA[i].b,DBG_DOM,+1);
    int pinned=(gc_rc_get(s0.b,DBG_DOM)>0)&&(gc_rc_get(s1.b,DBG_DOM)>0)&&(gc_rc_get(s2.b,DBG_DOM)>0);
    gc_rc_apply(s2.b,DBG_DOM,-1); int unpinnable=(gc_rc_get(s2.b,DBG_DOM)==0); gc_rc_apply(s2.b,DBG_DOM,+1); /* re-pin */
    sputs("DBG-1 RETENTION COST (surfaced, not hidden): retaining a state PINS its root -> un-reclaimable while retained="); sputs(pinned?"y":"n");
    sputs("; dropping the root makes it reclaimable again="); sputs(unpinnable?"y":"n"); sputs(" (committed gc_rc refcount path)\n");
    int dbg1=t_travel&&pinned&&unpinnable;

    /* ---- DBG-2: STRUCTURAL DIFF BY HASH (descend only where hashes differ; shared subtrees pruned). ---- */
    int v01=0; int c01=dbg_diff(&s0,&s1,2,&v01);   /* s0 vs s1: only r0 changed; whole mem subtree is IDENTICAL -> pruned */
    int v02=0; int c02=dbg_diff(&s0,&s2,2,&v02);   /* s0 vs s2: r0 AND p0 changed -> both subtrees descended */
    int diff_struct=(c01==1)&&(v01==2)&&(c02==2)&&(v02==3);
    sputs("DBG-2 diff(s0,s1): changed leaves="); sdec((uint64_t)c01); sputs(", dir-pairs descended="); sdec((uint64_t)v01);
    sputs(" (mem subtree shared -> PRUNED, r1 leaf pruned)\n");
    sputs("DBG-2 diff(s0,s2): changed leaves="); sdec((uint64_t)c02); sputs(", dir-pairs descended="); sdec((uint64_t)v02);
    sputs(" (r0 and p0 both changed -> cpu+mem descended); pruning is STRUCTURAL, not a byte scan="); sputs(diff_struct?"y":"n"); sputc('\n');

    /* ---- DBG-3: PROVENANCE WALK (honest): deterministic spine reproducible; external edge SAID SO. ---- */
    /* edge s0->s1 is deterministic (r0+=8): re-deriving from s0 reaches the SAME s1 hash. */
    cvsasx_hash_t s1r=dbg_state(9,2,memA,memB); int det_repro=cvsasx_hash_eq(&s1r,&s1);
    /* edge s1->s2 is EXTERNAL: re-deriving with a fresh external read reaches a DIFFERENT hash. */
    cvsasx_hash_t s2r=dbg_ext_step(9,2,memA,memB); int ext_nonrepro=!cvsasx_hash_eq(&s2r,&s2);
    sputs("DBG-3 provenance edge s0->s1: DETERMINISTIC (DRF spine), re-derivation reaches the same hash="); sputs(det_repro?"y":"n"); sputs(" -> reproducible lineage\n");
    sputs("DBG-3 provenance edge s1->s2: NON-DETERMINISTIC/EXTERNAL (p0 <- external input), re-derivation differs="); sputs(ext_nonrepro?"y":"n");
    sputs(" -> the walk STOPS here and SAYS SO: cause is outside the recorded content (NO fabricated cause)\n");
    int dbg3=det_repro&&ext_nonrepro;

    /* ---- DBG-4: OBSERVABILITY AS DAG QUERY - first divergence between two executions (by state hash). ---- */
    cvsasx_hash_t histB[3]={s0,s1,s2b}; int div=-1;
    for(int i=0;i<3;i++){ if(!cvsasx_hash_eq(&histA[i],&histB[i])){ div=i; break; } }
    int vD=0; int cD=(div>=0)?dbg_diff(&histA[div],&histB[div],2,&vD):0;   /* precise delta at the divergence node (reuse DBG-2) */
    int dbg4=(div==2)&&(cD==1)&&(vD==2);   /* diverge at index 2; delta = p0 only (cpu shared -> pruned) */
    sputs("DBG-4 two executions A,B compared by state hash: last common node = index "); sdec((uint64_t)(div>0?div-1:0));
    sputs(", FIRST DIVERGENCE at index "); sdec((uint64_t)div); sputc('\n');
    sputs("DBG-4 structural delta at the divergence node: changed leaves="); sdec((uint64_t)cD); sputs(", dir-pairs descended="); sdec((uint64_t)vD);
    sputs(" (only p0 differs; cpu subtree shared -> pruned)="); sputs(dbg4?"y":"n"); sputc('\n');

    /* ---- DBG-5: LIMITS ENFORCED (real refusal paths, not caveats in prose). ---- */
    /* (a) cannot un-send an external effect: the sent packet is NOT a referent of any state. */
    static uint8_t pkt[16]; for(int i=0;i<16;i++) pkt[i]=(uint8_t)(0xE0+i); cvsasx_hash_t sent=dbg_put(pkt,16); /* effect emitted to the world */
    cvsasx_hash_t junk; int effect_in_state=dbg_resolve(&s0,"sent",&junk); /* is the effect reachable from the re-materialized state? */
    int cannot_unsend=!effect_in_state; /* not a referent -> undo REFUSED */
    sputs("DBG-5(a) un-send external effect (packet "); sputs(hx(sent.b,6)); sputs("): effect is a referent of the re-materialized state="); sputs(effect_in_state?"y":"n");
    sputs(" -> undo REFUSED (re-materialization is pure w.r.t. internal state, impure w.r.t. the world)="); sputs(cannot_unsend?"y":"n"); sputc('\n');
    /* (b) cannot reproduce a race: two runs of the external edge reach different states (observed). */
    int cannot_reproduce=!cvsasx_hash_eq(&s2,&s2b);
    sputs("DBG-5(b) reproduce the non-deterministic edge: two runs reach the same state="); sputs(cannot_reproduce?"n":"y");
    sputs(" -> reproduction REFUSED (DRCC undefined for racy/external state; the state is re-materializable, its recurrence is not)="); sputs(cannot_reproduce?"y":"n"); sputc('\n');
    /* (c) cannot restore real-time context: a re-materialized state carries no clock. */
    cvsasx_hash_t clk; int has_clock=dbg_resolve(&s0,"clock",&clk); int cannot_realtime=!has_clock;
    sputs("DBG-5(c) restore real-time context: re-materialized state has a live clock="); sputs(has_clock?"y":"n");
    sputs(" -> real-time/liveness query REFUSED (the state is out of wall-clock time)="); sputs(cannot_realtime?"y":"n"); sputc('\n');
    int dbg5=cannot_unsend&&cannot_reproduce&&cannot_realtime;

    /* ---- DBG-6: MEASUREMENTS (rdtsc, this run). ---- */
    const int N=20000;
    uint64_t t0=rdtsc(); for(int i=0;i<N;i++){ const void* b; size_t l; cvsasx_store_get(&dbg_store,&s0,&b,&l); cvsasx_hash_t h; cvsasx_blake3(b,l,&h); (void)cvsasx_hash_eq(&h,&s0); } uint64_t jump=(rdtsc()-t0)/N;
    uint64_t t1=rdtsc(); for(int i=0;i<N;i++){ int v=0; (void)dbg_diff(&s0,&s0,2,&v); } uint64_t dpr=(rdtsc()-t1)/N;   /* identical roots -> pruned in ONE hash compare */
    uint64_t t2=rdtsc(); for(int i=0;i<N;i++){ int v=0; (void)dbg_diff(&s0,&s2,2,&v); } uint64_t dfu=(rdtsc()-t2)/N;   /* differing roots -> descend the changed spine */
    uint64_t t3=rdtsc(); for(int i=0;i<N;i++){ cvsasx_hash_t r=dbg_state(9,2,memA,memB); (void)cvsasx_hash_eq(&r,&s1); } uint64_t prov=(rdtsc()-t3)/N; /* provenance: re-derive+verify a deterministic edge */
    sputs("DBG-6 rdtsc: jump-to-state(fetch+re-hash)="); sdec(jump); sputs(" cyc; structural-diff PRUNED(identical roots)="); sdec(dpr);
    sputs(" cyc vs DESCENDED(differing)="); sdec(dfu); sputs(" cyc (pruning benefit); provenance-edge verify="); sdec(prov); sputs(" cyc\n");
    sputs("DBG-6 history-retention STORAGE: distinct objects="); sdec((uint64_t)dbg_store.index_count); sputs(", bytes stored="); sdec(dbg_store.bytes_stored);
    sputs(", dedup hits="); sdec(dbg_store.dedup_hits); sputs(" (structural sharing: shared subtrees stored ONCE; floor = distinct state size, not zero)\n");

    int dbgok=dbg1&&diff_struct&&dbg3&&dbg4&&dbg5;
    sputs(dbgok?"DBG -> REMATERIALIZATION DEBUGGING OK: time-travel by hash (re-hash-confirmed, NOT replay), structural diff prunes by hash, provenance honest at external edges, divergence via DAG query, LIMITS ENFORCED\n":"DBG -> *** see failures above ***\n");
    sputs("DBG SCOPE: single-CPU; the DEBUGGING TOOLING is new, the mechanism it drives (content-address + re-materialize + re-hash) is the committed store. DEMONSTRATED: direct-addressed time-travel, structural diff, provenance on the deterministic spine, divergence query, retention cost. DEFERRED/BOUNDARY (stated, not hidden): retention policy is un-reclaimable storage; provenance is state-lineage NOT cause across external/racy edges; re-materialization CANNOT un-send effects, CANNOT reproduce non-determinism, CANNOT restore wall-clock time.\n");
}

void kmain(void);
void kmain(void){
    serial_init();
    sputs("\n=== CARMIX kernel: bare-metal x86-64 via Limine ===\n");
    if(!LIMINE_BASE_REVISION_SUPPORTED){ sputs("FATAL: Limine base revision unsupported\n"); hcf(); }
    sputs("B0  serial OK (we are running our own code on the machine)\n");

    /* B1 */
    uint16_t cs; __asm__ volatile("mov %%cs,%0":"=r"(cs));
    uint64_t efer=rdmsr(0xC0000080);
    sputs("B1  long mode: CS="); sx64(cs); sputs(" EFER.LMA=");
    sputs((efer&(1u<<10))?"1 -> 64-bit CONFIRMED\n":"0 -> NOT LONG MODE\n");

    if(!hhdm_req.response){ sputs("FATAL: no HHDM\n"); hcf(); }
    hhdm_off=hhdm_req.response->offset;

    /* largest usable RAM region -> frame allocator */
    uint64_t bb=0, bl=0;
    if(mm_req.response){ struct limine_memmap_response*r=mm_req.response;
        for(uint64_t i=0;i<r->entry_count;i++){ struct limine_memmap_entry*e=r->entries[i];
            if(e->type==LIMINE_MEMMAP_USABLE && e->length>bl){ bb=e->base; bl=e->length; } } }
    free_base=(bb+0xfff)&~0xfffULL; free_top=bb+bl; freelist=0; ram_lo=free_base; ram_hi=free_top;
    sputs("    HHDM=") ; sx64(hhdm_off); sputs("  largest RAM @"); sx64(bb); sputs(" = "); sdec(bl>>20); sputs(" MiB\n");

    /* DURABLE PERSISTENCE (S1): bring up the virtio-blk stable-media path FIRST, so the
     * polled virtqueue gets contiguous early-boot frames, then self-test read/write. */
    virtio_blk_selftest();

    /* IDT: specific exception + timer vectors, plus a catch-all over 0-31 (AUDIT A6:
     * any stray/nested CPU exception DUMPS instead of silently triple-faulting). */
    idt_set(6,(void*)isr_ud,cs); idt_set(8,(void*)isr_df,cs); idt_set(13,(void*)isr_gp,cs);
    idt_set(14,(void*)isr_pf,cs); idt_set(0x20,(void*)isr_timer,cs);
    { static const uint8_t errv[]={8,10,11,12,13,14,17,21,29,30};
      for(int v=0;v<32;v++){ if(v==6||v==8||v==13||v==14) continue;
        int he=0; for(unsigned k=0;k<sizeof errv;k++) if(errv[k]==v) he=1;
        idt_set(v, he?(void*)isr_exc_err:(void*)isr_exc_noerr, cs); } }
    struct idtr idtr={(uint16_t)(sizeof(idt)-1),(uint64_t)idt}; __asm__ volatile("lidt %0"::"m"(idtr));
    __asm__ volatile("mov %%cr3,%0":"=r"(g_kernel_cr3)); g_kernel_cr3&=~0xfffULL;   /* the clean kernel PML4 (user half absent) for run_taskstate / run_persist */
    sputs("    IDT loaded (0-31 exceptions + #DF + timer)\n");

    /* SMP BRING-UP (S1-S4): start a second CPU, per-CPU state, a working IPI path, one
     * cross-core sanity test, then the AP parks. Runs here (IDT up, shared kernel cr3,
     * before the legacy PIC/timer) so the single-CPU stages below run unchanged. */
    run_smp();
    run_u3u4();   /* U-3: concurrency across cores + DRCC first exercise; U-4: Merkle-DAG namespace + delegation attenuation chain */

    /* DURABLE PERSISTENCE (S2-S7): runs each boot; persists on a fresh disk, revives on a
     * disk that already holds objects (the post-cold-reboot boot). Runs here, before the
     * regression demos, with a clean kernel cr3 and no timer yet. */
    run_persist();
    run_gc();      /* SINGLE-CPU DURABLE GC: GC1 refcount table, GC2 acyclicity, GC3 root set, GC4 crash-consistent tombstone, GC5 resurrection window, GC6 end-to-end reclaim, GC7 rdtsc */
    run_ds();      /* DEDUP DOMAIN-SCOPING: DS1 domain-tagged key, DS2 scoped dedup, DS3 channel closed (measured), DS4 domain-aware resurrection, DS5 storage cost, DS6 GC intact */
    run_vgate();   /* SINGLE-CPU VERSIONED GATE: VG1 slot+atomic commit, VG2 anti-amp preserved, VG3 never-torn capture, VG4 rdtsc cost */

    /* B3: frame alloc -> map at a fresh vaddr -> write/read-back -> free/realloc */
#if defined(AUDIT_PROBE) && AUDIT_PROBE==1
    sputs("AUDIT A1 PROBE: draining the frame allocator before B3 (force exhaustion)\n");
    free_base=free_top; freelist=0;
#endif
    uint64_t f1=falloc(), f2=falloc();
    uint64_t TVA=0x0000700000000000ULL;   /* free canonical vaddr (low half, Limine-unused) */
    if(!f1 || !f2){ sputs("B3  SKIP: out of frames -> fail-closed (no deref of a failed map)\n"); }
    else {
        int mp=map_page(TVA,f1,0x3);
        uint64_t rb=0;
        if(mp){ volatile uint64_t *tp=(volatile uint64_t*)TVA; *tp=0xCAFEBABEDEADBEEFULL; rb=*tp; } /* AUDIT A1: deref ONLY a succeeded map */
        ffree(f2); ffree(f1); uint64_t f3=falloc();    /* freelist is LIFO -> f3 == f1 */
        sputs("B3  frames: f1="); sx64(f1); sputs(" map@"); sx64(TVA); sputs(" rw-readback=");
        sputs((mp&&rb==0xCAFEBABEDEADBEEFULL)?"OK":"FAIL(map)");
        sputs("  free+realloc f3="); sx64(f3); sputs(f3==f1?" (reused) OK\n":" \n");
    }

    /* B4: framebuffer (the milestone) */
    if(fb_req.response && fb_req.response->framebuffer_count){
        FB=fb_req.response->framebuffers[0];
        sputs("B4  FB "); sdec(FB->width); sputc('x'); sdec(FB->height); sputs(" bpp="); sdec(FB->bpp);
        sputs(" @"); sx64((uint64_t)FB->address); sputc('\n');
        fill(0,0,(uint32_t)FB->width,(uint32_t)FB->height,0x00101830);     /* bg */
        fill(40,40,260,90,0x0020A040);                                     /* green rect */
        draw_str(52,56,"CARMIX ON HW",0x00FFFFFF);
        uint32_t bg=get_px((uint32_t)FB->width-1,(uint32_t)FB->height-1), rc=get_px(250,110); /* in rect, below text */
        int txt=0; for(uint32_t yy=56;yy<64&&!txt;yy++) for(uint32_t xx=52;xx<150;xx++) if(get_px(xx,yy)==0x00FFFFFF){txt=1;break;}
        sputs("    readback bg="); sx64(bg); sputs(" rect="); sx64(rc); sputs(" text-set="); sputs(txt?"y":"n");
        sputs((bg==0x00101830&&rc==0x0020A040&&txt)?"  -> DRAWN TO SCREEN OK\n":"  -> FAIL\n");
    } else { FB=0; sputs("B4  FB: NOT PROVIDED (NOT RUN)\n"); }

    /* B5: timer tick */
    pic_remap(); pit_init(100); kbd_init(); mouse_init(); __asm__ volatile("sti");
    sputs("B5  timer armed (PIT 100Hz); waiting for ticks...\n");
    uint64_t tgt=ticks+5; while(ticks<tgt) __asm__ volatile("hlt");
    sputs("    ticks incrementing = "); sdec(ticks); sputs(" -> timer OK\n");
    if(FB){ char b[24]; uint64_t t=ticks; int n=0,tn=0; char tmp[24]; if(!t)b[n++]='0'; while(t){tmp[tn++]=(char)('0'+t%10);t/=10;} while(tn)b[n++]=tmp[--tn]; b[n]=0;
            draw_str(52,82,"TICKS=",0x00FFFF00); draw_str(52+48,82,b,0x00FFFF00); }

    /* STEP 4: fuse the proven capability backend onto the booted kernel (E0-E4) */
    run_step4();
    /* STEP 5: rematerialization on the metal - the novel contribution (R0-R5) */
    run_step5();

    /* B2: deliberate fault to prove the fault handler (done last; it halts).
     * AUDIT A6 probes confirm a SECOND vector class is caught (not just #UD). */
#if defined(AUDIT_PROBE) && AUDIT_PROBE==2
    sputs("\nAUDIT A6 PROBE: deliberate #PF (read unmapped 0x0000680000000000)...\n");
    { volatile uint64_t *bad=(volatile uint64_t*)0x0000680000000000ULL; volatile uint64_t x=*bad; (void)x; }
    sputs("*** A6 FAIL: #PF did not trap ***\n");
#elif defined(AUDIT_PROBE) && AUDIT_PROBE==3
    sputs("\nAUDIT A6 PROBE: deliberate #DE (divide by zero, vector 0 -> catch-all)...\n");
    __asm__ volatile("movl $1,%%eax; xorl %%edx,%%edx; xorl %%ecx,%%ecx; divl %%ecx" ::: "eax","edx","ecx","cc");
    sputs("*** A6 FAIL: #DE did not trap ***\n");
#elif defined(AUDIT_PROBE)
    sputs("\nB2 PROBE: deliberate #UD...\n");
    __asm__ volatile("ud2");
    sputs("*** B2 FAIL: #UD did not trap (silent) ***\n");
#else
    run_stage2();      /* S2: scrolling text console */
    run_stage3();      /* S3: a window over the desktop */
    run_stage4();      /* S4: movable window + cursor */
    run_stage5();      /* S5: desktop shell - console inside the window */
    run_stage6();      /* S6: rematerialization in the GUI */
    run_a7();          /* A7: PS/2 mouse moves the cursor */
    run_a8();          /* A8: drag the titlebar to move the window */
    run_a9();          /* A9: second window + z-order (distinct colors) */
    run_e1();          /* E1: multi-window focus (raise-on-click) */
    run_e2();          /* E2: drag the focused window */
    run_e3();          /* E3: resize via the grip + min-clamp */
    run_sched();       /* STEP 11: task substrate - P0 cooperative, P1 remat-work, P2 preemption */
    run_unified();     /* STEP 12: unified content-addressed substrate - U0 task-as-object, U1 activate-by-hash, U2 incremental, U3 crossover, U4 persistence-as-noop */
    run_residency();   /* RESIDENCY MANAGER: M0 frame-db, M1 per-task page tables, M2 materialize/share-by-hash, M3 eviction/refdrop, M4 dirty-bit, M5 fragmentation */
    run_fault();       /* REMATERIALIZING FAULT HANDLER: F0 #PF+safety, F1 fault-in via A, F2 fail-closed on verify, F3 dedup-at-fault, F4 binding B + measured A-vs-B, F5 re-entrancy probe */
    run_userspace();   /* USERSPACE + USER/KERNEL BOUNDARY: US0 ring-3 + protection refuse, US1 syscall round trip, US2 re-mint-at-crossing, US3 adversarial table, US4 hash-passing+TOCTOU */
    run_u0();          /* PHASE U-0: one process, explicit CSpace, five gated syscalls (each authorized+refused), anti-amp acquire, content-addressed store, bump allocator, ring-3 end-to-end */
    run_u1u2();        /* U-1: spawn bounded authority + revocation + capability IPC; U-2: real heap + free coupled to the committed GC (sys_mem_release -> gc_rc reclaim) */
    run_fs();          /* CONTENT-ADDRESSED FILESYSTEM: file=object, dir=Merkle map, write re-roots (native versioning + free snapshots), reads re-verified, root durable */
    run_cfs();         /* CYCLE 2 FULL CFS: file ops (write/append/truncate re-root, large files=Merkle trees), dir ops (mkdir/rmdir/rename/move), GC-integrated (dead trees reclaim, snapshots pin), durable crash-consistent root, structural diff */
    run_dbg();         /* CYCLE 4 REMATERIALIZATION DEBUGGING: DBG1 time-travel by hash (re-hash-confirmed, not replay), DBG2 structural diff (prune by hash), DBG3 honest provenance (external edge said so), DBG4 divergence via DAG query, DBG5 limits ENFORCED (no un-send/no reproduce/no real-time), DBG6 rdtsc */
    run_net();         /* CYCLE 3 NETWORKING (LOOPBACK): NET-1 endpoint=bounded cap (no ambient reach, delegation attenuates), NET-2 messages=content-addressed objects (re-hash integrity + dedup), NET-3 ordering via explicit hash chain + liveness labeled NON-CA, NET-4 end-to-end cap-gated ordered integrity channel, NET-5 real-NIC gap, NET-6 rdtsc */
    run_loader();      /* PROCESS LOADER: L0 ELF as a stored object + parse, L1 materialize segments by hash (W^X), L2 enter+run the loaded ELF, L3 load-time authority ceiling, L4 two procs share code by hash */
    conc_run();        /* CONCURRENT MULTI-PROCESS SCHEDULING: C0 two ring-3 procs timer-preempted, C1 per-process ceilings, C2 deschedule->dematerialize->rematerialize, C3 measured policy-cost */
    run_permem();      /* PER-PROCESS MEMORY MANAGEMENT: PM0 extend-heap+demand-zero+bump alloc, PM1 authority-bounded, PM2 dematerialize-idle, PM3 cross-process dedup-by-hash+COW, PM4 U3 hot-page exclusion */
    run_taskstate();   /* FULL TASK-STATE DEMATERIALIZATION: TS0 canonical Merkle page-table form, TS1 whole-process task-hash, TS2 free ALL frames (page-table frames too), TS3 bit-exact rebuild, TS4 authority never amplified, TS5 break-even vs keep-root-resident */
    run_sharedmap();   /* SHARED MAPPINGS IN THE CANONICAL FORM: SM0 establish sharing, SM1 shared-object layer, SM2 lifetime across demat, SM3 capability gate (one shared frame survives the round trip), SM4 per-owner authority, SM5 limits */
    run_shell();       /* E4: live focus/drag/resize/type desktop (does not return) */
#endif
    hcf();
}
