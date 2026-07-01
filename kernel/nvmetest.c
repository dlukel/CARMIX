/* ===========================================================================
 * CARMIX hardware bring-up, rung (b): a real NVMe controller driver, doing
 * content-addressed write/read/re-verify against the REAL NVMe register and queue
 * interface. Tested against QEMU's -device nvme, which implements the NVMe spec, so
 * this is the real controller interface, NOT virtio. It is EMULATOR-TESTED, not
 * physical-NVMe-tested: running it on a physical NVMe SSD is the next step and needs
 * physical hardware this environment does not have. kernel.c and the proven core are
 * untouched; store/ + blake3/ are LINKED only. See kernel/HARDWARE_BRINGUP_LOG.md.
 * ===========================================================================*/
#include <stdint.h>
#include <stddef.h>
#include "limine.h"
#include "cvsasx_store.h"   /* cvsasx_blake3, cvsasx_hash_t, cvsasx_hash_eq (LINKED) */

__attribute__((used, section(".limine_requests_start_marker")))
static volatile LIMINE_REQUESTS_START_MARKER;
__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);
__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_req = { .id = LIMINE_HHDM_REQUEST, .revision = 0 };
__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request mm_req = { .id = LIMINE_MEMMAP_REQUEST, .revision = 0 };
__attribute__((used, section(".limine_requests_end_marker")))
static volatile LIMINE_REQUESTS_END_MARKER;

/* ---- serial + helpers (from nettest.c) ---- */
static inline void outb(uint16_t p, uint8_t v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t inb(uint16_t p){ uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void outl(uint16_t p, uint32_t v){ __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline uint32_t inl(uint16_t p){ uint32_t v; __asm__ volatile("inl %1,%0":"=a"(v):"Nd"(p)); return v; }
#define COM1 0x3F8
static void serial_init(void){ outb(COM1+1,0x00); outb(COM1+3,0x80); outb(COM1+0,0x03); outb(COM1+1,0x00); outb(COM1+3,0x03); outb(COM1+2,0xC7); outb(COM1+4,0x0B); }
static void sputc(char c){ while(!(inb(COM1+5)&0x20)){} outb(COM1,(uint8_t)c); if(c=='\n'){ while(!(inb(COM1+5)&0x20)){} outb(COM1,'\r'); } }
static void sputs(const char*s){ while(*s) sputc(*s++); }
static void sx64(uint64_t v){ sputs("0x"); for(int i=60;i>=0;i-=4){ int d=(v>>i)&0xF; sputc(d<10?(char)('0'+d):(char)('a'+d-10)); } }
static void sdec(uint64_t v){ char b[24]; int i=0; if(!v){sputc('0');return;} while(v){b[i++]=(char)('0'+v%10);v/=10;} while(i)sputc(b[--i]); }
static char hexbuf[80]; static const char* hx(const uint8_t*b,int n){ const char*d="0123456789abcdef"; int j=0; for(int i=0;i<n;i++){ hexbuf[j++]=d[b[i]>>4]; hexbuf[j++]=d[b[i]&15]; } hexbuf[j]=0; return hexbuf; }
static inline void hcf(void){ for(;;) __asm__ volatile("cli; hlt"); }
static inline void cpu_pause(void){ __asm__ volatile("pause"); }
static inline void mfence(void){ __asm__ volatile("mfence":::"memory"); }
static inline uint64_t rdtsc(void){ uint32_t a,d; __asm__ volatile("rdtsc":"=a"(a),"=d"(d)); return ((uint64_t)d<<32)|a; }
static uint64_t hhdm_off; static inline void* P2V(uint64_t p){ return (void*)(p+hhdm_off); }

/* ---- PCI config + frame alloc + page map (from nettest.c) ---- */
static uint32_t pci_cfg_rd(uint8_t bus,uint8_t dev,uint8_t fn,uint8_t off){ uint32_t a=0x80000000u|((uint32_t)bus<<16)|((uint32_t)dev<<11)|((uint32_t)fn<<8)|(off&0xFC); outl(0xCF8,a); return inl(0xCFC); }
static void pci_cfg_wr(uint8_t bus,uint8_t dev,uint8_t fn,uint8_t off,uint32_t v){ uint32_t a=0x80000000u|((uint32_t)bus<<16)|((uint32_t)dev<<11)|((uint32_t)fn<<8)|(off&0xFC); outl(0xCF8,a); outl(0xCFC,v); }
static uint64_t free_base, free_top;
static uint64_t falloc(void){ if(free_base+4096<=free_top){ uint64_t p=free_base; free_base+=4096; uint8_t*v=P2V(p); for(int i=0;i<4096;i++) v[i]=0; return p; } return 0; }
static uint64_t* next_tbl(uint64_t*t,int i){ if(!(t[i]&1)){ uint64_t p=falloc(); if(!p) return 0; t[i]=p|0x3; } return (uint64_t*)P2V(t[i]&~0xfffULL); }
static int map_page(uint64_t va,uint64_t pa,uint64_t flags){ uint64_t cr3; __asm__ volatile("mov %%cr3,%0":"=r"(cr3)); uint64_t*pml4=P2V(cr3&~0xfffULL);
    uint64_t*pdpt=next_tbl(pml4,(va>>39)&0x1ff); if(!pdpt)return 0; uint64_t*pd=next_tbl(pdpt,(va>>30)&0x1ff); if(!pd)return 0; uint64_t*pt=next_tbl(pd,(va>>21)&0x1ff); if(!pt)return 0;
    pt[(va>>12)&0x1ff]=(pa&~0xfffULL)|flags|1; __asm__ volatile("invlpg (%0)"::"r"(va):"memory"); return 1; }

/* ===========================================================================
 * NVMe controller driver (real register + queue interface).
 * ===========================================================================*/
#define NVME_CAP  0x00
#define NVME_CC   0x14
#define NVME_CSTS 0x1C
#define NVME_AQA  0x24
#define NVME_ASQ  0x28
#define NVME_ACQ  0x30
#define NVQ_DEPTH 64u
#define LBA_BYTES 512u

static volatile uint8_t* NVBAR;                 /* mapped NVMe BAR0 (MMIO, uncached) */
static uint32_t nvr32(uint32_t o){ return *(volatile uint32_t*)(NVBAR+o); }
static void     nvw32(uint32_t o,uint32_t v){ *(volatile uint32_t*)(NVBAR+o)=v; mfence(); }
static uint64_t nvr64(uint32_t o){ return *(volatile uint64_t*)(NVBAR+o); }
static void     nvw64(uint32_t o,uint64_t v){ *(volatile uint64_t*)(NVBAR+o)=v; mfence(); }

typedef struct { volatile uint32_t* sq; volatile uint32_t* cq; volatile uint32_t* sqdb; volatile uint32_t* cqdb;
                 uint32_t sqt, cqh, phase; } nvq_t;
static nvq_t admq, ioq;
static uint16_t g_cid=0;

/* submit a 64-byte command to q, ring the SQ doorbell, poll the CQ (phase bit),
 * advance + ring the CQ head doorbell. Returns the NVMe status field (0 = success). */
static int nvme_cmd(nvq_t* q, const uint32_t cmd[16]){
    for(int i=0;i<16;i++) q->sq[q->sqt*16+i]=cmd[i];
    q->sqt=(q->sqt+1)%NVQ_DEPTH; mfence(); *q->sqdb=q->sqt; mfence();
    uint64_t budget=2000000000ULL;
    while(budget--){ uint32_t dw3=q->cq[q->cqh*4+3];
        if(((dw3>>16)&1u)==q->phase){ int status=(int)((dw3>>17)&0x7FFFu);
            q->cqh++; if(q->cqh==NVQ_DEPTH){ q->cqh=0; q->phase^=1u; } mfence(); *q->cqdb=q->cqh; mfence(); return status; }
        cpu_pause(); }
    return -1;
}
static void cmd_zero(uint32_t c[16]){ for(int i=0;i<16;i++) c[i]=0; }

static int nvme_init(void){
    /* PCI: find the NVMe controller (class 01h subclass 08h progif 02h). */
    uint8_t nb=0,nd=0,nf=0; uint64_t bar0=0;
    for(uint16_t bus=0;bus<256&&!bar0;bus++) for(uint8_t dev=0;dev<32&&!bar0;dev++) for(uint8_t fn=0;fn<8;fn++){
        uint32_t id=pci_cfg_rd((uint8_t)bus,dev,fn,0x00); if((id&0xFFFF)==0xFFFF) continue;
        uint32_t cc=pci_cfg_rd((uint8_t)bus,dev,fn,0x08);   /* class(31:24) subclass(23:16) progif(15:8) */
        if(((cc>>24)&0xFF)==0x01 && ((cc>>16)&0xFF)==0x08){
            uint32_t b0=pci_cfg_rd((uint8_t)bus,dev,fn,0x10), b1=pci_cfg_rd((uint8_t)bus,dev,fn,0x14);
            bar0=((uint64_t)b1<<32)|(uint64_t)(b0&~0xFu);
            uint32_t cmd=pci_cfg_rd((uint8_t)bus,dev,fn,0x04); pci_cfg_wr((uint8_t)bus,dev,fn,0x04,cmd|0x6); /* MEM+BUSMASTER */
            nb=(uint8_t)bus; nd=dev; nf=fn; break; }
    }
    if(!bar0){ sputs("HW-B NVMe controller (class 0x0108) NOT FOUND - stall.\n"); return 0; }
    sputs("HW-B NVMe found bus="); sdec(nb); sputs(" dev="); sdec(nd); sputs(" fn="); sdec(nf); sputs(" BAR0="); sx64(bar0); sputc('\n');
    /* map BAR0 uncached (PCD) at a fixed VA */
    uint64_t NVV=0x0000680000000000ULL;
    for(uint64_t o=0;o<0x4000;o+=4096) if(!map_page(NVV+o,bar0+o,0x1|0x2|0x10)){ sputs("HW-B BAR0 map FAILED\n"); return 0; }
    NVBAR=(volatile uint8_t*)NVV;
    uint64_t cap=nvr64(NVME_CAP); uint32_t dstrd=(uint32_t)((cap>>32)&0xF); uint32_t stride=4u<<dstrd;
    sputs("HW-B NVMe CAP="); sx64(cap); sputs(" DSTRD="); sdec(dstrd); sputs(" MQES="); sdec((cap&0xFFFF)+1); sputc('\n');
    /* reset: CC.EN=0, wait CSTS.RDY==0 */
    nvw32(NVME_CC, nvr32(NVME_CC)&~1u);
    { uint64_t b=100000000ULL; while((nvr32(NVME_CSTS)&1u) && b--) cpu_pause(); }
    /* admin queues */
    uint64_t asq=falloc(), acq=falloc(); if(!asq||!acq){ sputs("HW-B no admin-queue frames\n"); return 0; }
    nvw32(NVME_AQA, ((NVQ_DEPTH-1)<<16)|(NVQ_DEPTH-1));
    nvw64(NVME_ASQ, asq); nvw64(NVME_ACQ, acq);
    /* enable: IOCQES=4 (16B), IOSQES=6 (64B), MPS=0 (4K), CSS=0 (NVM), EN=1 */
    nvw32(NVME_CC, (4u<<20)|(6u<<16)|1u);
    { uint64_t b=100000000ULL; while(!(nvr32(NVME_CSTS)&1u) && b--) cpu_pause(); }
    if(!(nvr32(NVME_CSTS)&1u)){ sputs("HW-B controller not READY (CSTS="); sx64(nvr32(NVME_CSTS)); sputs(") - stall.\n"); return 0; }
    admq.sq=P2V(asq); admq.cq=P2V(acq); admq.sqdb=(volatile uint32_t*)(NVBAR+0x1000);
    admq.cqdb=(volatile uint32_t*)(NVBAR+0x1000+stride); admq.sqt=0; admq.cqh=0; admq.phase=1;
    sputs("HW-B controller ENABLED (CSTS.RDY=1); admin queues live\n");
    /* create I/O CQ (qid 1) then I/O SQ (qid 1) */
    uint64_t iocq=falloc(), iosq=falloc(); if(!iocq||!iosq){ sputs("HW-B no I/O-queue frames\n"); return 0; }
    uint32_t c[16];
    cmd_zero(c); c[0]=0x05u|((uint32_t)(g_cid++)<<16); c[6]=(uint32_t)iocq; c[7]=(uint32_t)(iocq>>32);
    c[10]=((NVQ_DEPTH-1)<<16)|1u; c[11]=1u;                         /* create I/O CQ: qid 1, PC=1 */
    if(nvme_cmd(&admq,c)!=0){ sputs("HW-B create I/O CQ FAILED\n"); return 0; }
    cmd_zero(c); c[0]=0x01u|((uint32_t)(g_cid++)<<16); c[6]=(uint32_t)iosq; c[7]=(uint32_t)(iosq>>32);
    c[10]=((NVQ_DEPTH-1)<<16)|1u; c[11]=(1u<<16)|1u;                /* create I/O SQ: qid 1, CQID=1, PC=1 */
    if(nvme_cmd(&admq,c)!=0){ sputs("HW-B create I/O SQ FAILED\n"); return 0; }
    ioq.sq=P2V(iosq); ioq.cq=P2V(iocq); ioq.sqdb=(volatile uint32_t*)(NVBAR+0x1000+2*stride);
    ioq.cqdb=(volatile uint32_t*)(NVBAR+0x1000+3*stride); ioq.sqt=0; ioq.cqh=0; ioq.phase=1;
    sputs("HW-B I/O queue pair created (qid 1)\n");
    return 1;
}
/* one-LBA I/O: op 0x01 write / 0x02 read; nsid 1; PRP1 = 4K DMA frame phys. */
static int nvme_io(uint8_t op, uint64_t buf_phys, uint64_t slba){
    uint32_t c[16]; cmd_zero(c);
    c[0]=(uint32_t)op|((uint32_t)(g_cid++)<<16); c[1]=1u;           /* nsid=1 */
    c[6]=(uint32_t)buf_phys; c[7]=(uint32_t)(buf_phys>>32);         /* PRP1 */
    c[10]=(uint32_t)slba; c[11]=(uint32_t)(slba>>32); c[12]=0u;     /* SLBA, NLB=0 => 1 block */
    return nvme_cmd(&ioq,c);
}

/* ===========================================================================
 * HW-B2: content-addressed object write/read/re-verify on the NVMe device.
 * ===========================================================================*/
#define OBJ_MAGIC 0x4A424F4Eu   /* 'N','O','B','J' */
void kmain(void);
void kmain(void){
    serial_init();
    sputs("\n=== CARMIX hardware bring-up rung (b): real NVMe driver + content-addressed I/O ===\n");
    sputs("HW SCOPE: real NVMe register/queue interface, tested against QEMU -device nvme (the real interface). EMULATOR-tested, NOT physical-NVMe-tested (that needs real hardware). kernel.c + proven core untouched.\n");
    if(!LIMINE_BASE_REVISION_SUPPORTED){ sputs("FATAL: Limine base revision unsupported\n"); hcf(); }
    if(!hhdm_req.response){ sputs("FATAL: no HHDM\n"); hcf(); }
    hhdm_off=hhdm_req.response->offset;
    uint64_t bb=0,bl=0; if(mm_req.response){ struct limine_memmap_response*r=mm_req.response;
        for(uint64_t i=0;i<r->entry_count;i++){ struct limine_memmap_entry*e=r->entries[i]; if(e->type==LIMINE_MEMMAP_USABLE && e->length>bl){ bb=e->base; bl=e->length; } } }
    free_base=(bb+0xfff)&~0xfffULL; free_top=bb+bl;

    uint64_t t0=rdtsc(); int ok=nvme_init(); uint64_t init_cyc=rdtsc()-t0;
    if(!ok){ sputs("HW-B NVMe init FAILED (see above).\n"); hcf(); }
    sputs("HW-B1 NVMe init OK: "); sdec(init_cyc); sputs(" cyc (real-hardware-interface, QEMU-NVMe emulated this run)\n");

    /* DMA frames: write buffer, read buffer (4K each, page-aligned phys = PRP). */
    uint64_t wp=falloc(), rp=falloc(); uint8_t* wv=P2V(wp); uint8_t* rv=P2V(rp);

    /* build a content-addressed object (512B payload) + a header LBA naming it. */
    static uint8_t obj[LBA_BYTES]; for(uint32_t i=0;i<LBA_BYTES;i++) obj[i]=(uint8_t)(i*31u+7u);
    cvsasx_hash_t h; cvsasx_blake3(obj, LBA_BYTES, &h);
    sputs("HW-B2 object 512B, content address BLAKE3="); sputs(hx(h.b,16)); sputs("..\n");

    /* WRITE payload -> LBA 1, header -> LBA 0 (header binds len + hash: the torn-tail record). */
    for(uint32_t i=0;i<LBA_BYTES;i++) wv[i]=obj[i];
    uint64_t tw=rdtsc(); int w1=nvme_io(0x01, wp, 1); uint64_t wr_cyc=rdtsc()-tw;
    for(uint32_t i=0;i<4096;i++) wv[i]=0; *(uint32_t*)(wv+0)=OBJ_MAGIC; *(uint64_t*)(wv+4)=LBA_BYTES; for(int i=0;i<32;i++) wv[16+i]=h.b[i];
    int w0=nvme_io(0x01, wp, 0);
    sputs("HW-B2 wrote payload@LBA1 status="); sdec((uint64_t)w1); sputs(" header@LBA0 status="); sdec((uint64_t)w0); sputs(" (0=success); write "); sdec(wr_cyc); sputs(" cyc\n");

    /* READ header (LBA 0), then payload (LBA 1), re-verify by re-hash (disk untrusted). */
    for(uint32_t i=0;i<4096;i++) rv[i]=0;
    int r0=nvme_io(0x02, rp, 0);
    uint32_t magic=*(uint32_t*)(rv+0); uint64_t len=*(uint64_t*)(rv+4); uint8_t claim[32]; for(int i=0;i<32;i++) claim[i]=rv[16+i];
    for(uint32_t i=0;i<4096;i++) rv[i]=0;
    uint64_t trd=rdtsc(); int r1=nvme_io(0x02, rp, 1); uint64_t rd_cyc=rdtsc()-trd;
    cvsasx_hash_t chk; cvsasx_blake3(rv, len<=LBA_BYTES?len:LBA_BYTES, &chk);
    int hdr_ok=(magic==OBJ_MAGIC && len==LBA_BYTES);
    cvsasx_hash_t claimh; for(int i=0;i<32;i++) claimh.b[i]=claim[i];
    int reverify = hdr_ok && cvsasx_hash_eq(&chk,&claimh) && cvsasx_hash_eq(&chk,&h);
    sputs("HW-B2 read header status="); sdec((uint64_t)r0); sputs(" payload status="); sdec((uint64_t)r1); sputs("; read "); sdec(rd_cyc); sputs(" cyc\n");
    sputs("HW-B2 re-hash of the read-back payload == the stored content address="); sputs(reverify?"y":"n");
    sputs(" -> content-addressed object survived a real NVMe write/read round trip, re-verified from the device\n");

    /* tamper: a single flipped byte in the read-back payload must fail the re-hash. */
    rv[7]^=0xFF; cvsasx_hash_t bad; cvsasx_blake3(rv, LBA_BYTES, &bad);
    int tamper_caught = !cvsasx_hash_eq(&bad,&h);
    sputs("HW-B2 tamper: flip 1 payload byte -> re-hash MISMATCH detected="); sputs(tamper_caught?"y":"n"); sputs(" (device is untrusted media; every load re-verified)\n");

    sputs("\n===== HW-B RESULT =====\n");
    sputs("  NVMe init (real interface, QEMU-emulated): "); sputs(ok?"OK":"FAIL"); sputc('\n');
    sputs("  content-addressed write+read+re-verify on NVMe: "); sputs(reverify?"OK":"FAIL");
    sputs("   tamper caught: "); sputs(tamper_caught?"y":"n"); sputc('\n');
    sputs("  rdtsc (this run, QEMU-NVMe): init="); sdec(init_cyc); sputs(" write-1LBA="); sdec(wr_cyc); sputs(" read-1LBA="); sdec(rd_cyc); sputs(" cyc\n");
    sputs((ok&&reverify&&tamper_caught)?
        "  -> RUNG (b) DONE: a real NVMe controller driver does content-addressed I/O, re-verified from the device (QEMU-NVMe; physical-NVMe untested)\n":
        "  -> *** see failures above ***\n");
    sputs("##### NVMETEST DONE #####\n");
    hcf();
}
