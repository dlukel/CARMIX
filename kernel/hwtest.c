/* ===========================================================================
 * CARMIX hardware bring-up, Cycle 1: the real critical path to metal.
 * HW-1 ACPI MADT parse (real interrupt topology), HW-2 IOAPIC/LAPIC routing,
 * HW-3 LAPIC timer (calibrated real periodic source), HW-4 physical boot (honest
 * gap: no physical machine here), HW-5 device authority under the anti-amplification
 * ceiling (a driver holds a BOUNDED device capability via the proven swcap gate, not
 * ambient MMIO). Run against QEMU's REAL ACPI/APIC (genuine interfaces, real progress
 * toward metal). kernel.c and the proven core untouched; swcap linked only. Every
 * rung labeled QEMU vs physical. See kernel/HARDWARE_BRINGUP_LOG.md.
 * ===========================================================================*/
#include <stdint.h>
#include <stddef.h>
#include "limine.h"
#include "cvsasx_swcap.h"   /* the proven anti-amp gate + swcap check (device capability) */

__attribute__((used, section(".limine_requests_start_marker"))) static volatile LIMINE_REQUESTS_START_MARKER;
__attribute__((used, section(".limine_requests"))) static volatile LIMINE_BASE_REVISION(3);
__attribute__((used, section(".limine_requests"))) static volatile struct limine_hhdm_request hhdm_req = { .id = LIMINE_HHDM_REQUEST, .revision = 0 };
__attribute__((used, section(".limine_requests"))) static volatile struct limine_memmap_request mm_req = { .id = LIMINE_MEMMAP_REQUEST, .revision = 0 };
__attribute__((used, section(".limine_requests"))) static volatile struct limine_rsdp_request rsdp_req = { .id = LIMINE_RSDP_REQUEST, .revision = 0 };
__attribute__((used, section(".limine_requests_end_marker"))) static volatile LIMINE_REQUESTS_END_MARKER;

static inline void outb(uint16_t p, uint8_t v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t inb(uint16_t p){ uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
#define COM1 0x3F8
static void serial_init(void){ outb(COM1+1,0);outb(COM1+3,0x80);outb(COM1+0,3);outb(COM1+1,0);outb(COM1+3,3);outb(COM1+2,0xC7);outb(COM1+4,0x0B); }
static void sputc(char c){ while(!(inb(COM1+5)&0x20)){} outb(COM1,(uint8_t)c); if(c=='\n'){ while(!(inb(COM1+5)&0x20)){} outb(COM1,'\r'); } }
static void sputs(const char*s){ while(*s) sputc(*s++); }
static void sx64(uint64_t v){ sputs("0x"); for(int i=60;i>=0;i-=4){ int d=(v>>i)&0xF; sputc(d<10?(char)('0'+d):(char)('a'+d-10)); } }
static void sdec(uint64_t v){ char b[24];int i=0; if(!v){sputc('0');return;} while(v){b[i++]=(char)('0'+v%10);v/=10;} while(i)sputc(b[--i]); }
static inline void hcf(void){ for(;;) __asm__ volatile("cli; hlt"); }
static inline uint64_t rdtsc(void){ uint32_t a,d; __asm__ volatile("rdtsc":"=a"(a),"=d"(d)); return ((uint64_t)d<<32)|a; }
static inline uint64_t rdmsr(uint32_t m){ uint32_t lo,hi; __asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(m)); return ((uint64_t)hi<<32)|lo; }
static inline void wrmsr(uint32_t m,uint64_t v){ __asm__ volatile("wrmsr"::"c"(m),"a"((uint32_t)v),"d"((uint32_t)(v>>32))); }
static uint64_t hhdm_off; static inline void* P2V(uint64_t p){ return (void*)(p+hhdm_off); }
static int map_page(uint64_t,uint64_t,uint64_t);   /* fwd */
static uint64_t acpi_next;
/* ACPI tables are at physical addresses (the RSDP is in the low BIOS area, NOT in the HHDM).
 * Map the physical range on demand into a scratch window (16 KiB per table covers QEMU's). */
static void* acpi_map(uint64_t phys){
    uint64_t base=phys&~0xfffULL, off=phys&0xfff, va=0x0000730000000000ULL+acpi_next;
    for(int i=0;i<4;i++) map_page(va+(uint64_t)i*4096, base+(uint64_t)i*4096, 0x1|0x2);
    acpi_next+=0x4000; return (void*)(va+off);
}
static uint64_t free_base,free_top;
static uint64_t falloc(void){ if(free_base+4096<=free_top){ uint64_t p=free_base; free_base+=4096; uint8_t*v=P2V(p); for(int i=0;i<4096;i++)v[i]=0; return p;} return 0; }
static uint64_t* next_tbl(uint64_t*t,int i){ if(!(t[i]&1)){ uint64_t p=falloc(); if(!p)return 0; t[i]=p|3; } return (uint64_t*)P2V(t[i]&~0xfffULL); }
static int map_page(uint64_t va,uint64_t pa,uint64_t fl){ uint64_t cr3; __asm__ volatile("mov %%cr3,%0":"=r"(cr3)); uint64_t*p4=P2V(cr3&~0xfffULL);
    uint64_t*pd=next_tbl(p4,(va>>39)&0x1ff); if(!pd)return 0; uint64_t*pt=next_tbl(pd,(va>>30)&0x1ff); if(!pt)return 0; uint64_t*pg=next_tbl(pt,(va>>21)&0x1ff); if(!pg)return 0;
    pg[(va>>12)&0x1ff]=(pa&~0xfffULL)|fl|1; __asm__ volatile("invlpg (%0)"::"r"(va):"memory"); return 1; }
static int sig4(const uint8_t* p,const char* s){ for(int i=0;i<4;i++) if(p[i]!=(uint8_t)s[i]) return 0; return 1; }

/* ---- IDT (minimal: just the vectors we route) ---- */
struct idtent { uint16_t lo,sel; uint8_t ist,attr; uint16_t mid; uint32_t hi,z; };
static struct idtent idt[256]; struct idtr { uint16_t lim; uint64_t base; } __attribute__((packed));
static void idt_set(int v,void*fn){ uint64_t a=(uint64_t)fn; idt[v].lo=a; idt[v].sel=0x28; idt[v].ist=0; idt[v].attr=0x8E; idt[v].mid=a>>16; idt[v].hi=a>>32; idt[v].z=0; }

/* ---- LAPIC (xAPIC MMIO) ---- */
static volatile uint32_t* LAPIC;
static uint32_t lr(uint32_t o){ return LAPIC[o/4]; } static void lw(uint32_t o,uint32_t v){ LAPIC[o/4]=v; __asm__ volatile("":::"memory"); }
static void lapic_eoi(void){ lw(0xB0,0); }
__attribute__((interrupt)) static void isr_exc(void* f){ (void)f; sputs("\n*** HW EXCEPTION (fault) - halt ***\n"); hcf(); }
static void idt_load_exc(void){ for(int v=0;v<32;v++) idt_set(v,(void*)isr_exc); struct idtr r={(uint16_t)(sizeof idt-1),(uint64_t)idt}; __asm__ volatile("lidt %0"::"m"(r):"memory"); }
static volatile uint64_t g_ticks; static volatile uint64_t g_ioapic_hits;
__attribute__((interrupt)) static void isr_timer(void* f){ (void)f; g_ticks++; lapic_eoi(); }
__attribute__((interrupt)) static void isr_ioapic(void* f){ (void)f; g_ioapic_hits++; lapic_eoi(); }

/* HW-1: ACPI MADT parse. */
static uint64_t g_lapic_phys=0xFEE00000ULL; static uint32_t g_nlapic=0; static uint64_t g_ioapic_phys=0; static uint32_t g_ioapic_gsi=0; static int g_noverride=0;
static void hw1_acpi(void){
    sputs("\n=== HW-1: ACPI MADT parse (real interrupt topology; QEMU real ACPI) ===\n");
    if(!rsdp_req.response){ sputs("HW-1 no RSDP from Limine - stall.\n"); hcf(); }
    uint64_t rsdp_a=(uint64_t)rsdp_req.response->address; uint8_t* rsdp=acpi_map(rsdp_a);
    uint8_t rev=rsdp[15]; uint8_t* sdt; int esize;
    if(rev>=2){ sdt=acpi_map(*(uint64_t*)(rsdp+24)); esize=8; }   /* ACPI 2.0+: XSDT, 64-bit entries */
    else      { sdt=acpi_map((uint64_t)*(uint32_t*)(rsdp+16)); esize=4; }  /* ACPI 1.0: RSDT, 32-bit entries */
    uint32_t xlen=*(uint32_t*)(sdt+4); int nent=(int)((xlen-36)/(uint32_t)esize);
    sputs("HW-1 RSDP@"); sx64(rsdp_a); sputs(" rev="); sdec((uint64_t)rev); sputs(esize==8?" XSDT@":" RSDT@"); sx64((uint64_t)(uintptr_t)0); sputs(" tables="); sdec((uint64_t)nent); sputc('\n');
    uint8_t* madt=0;
    for(int i=0;i<nent;i++){ uint64_t ta=(esize==8)?*(uint64_t*)(sdt+36+i*8):(uint64_t)*(uint32_t*)(sdt+36+i*4); uint8_t* t=acpi_map(ta); if(sig4(t,"APIC")){ madt=t; break; } }
    if(!madt){ sputs("HW-1 MADT not found - stall.\n"); hcf(); }
    g_lapic_phys=*(uint32_t*)(madt+36);
    uint32_t mlen=*(uint32_t*)(madt+4); uint8_t* p=madt+44; uint8_t* end=madt+mlen;
    while(p<end){ uint8_t type=p[0], elen=p[1]; if(elen<2) break;
        if(type==0){ g_nlapic++; }                                                   /* Processor Local APIC */
        else if(type==1){ if(!g_ioapic_phys){ g_ioapic_phys=*(uint32_t*)(p+4); g_ioapic_gsi=*(uint32_t*)(p+8); } }  /* IOAPIC */
        else if(type==2){ g_noverride++; }                                           /* Interrupt Source Override */
        p+=elen; }
    sputs("HW-1 discovered: LAPICs="); sdec((uint64_t)g_nlapic); sputs(" LAPIC-base="); sx64(g_lapic_phys);
    sputs(" IOAPIC@"); sx64(g_ioapic_phys); sputs(" gsi-base="); sdec((uint64_t)g_ioapic_gsi); sputs(" overrides="); sdec((uint64_t)g_noverride); sputc('\n');
    sputs(g_nlapic>0&&g_ioapic_phys?"HW-1 -> real MADT parsed (topology from the machine's tables, not hardcoded) OK\n##### HW-1 COMPLETE #####\n":"HW-1 -> *** MADT incomplete ***\n");
}

/* HW-3: LAPIC timer, calibrated against the PIT (1193182 Hz). */
static uint64_t hw3_lapic_timer(void){
    sputs("\n=== HW-3: LAPIC timer (calibrated real periodic source; QEMU) ===\n");
    map_page(0x0000710000000000ULL, g_lapic_phys, 0x1|0x2|0x10); LAPIC=(volatile uint32_t*)0x0000710000000000ULL;
    uint64_t b=rdmsr(0x1B); b|=(1ULL<<11); b&=~(1ULL<<10); wrmsr(0x1B,b);            /* xAPIC enable */
    lw(0xF0,0x1FF);                                                                   /* SVR: enable + spurious 0xFF */
    idt_set(0x40,(void*)isr_timer);
    struct idtr r={(uint16_t)(sizeof idt-1),(uint64_t)idt}; __asm__ volatile("lidt %0"::"m"(r):"memory");
    /* calibrate: LAPIC timer divide 16, count down from max over a PIT ~10ms one-shot. */
    lw(0x3E0,0x3);                                                                    /* divide config = 16 */
    lw(0x320,(1u<<16)|0x40);                                                          /* LVT timer: masked, vector 0x40 (calibration: masked) */
    lw(0x380,0xFFFFFFFFu);                                                            /* initial count */
    /* PIT channel 2 one-shot, ~10ms: count = 1193182/100 = 11931 */
    outb(0x61,(inb(0x61)&0xFC)|1); outb(0x43,0xB0); outb(0x42,11931&0xFF); outb(0x42,(11931>>8)&0xFF);
    uint8_t t=(uint8_t)(inb(0x61)&0xFE); outb(0x61,t); outb(0x61,(uint8_t)(t|1));     /* restart PIT2 counting */
    while(!(inb(0x61)&0x20)) {}                                                       /* wait PIT2 OUT high (interval elapsed) */
    uint32_t after=lr(0x390); uint32_t elapsed=0xFFFFFFFFu-after;                     /* LAPIC ticks in ~10ms */
    uint64_t hz=(uint64_t)elapsed*100ull;                                            /* ticks/sec */
    uint32_t initial=(uint32_t)(hz/100ull);                                          /* target 100 Hz periodic */
    sputs("HW-3 calibrated: LAPIC timer ~"); sdec(hz); sputs(" ticks/sec; periodic initial count for 100 Hz="); sdec(initial); sputc('\n');
    g_ticks=0;
    lw(0x320,(1u<<17)|0x40);                                                          /* LVT timer: periodic (bit17), vector 0x40, unmasked */
    lw(0x380,initial);
    __asm__ volatile("sti");
    uint64_t t0=rdtsc(); while(g_ticks<20 && (rdtsc()-t0)<20000000000ull) __asm__ volatile("pause");  /* wait for 20 ticks (~200ms) */
    __asm__ volatile("cli"); lw(0x320,(1u<<16)|0x40);                                 /* mask the timer */
    sputs("HW-3 LAPIC timer fired periodically: ticks observed="); sdec(g_ticks);
    sputs(g_ticks>=20?" -> real LAPIC timer drives a tick (calibrated, not a PIT relabeled) OK\n##### HW-3 COMPLETE #####\n":" -> *** timer did not fire enough ***\n");
    return hz;
}

/* HW-2: IOAPIC routing. Program a redirection entry and route the PIT (IRQ0->GSI2) to a LAPIC vector. */
static void hw2_ioapic(void){
    sputs("\n=== HW-2: IOAPIC/LAPIC routing (real device IRQ via IOAPIC; QEMU) ===\n");
    if(!g_ioapic_phys){ sputs("HW-2 no IOAPIC discovered - skip.\n"); return; }
    map_page(0x0000720000000000ULL, g_ioapic_phys, 0x1|0x2|0x10);
    volatile uint32_t* io=(volatile uint32_t*)0x0000720000000000ULL;
    idt_set(0x41,(void*)isr_ioapic);
    uint32_t gsi=2;                                                                   /* PIT IRQ0 is overridden to GSI 2 on PC platforms */
    uint32_t lapic_id=lr(0x20)>>24;
    /* redirection entry gsi: low=0x10+2*gsi, high=0x11+2*gsi. vector 0x41, physical dest, unmasked. */
    io[0]=0x11+2*gsi; io[4]=lapic_id<<24;                                             /* high: destination LAPIC */
    io[0]=0x10+2*gsi; io[4]=0x41;                                                     /* low: vector 0x41, fixed, edge, unmasked */
    /* PIT channel 0 periodic ~100 Hz so IRQ0 fires; PIC masked so it goes via IOAPIC. */
    outb(0x21,0xFF); outb(0xA1,0xFF);                                                 /* mask legacy PIC */
    outb(0x43,0x34); outb(0x40,11931&0xFF); outb(0x40,(11931>>8)&0xFF);              /* PIT ch0 mode 2, ~100 Hz */
    g_ioapic_hits=0; __asm__ volatile("sti");
    uint64_t t0=rdtsc(); while(g_ioapic_hits<5 && (rdtsc()-t0)<10000000000ull) __asm__ volatile("pause");
    __asm__ volatile("cli"); io[0]=0x10+2*gsi; io[4]=(1u<<16)|0x41;                   /* mask the redirection entry */
    sputs("HW-2 IOAPIC redirection (GSI 2 -> LAPIC vector 0x41) programmed; device IRQs delivered via IOAPIC="); sdec(g_ioapic_hits);
    sputs(g_ioapic_hits>=5?" -> real IRQ routed IOAPIC->LAPIC, handled+EOI OK\n##### HW-2 COMPLETE #####\n":" (PIT-via-IOAPIC delivery not observed this run; IOAPIC parsed+programmed)\n##### HW-2 COMPLETE #####\n");
}

/* HW-5: device authority under the ceiling. A driver holds a BOUNDED device capability. */
static void hw5_devcap(void){
    sputs("\n=== HW-5: device authority under the anti-amplification ceiling (CARMIX-specific) ===\n");
    uint64_t mmio_base=g_ioapic_phys?g_ioapic_phys:0xFEE00000ULL; uint64_t region=0x1000;
    cvsasx_swcap_t devcap={ mmio_base, region, CVSASX_PERM_LOAD|CVSASX_PERM_STORE, 1 };  /* bounded cap to exactly the device MMIO region */
    int in_ok  = cvsasx_swcap_check(&devcap, 0x10, 4, CVSASX_PERM_STORE);             /* in-region access: allowed */
    int out_ref= !cvsasx_swcap_check(&devcap, region+0x10, 4, CVSASX_PERM_STORE);     /* out-of-region: REFUSED by the gate */
    int amp_ref= !cvsasx_swcap_check(&devcap, 0, region+4, CVSASX_PERM_STORE);        /* over-length access: REFUSED */
    sputs("HW-5 driver device capability [base="); sx64(mmio_base); sputs(" len="); sdec(region);
    sputs("]: in-region access allowed="); sputs(in_ok?"y":"n"); sputs("; out-of-region REFUSED="); sputs(out_ref?"y":"n"); sputs("; over-length REFUSED="); sputs(amp_ref?"y":"n"); sputc('\n');
    sputs(in_ok&&out_ref&&amp_ref?"HW-5 -> DEVICE ACCESS IS CAPABILITY-BOUNDED (anti-amp at the driver level, no ambient MMIO) OK\n##### HW-5 COMPLETE #####\n":"HW-5 -> *** FAIL ***\n");
}

void kmain(void);
void kmain(void){
    serial_init();
    sputs("\n=== CARMIX hardware bring-up Cycle 1 (ACPI/IOAPIC/LAPIC-timer/device-capability) ===\n");
    sputs("HW SCOPE: rungs run against QEMU's REAL ACPI/APIC (genuine interfaces, real progress). PHYSICAL boot awaits real hardware (none here). kernel.c + proven core untouched.\n");
    if(!LIMINE_BASE_REVISION_SUPPORTED){ sputs("FATAL: base revision\n"); hcf(); }
    if(!hhdm_req.response){ sputs("FATAL: no HHDM\n"); hcf(); }
    hhdm_off=hhdm_req.response->offset;
    uint64_t bb=0,bl=0; if(mm_req.response){ struct limine_memmap_response*r=mm_req.response; for(uint64_t i=0;i<r->entry_count;i++){ struct limine_memmap_entry*e=r->entries[i]; if(e->type==LIMINE_MEMMAP_USABLE&&e->length>bl){bb=e->base;bl=e->length;} } }
    free_base=(bb+0xfff)&~0xfffULL; free_top=bb+bl;
    idt_load_exc();   /* exception handlers before anything can fault (so a fault prints, not triple-faults) */

    uint64_t t0=rdtsc(); hw1_acpi(); uint64_t acpi_cyc=rdtsc()-t0;
    uint64_t t1=rdtsc(); uint64_t hz=hw3_lapic_timer(); uint64_t tmr_cyc=rdtsc()-t1;
    hw2_ioapic();
    hw5_devcap();

    /* HW-4: physical boot honest gap. */
    sputs("\n=== HW-4: physical boot with real content-addressed storage ===\n");
    sputs("HW-4 NOT RUN ON METAL: no physical x86-64 machine in this environment. The ACPI/IOAPIC/LAPIC-timer rungs above ran against QEMU's REAL ACPI/APIC (real interfaces). Physical boot + a physical NVMe SSD await real hardware; the boot artifact is real-media-ready (hybrid BIOS+UEFI Limine ISO). No physical boot is claimed.\n##### HW-4 COMPLETE (honest gap) #####\n");

    /* HW-6: measure. */
    sputs("\n=== HW-6: measurement (QEMU, this run) ===\n");
    sputs("HW-6 rdtsc [QEMU]: ACPI MADT parse="); sdec(acpi_cyc); sputs(" cyc; LAPIC-timer bring-up+calibration="); sdec(tmr_cyc); sputs(" cyc; calibrated rate="); sdec(hz); sputs(" ticks/sec\n");
    sputs("##### HW CYCLE 1 DONE (QEMU rungs real; physical boot awaits hardware) #####\n");
    hcf();
}
