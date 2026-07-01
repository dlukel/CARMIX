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
 * CARMIX - Track B: two-machine distributed rematerialization over ivshmem.
 * STANDALONE bare-metal test (NOT the desktop kernel). Two QEMU instances boot
 * THIS same ELF and share a host file via ivshmem-plain; the DIFF of a live
 * content-addressed state crosses the (shared-memory) wire.
 *
 *   Transport: ivshmem-plain (PCI vendor 0x1af4 device 0x1110). Chosen over
 *   virtio-net because virtio-net is a full NIC + ring + a TCP/IP stack - too
 *   heavy for one pass. ivshmem is a single shared-RAM BAR: enumerate PCI, read
 *   BAR2 (64-bit MMIO), map it through Limine's HHDM, poll/write tokens.
 *
 * BORROWED: the Limine boot shim + the serial/HHDM/PCI-config-port primitives
 * (replicated minimally from kernel.c - kernel.c stays byte-identical).
 * WRITTEN: PCI enumeration, the BAR2 map, the A<->B mailbox protocol, the
 * destination-pull Merkle sync, the warm-migration byte measurement.
 *
 * ROLE SELECTION: both instances boot the same image. The shared region's first
 * 8 bytes are a magic claim word. The instance that CASes the magic from 0 wins
 * role A (source); the other becomes B (destination). (Single-writer init: A
 * zeroes the region first, so a deterministic launch order isn't required - the
 * harness starts A, waits for it to claim, then starts B.)
 * ===========================================================================*/
#include <stdint.h>
#include <stddef.h>
#include "limine.h"
#include "cvsasx_store.h"
#include "cvsasx_sls.h"
#include "cvsasx_swcap.h"   /* Step 10: anti-amp re-mint gate (the authority backstop) */
#include "tweetnacl.h"      /* Step 10: vendored ref Ed25519 (real public-key source auth) */
#include "authz_keys.h"     /* Step 10: generated Ed25519 TEST keys (host genkeys) */

/* ---- Limine requests (framebuffer omitted; this test is serial-only) ------ */
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

/* ---- serial (16550 COM1) - minimal, from kernel.c ------------------------- */
static inline void outb(uint16_t p, uint8_t v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t inb(uint16_t p){ uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
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
static char hexbuf[65];
static const char* hx(const uint8_t*b,int n){ const char*d="0123456789abcdef"; int j=0; for(int i=0;i<n;i++){ hexbuf[j++]=d[b[i]>>4]; hexbuf[j++]=d[b[i]&15]; } hexbuf[j]=0; return hexbuf; }
static inline void hcf(void){ for(;;) __asm__ volatile("cli; hlt"); }
static inline void cpu_pause(void){ __asm__ volatile("pause"); }
static inline uint64_t rdtsc(void){ uint32_t a,d; __asm__ volatile("rdtsc":"=a"(a),"=d"(d)); return ((uint64_t)d<<32)|a; }

static uint64_t hhdm_off;
static inline void* P2V(uint64_t p){ return (void*)(p + hhdm_off); }

/* ---- PCI config-space access via ports 0xCF8 / 0xCFC ---------------------- */
#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC
static uint32_t pci_cfg_rd(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off){
    uint32_t a = 0x80000000u | ((uint32_t)bus<<16) | ((uint32_t)dev<<11) | ((uint32_t)fn<<8) | (off & 0xFC);
    outl(PCI_ADDR, a); return inl(PCI_DATA);
}
static void pci_cfg_wr(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t val){
    uint32_t a = 0x80000000u | ((uint32_t)bus<<16) | ((uint32_t)dev<<11) | ((uint32_t)fn<<8) | (off & 0xFC);
    outl(PCI_ADDR, a); outl(PCI_DATA, val);
}

/* ===========================================================================
 * Shared-region mailbox layout (offsets within ivshmem BAR2).
 *   The ivshmem-plain device's first 256 bytes are registers; with -plain (no
 *   interrupts) they are unused, but we start our payload at 0x1000 to be safe.
 * ===========================================================================*/
#define SH_MAGIC_OFF   0x1000   /* claim/handshake word: A writes the magic */
#define SH_A2B_OFF     0x1040   /* A->B mailbox: [flag(8)][len(8)][hash(32)][...] */
#define SH_B2A_OFF     0x1080   /* B->A mailbox */
#define SH_DATA_OFF    0x2000   /* bulk block-streaming region (B2/B3) */
#define SH_MAGIC       0x43524d49585f4231ULL   /* "CRMIXB1" */
#define MB_EMPTY 0u
#define MB_REQ   1u   /* request posted, awaiting consumer */
#define MB_ACK   2u   /* consumer processed */

/* a mailbox slot: flag, opcode, len, then 32-byte hash, then inline payload area */
typedef struct {
    volatile uint32_t flag;   /* MB_EMPTY/MB_REQ/MB_ACK */
    volatile uint32_t op;     /* opcode (B2: ADV/WANT/BLOCK) */
    volatile uint32_t len;    /* payload length */
    volatile uint32_t seq;    /* sequence / token */
    volatile uint8_t  hash[32];
} mbox_t;

#define OP_TOKEN 1   /* B1 round-trip token */
#define OP_ADV   2   /* B2: advertise root hash */
#define OP_WANT  3   /* B2: request a block by hash (hash field) */
#define OP_BLOCK 4   /* B2: a block follows in SH_DATA region (len, hash) */
#define OP_DONE  5   /* B2: sync complete */

/* ---- memory barrier so MMIO ordering between the two guests is observable -- */
static inline void mfence(void){ __asm__ volatile("mfence":::"memory"); }

/* small local mem helpers (store_mem.c provides the linked ones; these are tiny
 * locals for the mailbox copies so we don't depend on memcpy symbol ordering). */
static void bcopy_to(volatile uint8_t *d, const uint8_t *s, uint32_t n){ for(uint32_t i=0;i<n;i++) d[i]=s[i]; }
static void bcopy_from(uint8_t *d, const volatile uint8_t *s, uint32_t n){ for(uint32_t i=0;i<n;i++) d[i]=s[i]; }
static int  bcmp32(const volatile uint8_t *a, const uint8_t *b){ for(int i=0;i<32;i++) if(a[i]!=b[i]) return 1; return 0; }

/* spin until *flag == want, or budget exhausted. returns 1 on success, 0 on timeout. */
static int wait_flag(volatile uint32_t *flag, uint32_t want, uint64_t budget){
    while(budget--){ if(*flag==want){ mfence(); return 1; } cpu_pause(); }
    return 0;
}
#define WAIT_BUDGET 2000000000ULL   /* ~seconds of busy-spin; the harness caps wall time */

/* ===========================================================================
 * BAR2 discovery: walk PCI bus 0, find vendor 0x1af4 / device 0x1110, read its
 * 64-bit BAR2 base. Returns the physical base (0 on not-found).
 * ===========================================================================*/
static uint64_t find_ivshmem_bar2(uint8_t *out_bus, uint8_t *out_dev, uint8_t *out_fn){
    for(uint16_t bus=0; bus<256; bus++){
        for(uint8_t dev=0; dev<32; dev++){
            for(uint8_t fn=0; fn<8; fn++){
                uint32_t id = pci_cfg_rd((uint8_t)bus,dev,fn,0x00);
                uint16_t ven = id & 0xFFFF, devid = id >> 16;
                if(ven==0xFFFF) continue;
                if(ven==0x1af4 && devid==0x1110){
                    /* BAR2 at config offset 0x18; ivshmem BAR2 is 64-bit MMIO (BAR2+BAR3). */
                    uint32_t bar2 = pci_cfg_rd((uint8_t)bus,dev,fn,0x18);
                    uint32_t bar3 = pci_cfg_rd((uint8_t)bus,dev,fn,0x1C);
                    uint64_t base = ((uint64_t)bar3 << 32) | (uint64_t)(bar2 & ~0xFu);
                    /* ensure MEM space + bus-master enabled in the command register */
                    uint32_t cmd = pci_cfg_rd((uint8_t)bus,dev,fn,0x04);
                    pci_cfg_wr((uint8_t)bus,dev,fn,0x04, cmd | 0x6 /* MEM + BUSMASTER */);
                    if(out_bus)*out_bus=(uint8_t)bus; if(out_dev)*out_dev=dev; if(out_fn)*out_fn=fn;
                    return base;
                }
            }
        }
    }
    return 0;
}

/* Limine maps all phys RAM at HHDM, but a PCI MMIO BAR is NOT RAM - it may not be
 * in the HHDM. We map it ourselves into the page tables (uncacheable not needed
 * for ivshmem-plain backing file; it is plain RAM behind the BAR). We reuse the
 * page-table walker minimally. */
static uint64_t free_base, free_top, ram_lo, ram_hi;
static uint64_t falloc(void){ if(free_base+4096<=free_top){ uint64_t p=free_base; free_base+=4096; return p; } return 0; }
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
 * THE WORKLOAD (B2/B3): a content-addressed Merkle tree built in a local store.
 * A LARGE static region + a TINY mutable scratchpad. Source A holds the real
 * objects; destination B starts EMPTY and pulls only what it lacks.
 * ===========================================================================*/
#define N_LEAVES 1024u
static uint8_t  storeA_arena[1u<<20]; static cvsasx_store_entry_t storeA_idx[8192]; static cvsasx_store_t storeA;
static uint8_t  storeB_arena[1u<<20]; static cvsasx_store_entry_t storeB_idx[8192]; static cvsasx_store_t storeB;
static cvsasx_oid_t leaves[N_LEAVES], lvl[N_LEAVES], nxt[N_LEAVES];

static int g_tree_err;
static cvsasx_oid_t build_tree(cvsasx_sls_t *S, const cvsasx_oid_t *lv, uint32_t n){
    g_tree_err=0;
    for(uint32_t i=0;i<n;i++) lvl[i]=lv[i];
    uint32_t cnt=n;
    while(cnt>1){
        uint32_t m=0;
        for(uint32_t i=0;i<cnt;i+=16u){ uint32_t k=(cnt-i<16u)?(cnt-i):16u;
            if(cvsasx_sls_put_node(S,&lvl[i],k,&nxt[m++])!=CVSASX_SLS_OK) g_tree_err=1; }
        for(uint32_t i=0;i<m;i++) lvl[i]=nxt[i]; cnt=m;
    }
    return lvl[0];
}

/* fill the leaf set: leaf i = 8 distinct bytes (so no accidental dedup). A
 * mutation = (index, tag): leaf[index]'s byte 0 is overwritten with tag, so
 * DIFFERENT indices change DIFFERENT node paths and a 2-mutation set changes more
 * than a 1-mutation set. mut==0 (n_mut==0) rebuilds the baseline state. */
typedef struct { uint32_t idx; uint8_t tag; } mut_t;
static void make_leaves(cvsasx_sls_t *S, const mut_t *mut, uint32_t n_mut){
    for(uint32_t i=0;i<N_LEAVES;i++){
        uint8_t d[8]={(uint8_t)i,(uint8_t)(i>>8),(uint8_t)(i*3u),(uint8_t)(i*5u),
                      (uint8_t)(i*7u),(uint8_t)(i*11u),(uint8_t)(i*13u),(uint8_t)(i*17u)};
        for(uint32_t m=0;m<n_mut;m++) if(mut[m].idx==i) d[0]=mut[m].tag;
        cvsasx_sls_put_leaf(S,d,8,&leaves[i]);
    }
}

/* ===========================================================================
 * ROLE A (source) and ROLE B (destination) drivers.
 * Shared region pointers are computed from the mapped BAR2 virtual base.
 * ===========================================================================*/
static volatile uint8_t *SHM;   /* mapped BAR2 base (virtual) */
static inline mbox_t* A2B(void){ return (mbox_t*)(SHM+SH_A2B_OFF); }
static inline mbox_t* B2A(void){ return (mbox_t*)(SHM+SH_B2A_OFF); }

/* B1: token round-trip. A posts a token in A2B, B reads it, replies in B2A,
 * A reads the reply. Both print to serial. */
static int b1_source(void){
    mbox_t *a=A2B(), *b=B2A();
    uint32_t token = 0xCA11AB1E;
    sputs("B1 [A] posting token "); sx64(token); sputc('\n');
    a->op=OP_TOKEN; a->seq=token; a->len=0; mfence(); a->flag=MB_REQ; mfence();
    if(!wait_flag(&b->flag, MB_REQ, WAIT_BUDGET)){ sputs("B1 [A] TIMEOUT waiting for B reply\n"); return 0; }
    uint32_t reply=b->seq; b->flag=MB_ACK; mfence();
    sputs("B1 [A] got reply  "); sx64(reply); sputs(reply==(token^0xFFFFFFFFu)?"  (== ~token, ROUND-TRIP OK)\n":"  (BAD)\n");
    return reply==(token^0xFFFFFFFFu);
}
static int b1_dest(void){
    mbox_t *a=A2B(), *b=B2A();
    sputs("B1 [B] polling for A's token...\n");
    if(!wait_flag(&a->flag, MB_REQ, WAIT_BUDGET)){ sputs("B1 [B] TIMEOUT waiting for A token\n"); return 0; }
    uint32_t token=a->seq; a->flag=MB_ACK; mfence();
    sputs("B1 [B] received token "); sx64(token); sputs(" -> replying ~token\n");
    b->op=OP_TOKEN; b->seq=token^0xFFFFFFFFu; b->len=0; mfence(); b->flag=MB_REQ; mfence();
    return 1;
}

/* ===========================================================================
 * B2: destination-pull Merkle sync. A advertises root; B requests WANT(hash) for
 * every object it lacks (walking the tree top-down); A streams the raw bytes via
 * SH_DATA; B verifies BLAKE3(received)==requested-hash before inserting.
 *
 * Object model on the wire: each object is its sls-serialized bytes (leaf or
 * node). B re-puts them into its own store under the SAME hash (store recomputes
 * BLAKE3 - a forged block FAILS BY CONSTRUCTION: a tampered block hashes to a
 * different address and the requested-hash compare rejects it).
 *
 * B's walk: it knows the root oid (from ADV). It maintains a small work-stack of
 * oids it needs. For each, it WANTs the bytes; if the object is a NODE, it
 * parses the child oids and pushes the ones it still lacks. Identical subtrees
 * are pruned because B already has them (store_exists) -> never re-WANTed.
 * ===========================================================================*/

/* parse an sls node serialization -> child oids. Layout (from sls.c): we don't
 * know the exact byte layout, so B asks A's store directly via the wire for the
 * children list. To stay decoupled from sls.c internals, A sends, alongside each
 * BLOCK, a small header: [is_node(1)][nchild(1)] then nchild*32 child hashes for
 * nodes. B uses that to walk. The block PAYLOAD itself is still the raw sls bytes
 * and is hash-verified; the header is only a walk hint (B re-derives nothing
 * security-relevant from it - the hash check is on the payload). */

static cvsasx_sls_t SA, SB;

/* ---------------------------------------------------------------------------
 * SEQUENCE-NUMBER channel (replaces the tri-state flag - that had a toggle
 * ambiguity: BLOCK#2's MB_REQ was indistinguishable from BLOCK#1's). Each side
 * owns one MONOTONIC counter in the shared region; a message is "ready" when the
 * counter EXCEEDS the value the reader last consumed. No reset, no toggle race.
 *
 *   B->A channel: B writes a request (op + 32-byte hash) then bumps b_seq.
 *   A->B channel: A writes a response (op + len + child-hint + payload) then
 *                 bumps a_seq.
 * The reader spins until peer_seq != last_seen, then reads the (already-fenced)
 * fields. Single-producer-per-counter => the bytes are stable once the counter
 * advances (writer fences BEFORE the bump; reader fences AFTER observing it).
 * ------------------------------------------------------------------------- */
#define SEQ_A_OFF   0x1100   /* A's response sequence counter (A writes, B reads) */
#define SEQ_B_OFF   0x1108   /* B's request  sequence counter (B writes, A reads) */
#define REQ_OFF     0x1140   /* B's request record: op + hash */
#define RSP_OFF     0x1180   /* A's response record: op + len + nchild */
/* child hints + payload live in the bulk DATA region */
#define HINT_OFF    0x2000   /* child-hash hints: nchild*32 bytes              */
#define PAY_OFF     0x3000   /* raw sls payload bytes (hash-verified)          */

static inline volatile uint64_t* seqA(void){ return (volatile uint64_t*)(SHM+SEQ_A_OFF); }
static inline volatile uint64_t* seqB(void){ return (volatile uint64_t*)(SHM+SEQ_B_OFF); }
typedef struct { volatile uint32_t op; volatile uint32_t len; volatile uint32_t nchild; volatile uint8_t hash[32]; } rec_t;
static inline rec_t* REQ(void){ return (rec_t*)(SHM+REQ_OFF); }
static inline rec_t* RSP(void){ return (rec_t*)(SHM+RSP_OFF); }
static inline volatile uint8_t* HINT(void){ return SHM+HINT_OFF; }
static inline volatile uint8_t* PAY(void){ return SHM+PAY_OFF; }

static int wait_seq(volatile uint64_t *ctr, uint64_t last, uint64_t budget){
    while(budget--){ if(*ctr != last){ mfence(); return 1; } cpu_pause(); }
    return 0;
}
/* ABSOLUTE-sequence wait (timing-independent): spin until *ctr >= want. Used by
 * the Step-12 K-suite so a reader that starts late (after the writer already
 * bumped the counter) still rendezvouses correctly - no "!= last" toggle race. */
static int wait_seq_ge(volatile uint64_t *ctr, uint64_t want, uint64_t budget){
    while(budget--){ if(*ctr >= want){ mfence(); return 1; } cpu_pause(); }
    return 0;
}

/* A side: serve WANT requests. Counts payload bytes streamed (= bytes on wire). */
static uint64_t a_bytes_sent;
#if defined(TAMPER_TEST) || defined(NODE_TAMPER_TEST)
static int g_tampered;
#endif
#ifdef NODE_TAMPER_TEST
static uint32_t g_nodes_served;
#endif
static int a_serve(cvsasx_sls_t *S, const cvsasx_oid_t *root){
    rec_t *rq=REQ(), *rs=RSP();
    a_bytes_sent = 0;
    uint64_t b_last = *seqB();   /* baseline: ignore stale requests from a prior round */
    /* advertise the root via the FIRST response (B reads it before any WANT). */
    rs->op=OP_ADV; rs->len=0; rs->nchild=0; bcopy_to(rs->hash, root->b, 32); mfence(); (*seqA())++; mfence();
    for(;;){
        if(!wait_seq(seqB(), b_last, WAIT_BUDGET)){ sputs("B2 [A] TIMEOUT waiting for WANT/DONE\n"); return 0; }
        b_last = *seqB();
        uint32_t op=rq->op;
        if(op==OP_DONE){ return 1; }
        if(op!=OP_WANT) continue;
        cvsasx_oid_t want; bcopy_from(want.b, rq->hash, 32);
        const void *bytes; size_t len;
        if(cvsasx_sls_get(S,&want,&bytes,&len)!=CVSASX_SLS_OK){
            sputs("B2 [A] WANT miss for "); sputs(hx(want.b,8)); sputc('\n');
            rs->op=OP_BLOCK; rs->len=0; rs->nchild=0; mfence(); (*seqA())++; mfence();
            continue;
        }
        /* sls node serialization = [tag(1)][n as 4 LE bytes][n*32 child hashes].
         * leaf = [tag(1)][data]. The child-hint lets B walk; the payload itself is
         * still BLAKE3-verified, so the hint is untrusted (security is on the hash). */
        const uint8_t *pb=(const uint8_t*)bytes;
        uint8_t is_node = (len>=5 && pb[0]==CVSASX_SLS_NODE) ? 1 : 0;
        uint32_t nchild = is_node ? (uint32_t)pb[1] | ((uint32_t)pb[2]<<8) | ((uint32_t)pb[3]<<16) | ((uint32_t)pb[4]<<24) : 0;
        if(nchild>CVSASX_SLS_MAX_CHILDREN) nchild=CVSASX_SLS_MAX_CHILDREN;
        volatile uint8_t *h=HINT();
        for(uint32_t c=0;c<nchild;c++) bcopy_to(h+(uint64_t)c*32, pb+5+(uint64_t)c*32, 32);  /* children at byte 5 */
        bcopy_to(PAY(), pb, (uint32_t)len);
#ifdef TAMPER_TEST
        /* SECURITY DEMO (B-tamper, LEAF site): corrupt exactly ONE leaf block on the
         * wire. B must reject it by BLAKE3 mismatch (content != requested address) -
         * forgery fails BY CONSTRUCTION. One-shot so the rest of the sync completes. */
        if(!is_node && !g_tampered){ PAY()[0] ^= 0xFFu; g_tampered=1;
            sputs("B2 [A] *** TAMPER(leaf): flipped a byte in a streamed LEAF block (expect B to REJECT)\n"); }
#endif
#ifdef NODE_TAMPER_TEST
        /* SECURITY DEMO (B5, NODE site): corrupt exactly ONE internal Merkle NODE block
         * (a different on-wire object kind than the leaf case). B must reject it by the
         * SAME BLAKE3 check - integrity is on the hash, not the object kind. We flip a
         * byte INSIDE the node body (a child-hash byte) so the serialization is a
         * structurally-plausible-but-wrong node. Skip the FIRST node served (the root)
         * so B accepts the root, descends, and we corrupt a MID-TREE node - the rest of
         * the tree still flows, isolating the rejected node's fail-closed effect. */
        if(is_node){ if(g_nodes_served++ == 1 && !g_tampered){ PAY()[6] ^= 0xFFu; g_tampered=1;
            sputs("B2 [A] *** TAMPER(node): flipped a byte in a streamed mid-tree NODE block (expect B to REJECT)\n"); } }
#endif
        rs->op=OP_BLOCK; rs->len=(uint32_t)len; rs->nchild=nchild; mfence(); (*seqA())++; mfence();
        a_bytes_sent += len;
    }
}

/* B side work-stack of oids to fetch. */
#define WS_MAX 4096
static cvsasx_oid_t ws[WS_MAX]; static uint32_t ws_top;
static uint8_t recvbuf[CVSASX_SLS_MAX_LEAF + 16];

static uint64_t b_received; static uint32_t b_blocks; static uint32_t b_rejected; static uint32_t b_pruned;
static cvsasx_oid_t b2_root;   /* the root B installed in the cold sync (Step-10 authz binding) */
static int b_sync(cvsasx_sls_t *S){
    rec_t *rq=REQ(), *rs=RSP();
    uint64_t a_last = *seqA();
    /* receive ADV (A's first response) */
    if(!wait_seq(seqA(), a_last, WAIT_BUDGET)){ sputs("B2 [B] TIMEOUT waiting for ADV\n"); return 0; }
    a_last=*seqA();
    if(rs->op!=OP_ADV){ sputs("B2 [B] expected ADV, got op="); sdec(rs->op); sputc('\n'); return 0; }
    cvsasx_oid_t root; bcopy_from(root.b, rs->hash, 32);
    b2_root = root;   /* capture the installed root for the Step-10 authz binding */
    sputs("B2 [B] root advertised "); sputs(hx(root.b,8)); sputc('\n');

    b_received=0; b_blocks=0; b_rejected=0; b_pruned=0;
    ws_top=0; ws[ws_top++]=root;
    while(ws_top){
        cvsasx_oid_t want = ws[--ws_top];
        cvsasx_hash_t wh; bcopy_from(wh.b, want.b, 32);
        if(cvsasx_store_exists(S->store,&wh)){ b_pruned++; continue; }   /* PRUNE: already resident */
        /* post WANT */
        rq->op=OP_WANT; bcopy_to(rq->hash, want.b, 32); mfence(); (*seqB())++; mfence();
        /* receive BLOCK */
        if(!wait_seq(seqA(), a_last, WAIT_BUDGET)){ sputs("B2 [B] TIMEOUT waiting for BLOCK\n"); return 0; }
        a_last=*seqA();
        if(rs->op!=OP_BLOCK){ sputs("B2 [B] expected BLOCK, got op="); sdec(rs->op); sputc('\n'); return 0; }
        uint32_t len=rs->len, nchild=rs->nchild;
        if(len==0 || len>CVSASX_SLS_MAX_LEAF+16){ sputs("B2 [B] bad block len="); sdec(len); sputc('\n'); return 0; }
        bcopy_from(recvbuf, PAY(), len);
        cvsasx_oid_t kids[CVSASX_SLS_MAX_CHILDREN]; uint32_t nk=nchild>CVSASX_SLS_MAX_CHILDREN?CVSASX_SLS_MAX_CHILDREN:nchild;
        for(uint32_t c=0;c<nk;c++) bcopy_from(kids[c].b, HINT()+(uint64_t)c*32, 32);

        /* BY CONSTRUCTION: recompute BLAKE3; demand it equals the requested address. */
        cvsasx_hash_t got; cvsasx_blake3(recvbuf, len, &got);
        if(bcmp32(want.b, got.b)){
            b_rejected++;
            sputs("B2 [B] *** HASH MISMATCH - block REJECTED (tamper-proof) want="); sputs(hx(want.b,8));
            sputs(" got="); sputs(hx(got.b,8)); sputc('\n');
            continue;   /* fail-closed: never accept content != its address */
        }
        cvsasx_hash_t ins; cvsasx_store_put(S->store, recvbuf, len, &ins);
        b_received += len; b_blocks++;
        for(uint32_t c=0;c<nk;c++){
            cvsasx_hash_t kh; bcopy_from(kh.b, kids[c].b, 32);
            if(!cvsasx_store_exists(S->store,&kh) && ws_top<WS_MAX) ws[ws_top++]=kids[c];
        }
    }
    /* tell A we're done */
    rq->op=OP_DONE; mfence(); (*seqB())++; mfence();
    return 1;
}

/* ===========================================================================
 * STEP 10 - TRUST-BOUNDARY AUTHORIZATION (D0/D1/D2).
 * Hash-verify (B2/B5) proves INTEGRITY, not AUTHORIZATION. A SIGNED AuthRec gates
 * re-mint: it binds the migration to the exact Merkle root, the scope (computation
 * + destination), an authority ceiling, and a single-use time-bound nonce. The
 * anti-amp swcap gate is the backstop (B re-mints capped at the record's ceiling).
 *
 * CRYPTO: real public-key Ed25519 (vendored tweetnacl ref - SHA-512 included, no
 * libc, links freestanding; host-side genkeys produced the TEST keys). This is
 * genuine source AUTHENTICATION, not a PSK MAC. not implemented: real key distribution/PKI.
 * ===========================================================================*/

/* D0: the authorization record. Packed, fixed layout = the signed message. */
#define AUTHREC_LEN 64u  /* 32 root + 4 comp + 4 dest + 8 ceiling + 8 nonce + 8 expiry (+ pad) */
#define SIGNED_LEN  (64u + AUTHREC_LEN)   /* tweetnacl sm = sig(64) || authrec(64) = 128 */
typedef struct {
    uint8_t  epoch_root_hash[32];   /* binds to EXACTLY the Merkle root B installs   */
    uint32_t computation_id;        /* scope: which computation                       */
    uint32_t dest_id;               /* scope: which destination machine               */
    uint64_t authority_ceiling;     /* caps re-mint (max object length B may grant)    */
    uint64_t nonce;                 /* single-use (replay defense)                     */
    uint64_t expiry;                /* time-bound (tick deadline)                      */
} authrec_t;
_Static_assert(sizeof(authrec_t) <= AUTHREC_LEN, "authrec fits");

/* expected scope on B (the legitimate migration's identity). */
#define EXPECT_COMP 0xC0FFEE01u
#define MY_DEST_ID  0x0000000Bu     /* this destination machine's id ("B") */
#define SRC_CEILING 256u            /* the source's true authority (object length) */

/* serialize an authrec into a flat AUTHREC_LEN buffer (deterministic LE). */
static void authrec_pack(const authrec_t *r, uint8_t out[AUTHREC_LEN]){
    for(uint32_t i=0;i<AUTHREC_LEN;i++) out[i]=0;
    for(int i=0;i<32;i++) out[i]=r->epoch_root_hash[i];
    out[32]=r->computation_id; out[33]=r->computation_id>>8; out[34]=r->computation_id>>16; out[35]=r->computation_id>>24;
    out[36]=r->dest_id; out[37]=r->dest_id>>8; out[38]=r->dest_id>>16; out[39]=r->dest_id>>24;
    for(int i=0;i<8;i++) out[40+i]=(uint8_t)(r->authority_ceiling>>(8*i));
    for(int i=0;i<8;i++) out[48+i]=(uint8_t)(r->nonce>>(8*i));
    for(int i=0;i<8;i++) out[56+i]=(uint8_t)(r->expiry>>(8*i));
}

/* shared-region offsets for the signed record (kept clear of the B2 channel). */
#define AUTHZ_OFF   0x4000   /* signed message: sig(64) || authrec(64) = 128 bytes, then served root(32) */
#define AUTHZ_ROOT  0x4080   /* the served root (32) follows the 128-byte signed msg */
#define AUTHZ_CASE  0x40C0   /* current case id (A writes, B reads) */
#define AUTHZ_SEQA  0x40C8   /* A's authz seq */
#define AUTHZ_SEQB  0x40D0   /* B's authz verdict seq */
#define AUTHZ_VERD  0x40D8   /* B's verdict code */
static inline volatile uint8_t* AUTHZ(void){ return SHM+AUTHZ_OFF; }
static inline volatile uint32_t* authz_case(void){ return (volatile uint32_t*)(SHM+AUTHZ_CASE); }
static inline volatile uint64_t* authz_seqA(void){ return (volatile uint64_t*)(SHM+AUTHZ_SEQA); }
static inline volatile uint64_t* authz_seqB(void){ return (volatile uint64_t*)(SHM+AUTHZ_SEQB); }
static inline volatile uint32_t* authz_verd(void){ return (volatile uint32_t*)(SHM+AUTHZ_VERD); }

/* Step 12 K-suite wire window (fresh region, clear of the AUTHZ window).
 *   KIND   : 0=lifecycle op, 1=migration attempt
 *   MSG    : signed message (lifecycle 112B or migration 128B)
 *   SIGNER : signer_pk(32) - migration only
 *   ROOT   : served root(32) - migration only  */
#define K_KIND_OFF   0x5000   /* uint32 kind (A writes) */
#define K_MSG_OFF    0x5010   /* signed message bytes */
#define K_SIGNER_OFF 0x5090   /* signer_pk(32), migration */
#define K_ROOT_OFF   0x50B0   /* served root(32), migration */
#define K_SEQA_OFF   0x50E0   /* A's K seq */
#define K_SEQB_OFF   0x50E8   /* B's K verdict seq */
#define K_VERD_OFF   0x50F0   /* B's verdict */
static inline volatile uint32_t* k_kind(void){ return (volatile uint32_t*)(SHM+K_KIND_OFF); }
static inline volatile uint8_t*  k_msg(void){ return SHM+K_MSG_OFF; }
static inline volatile uint8_t*  k_signer(void){ return SHM+K_SIGNER_OFF; }
static inline volatile uint8_t*  k_root(void){ return SHM+K_ROOT_OFF; }
static inline volatile uint64_t* k_seqA(void){ return (volatile uint64_t*)(SHM+K_SEQA_OFF); }
static inline volatile uint64_t* k_seqB(void){ return (volatile uint64_t*)(SHM+K_SEQB_OFF); }
static inline volatile uint32_t* k_verd(void){ return (volatile uint32_t*)(SHM+K_VERD_OFF); }
#define K_LIFE 0u
#define K_MIGR 1u

/* ===========================================================================
 * STEP 13 - AUTHORITY-SET MIGRATION (DM1-DM8). Step 10 signs a single authority
 * ceiling. This generalizes to a process's whole AUTHORITY SET: N capabilities,
 * each an object hash + rights + length, canonically serialized (sorted by hash,
 * so the bytes are layout-independent), signed by A with the EXISTING Ed25519, and
 * verified by B against A's pre-shared key. On success B re-mints EACH cap bounded
 * by the SAME unmodified swcap anti-amp gate (the distributed analogue of the US3
 * re-mint), minting NOTHING beyond the verified set (C_B' = C_A). Any post-sign
 * change to the set (a widened or added cap = an amplification attempt), a wrong
 * signing key, or a corrupt signature breaks the Ed25519 check, so B rejects
 * fail-closed and re-mints nothing. TRUSTED-CLUSTER model: B trusts A's key and
 * assumes A is honest about the set it signs. Reuses crypto_sign/crypto_sign_open,
 * the swcap gate, and the ivshmem transfer. See kernel/MIGRATION_AUTHSET_LOG.md. */
#define DM_NCAP       3u
#define DM_CAP_BYTES  44u                          /* 32 hash + 4 rights + 8 length (LE) */
#define DM_SET_BYTES  (DM_NCAP*DM_CAP_BYTES)       /* 132 */
#define DM_SIGNED_LEN (64u + DM_SET_BYTES)         /* tweetnacl sm = sig(64) || set(132) = 196 */
typedef struct { uint8_t hash[32]; uint32_t rights; uint64_t length; } dm_cap_t;
#define DM_MSG_OFF   0x6000                         /* signed set message */
#define DM_ROOT_OFF  0x6100                         /* served root (32) */
#define DM_CASE_OFF  0x6120
#define DM_SEQA_OFF  0x6128
#define DM_SEQB_OFF  0x6130
#define DM_VERD_OFF  0x6138
#define DM_NREM_OFF  0x6140                         /* B reports how many caps it re-minted (|C_B'|) */
static inline volatile uint32_t* dm_case(void){ return (volatile uint32_t*)(SHM+DM_CASE_OFF); }
static inline volatile uint64_t* dm_seqA(void){ return (volatile uint64_t*)(SHM+DM_SEQA_OFF); }
static inline volatile uint64_t* dm_seqB(void){ return (volatile uint64_t*)(SHM+DM_SEQB_OFF); }
static inline volatile uint32_t* dm_verd(void){ return (volatile uint32_t*)(SHM+DM_VERD_OFF); }
static inline volatile uint32_t* dm_nrem(void){ return (volatile uint32_t*)(SHM+DM_NREM_OFF); }
/* order-compare two 32-byte hashes (canonical sort key). */
static int dm_hcmp(const uint8_t a[32], const uint8_t b[32]){ for(int i=0;i<32;i++){ if(a[i]<b[i]) return -1; if(a[i]>b[i]) return 1; } return 0; }
/* canonical serialization (DM1): sort the caps by hash, pack each hash||rights(LE)||length(LE).
 * Same set -> same bytes, independent of the caller's ordering. */
static void dm_set_pack(const dm_cap_t* in, uint32_t n, uint8_t out[DM_SET_BYTES]){
    dm_cap_t s[DM_NCAP]; for(uint32_t i=0;i<n;i++) s[i]=in[i];
    for(uint32_t i=0;i<n;i++) for(uint32_t j=0;j+1<n-i;j++) if(dm_hcmp(s[j].hash,s[j+1].hash)>0){ dm_cap_t t=s[j]; s[j]=s[j+1]; s[j+1]=t; }
    for(uint32_t i=0;i<DM_SET_BYTES;i++) out[i]=0;
    for(uint32_t k=0;k<n;k++){ uint32_t b=k*DM_CAP_BYTES;
        for(int i=0;i<32;i++) out[b+i]=s[k].hash[i];
        for(int i=0;i<4;i++) out[b+32+i]=(uint8_t)(s[k].rights>>(8*i));
        for(int i=0;i<8;i++) out[b+36+i]=(uint8_t)(s[k].length>>(8*i)); }
}

/* distinct reject reasons (each adversary fails by its OWN check - E2 rigor).
 * Step 10 verdicts V_ACCEPT..V_ANTIAMP; Step 12 adds the 4 key-lifecycle verdicts. */
enum { V_ACCEPT=0, V_BAD_SIG=1, V_ROOT_MISMATCH=2, V_WRONG_SCOPE=3, V_REPLAY_OR_EXPIRED=4, V_ANTIAMP=5,
       V_UNKNOWN_KEY=6, V_REVOKED_KEY=7, V_BAD_AUTHORITY=8, V_STALE_EPOCH=9 };
static const char* verd_name(uint32_t v){
    switch(v){ case V_ACCEPT:return "ACCEPT"; case V_BAD_SIG:return "REJECT bad-signature [T1]";
        case V_ROOT_MISMATCH:return "REJECT root-mismatch [T1/T4]"; case V_WRONG_SCOPE:return "REJECT wrong-scope [T3]";
        case V_REPLAY_OR_EXPIRED:return "REJECT replay-or-expired [T2]"; case V_ANTIAMP:return "REJECT anti-amp-ceiling [T3]";
        case V_UNKNOWN_KEY:return "REJECT unknown-key [K1a]"; case V_REVOKED_KEY:return "REJECT revoked-key [K1b]";
        case V_BAD_AUTHORITY:return "REJECT bad-authority [K-lifecycle]"; case V_STALE_EPOCH:return "REJECT stale-epoch [K-lifecycle]";
        default:return "??"; } }

/* B's nonce-seen set (single-use). Tiny ring; a real impl persists this. */
static uint64_t seen_nonces[64]; static uint32_t seen_n;
static int nonce_seen(uint64_t n){ for(uint32_t i=0;i<seen_n;i++) if(seen_nonces[i]==n) return 1; return 0; }
static void nonce_mark(uint64_t n){ if(seen_n<64) seen_nonces[seen_n++]=n; }

/* B's clock: reuse the migration's monotonic tick proxy. We don't have a timer in
 * nettest, so "now" is a counter B advances each case - expiry < now => expired. */
static uint64_t b_now;

/* the anti-amp re-mint BACKSTOP (shared by the Step-10 gate AND the Step-12
 * lifecycle gate): mint a fresh handle for `want_len` capped at SRC_CEILING via
 * the UNMODIFIED swcap gate. Returns V_ACCEPT or V_ANTIAMP. */
static uint8_t remint_arena[256];
static uint32_t antiamp_remint(uint64_t want_len, const cvsasx_oid_t *served_root){
    cvsasx_swcap_t croot={ (uint64_t)(uintptr_t)remint_arena, SRC_CEILING,
                           CVSASX_PERM_LOAD|CVSASX_PERM_STORE|CVSASX_PERM_GLOBAL, 1 };
    cvsasx_sw_custodian_t cust; cvsasx_sw_custodian_init(&cust, croot);
    cvsasx_sw_region_t reg; reg.object_cap=croot; reg.object_base_addr=(uint64_t)(uintptr_t)remint_arena; reg.object_length=want_len;
    for(int i=0;i<(int)CVSASX_BLAKE3_LEN;i++) reg.hash[i]=served_root->b[i];
    cvsasx_referent_t ref; for(int i=0;i<(int)CVSASX_BLAKE3_LEN;i++) ref.hash[i]=served_root->b[i];
    ref.object_base_addr=(uint64_t)(uintptr_t)remint_arena; ref.object_length=want_len;
    cvsasx_pir_t pir; cvsasx_sw_cap_strip(croot,&ref,&pir);
    pir.length = want_len;              /* the ceiling B is asked to grant (UNCLAMPED) */
    cvsasx_swcap_t out; cvsasx_status_t st = cvsasx_sw_cap_remint(&cust,&pir,&reg,&out);
    if(st!=CVSASX_OK || !out.valid) return V_ANTIAMP;   /* gate refused (e.g. ceiling > source) */
    return V_ACCEPT;
}

/* checks (2)-(5) on an ALREADY-signature-verified recovered authrec. Shared by
 * the Step-10 gate (fixed SRC_PK) and the Step-12 gate (trust-store key). */
static uint32_t authz_checks_2to5(const uint8_t *recovered, unsigned long long rlen,
                                  const cvsasx_oid_t *served_root){
    if(rlen != AUTHREC_LEN) return V_BAD_SIG;
    authrec_t r; for(int i=0;i<32;i++) r.epoch_root_hash[i]=recovered[i];
    r.computation_id = (uint32_t)recovered[32]|((uint32_t)recovered[33]<<8)|((uint32_t)recovered[34]<<16)|((uint32_t)recovered[35]<<24);
    r.dest_id        = (uint32_t)recovered[36]|((uint32_t)recovered[37]<<8)|((uint32_t)recovered[38]<<16)|((uint32_t)recovered[39]<<24);
    r.authority_ceiling=0; for(int i=0;i<8;i++) r.authority_ceiling|=(uint64_t)recovered[40+i]<<(8*i);
    r.nonce=0;  for(int i=0;i<8;i++) r.nonce |=(uint64_t)recovered[48+i]<<(8*i);
    r.expiry=0; for(int i=0;i<8;i++) r.expiry|=(uint64_t)recovered[56+i]<<(8*i);
    /* (2) record binds to the EXACT root B pulled? [T1/T4] */
    for(int i=0;i<32;i++) if(r.epoch_root_hash[i]!=served_root->b[i]) return V_ROOT_MISMATCH;
    /* (3) scope: this destination AND the expected computation? [T3] */
    if(r.dest_id!=MY_DEST_ID || r.computation_id!=EXPECT_COMP) return V_WRONG_SCOPE;
    /* (4) nonce unseen + not expired? [T2] */
    if(nonce_seen(r.nonce) || r.expiry < b_now) return V_REPLAY_OR_EXPIRED;
    nonce_mark(r.nonce);
    /* (5) re-mint capped at the SOURCE's true authority via the anti-amp gate
     * [T3 backstop]. The gate, not a min(), bounds the grant; over-ceiling REJECTS
     * with BAD_BOUNDS (the SAME E2/C1 wider-bounds check). */
    return antiamp_remint(r.authority_ceiling, served_root);
}

/* THE STEP-10 GATE (D1): verify a signed record IN ORDER under the FIXED source
 * key, fail-closed with a DISTINCT status, then re-mint capped at the ceiling. */
static uint32_t authz_verify_and_remint(const uint8_t signed_msg[SIGNED_LEN], const cvsasx_oid_t *served_root){
    /* (1) signature valid under the EXPECTED source key? [T1] */
    uint8_t recovered[SIGNED_LEN]; unsigned long long rlen=0;
    if(crypto_sign_open(recovered, &rlen, signed_msg, SIGNED_LEN, SRC_PK) != 0) return V_BAD_SIG;
    return authz_checks_2to5(recovered, rlen, served_root);
}

/* ===========================================================================
 * STEP 12 - KEY-DISTRIBUTION / ROTATION / REVOCATION LIFECYCLE (K0/K1/K2/K3).
 * Step 10 hardcoded SRC_PK as the one true signer. Real systems rotate and revoke
 * source keys. We add a tiny in-memory TRUST STORE on B, fed by AUTHORITY-signed
 * lifecycle records, and gate the migration on the CURRENT (non-revoked) key.
 *
 * Trust chain: AUTH_PK (the irreducible bootstrap root, established out of band //
 * OPEN) signs enroll/rotate/revoke records; those records set/replace the current
 * source key and the revocation set; the migration gate then verifies the wire
 * signer against that live trust state BEFORE the Step-10 checks. The anti-amp
 * swcap gate remains the backstop (a key-valid migration still re-mints capped).
 * ===========================================================================*/

/* K0: the lifecycle record. op + subject_pk(32) + epoch(8). Signed by AUTH_SK. */
enum { LC_ENROLL=0, LC_ROTATE=1, LC_REVOKE=2 };
#define LIFEREC_LEN 48u   /* 1 op + 32 pk + 8 epoch (+ 7 pad) - fixed signed message */
#define LC_SIGNED_LEN (64u + LIFEREC_LEN)   /* sm = sig(64) || liferec(48) = 112 */
typedef struct { uint8_t op; uint8_t subject_pk[32]; uint64_t epoch; } liferec_t;
_Static_assert(sizeof(liferec_t) <= LIFEREC_LEN, "liferec fits");
static void liferec_pack(const liferec_t *r, uint8_t out[LIFEREC_LEN]){
    for(uint32_t i=0;i<LIFEREC_LEN;i++) out[i]=0;
    out[0]=r->op;
    for(int i=0;i<32;i++) out[1+i]=r->subject_pk[i];
    for(int i=0;i<8;i++) out[33+i]=(uint8_t)(r->epoch>>(8*i));
}

/* K0: the trust store (B-side, in-memory). authority_pk is BAKED = AUTH_PK - the
 * one root we cannot derive from anything else (not implemented: out-of-band bootstrap /
 * CA / identity proofing of this first key; not implemented: persistence across reboot -
 * this store is RAM-only, a reboot re-bootstraps from the baked AUTH_PK). */
#define TS_REVOKED_MAX 16
typedef struct {
    uint8_t  authority_pk[32];
    uint8_t  current_src_pk[32];
    uint8_t  has_current;                 /* 0 until the first enroll */
    uint64_t lifecycle_epoch;             /* monotonic; every accepted op raises it */
    uint8_t  revoked[TS_REVOKED_MAX][32];
    int      n_revoked;
} trust_store_t;
static trust_store_t TRUST;

static int pk_eq(const uint8_t a[32], const uint8_t b[32]){ for(int i=0;i<32;i++) if(a[i]!=b[i]) return 1; return 0; }
static int ts_is_revoked(const uint8_t pk[32]){
    for(int i=0;i<TRUST.n_revoked;i++) if(pk_eq(TRUST.revoked[i],pk)==0) return 1; return 0; }

static void ts_init(void){
    for(int i=0;i<32;i++) TRUST.authority_pk[i]=AUTH_PK[i];
    for(int i=0;i<32;i++) TRUST.current_src_pk[i]=0;
    TRUST.has_current=0; TRUST.lifecycle_epoch=0; TRUST.n_revoked=0;
}

/* K1 (lifecycle apply gate): verify a signed lifecycle record under authority_pk,
 * require strictly-monotonic epoch, then apply. Returns the DISTINCT verdict.
 * Fail-closed: a forged record (bad authority) or a stale/replayed epoch changes
 * NOTHING. enroll/rotate set current_src_pk; revoke appends permanently. */
static uint32_t lifecycle_apply(const uint8_t signed_msg[LC_SIGNED_LEN]){
    uint8_t recovered[LC_SIGNED_LEN]; unsigned long long rlen=0;
    /* signature MUST verify under the authority key, else bad-authority [forgery] */
    if(crypto_sign_open(recovered, &rlen, signed_msg, LC_SIGNED_LEN, TRUST.authority_pk) != 0) return V_BAD_AUTHORITY;
    if(rlen != LIFEREC_LEN) return V_BAD_AUTHORITY;
    liferec_t r; r.op=recovered[0];
    for(int i=0;i<32;i++) r.subject_pk[i]=recovered[1+i];
    r.epoch=0; for(int i=0;i<8;i++) r.epoch|=(uint64_t)recovered[33+i]<<(8*i);
    /* monotonic: strictly greater than the stored epoch, else stale/replay */
    if(r.epoch <= TRUST.lifecycle_epoch) return V_STALE_EPOCH;
    /* APPLY (all accepted ops raise the epoch) */
    switch(r.op){
        case LC_ENROLL:
        case LC_ROTATE:
            for(int i=0;i<32;i++) TRUST.current_src_pk[i]=r.subject_pk[i];
            TRUST.has_current=1; break;
        case LC_REVOKE:
            if(TRUST.n_revoked<TS_REVOKED_MAX){ for(int i=0;i<32;i++) TRUST.revoked[TRUST.n_revoked][i]=r.subject_pk[i]; TRUST.n_revoked++; }
            break;
        default: return V_BAD_AUTHORITY;   /* unknown op = malformed = fail-closed */
    }
    TRUST.lifecycle_epoch=r.epoch;
    return V_ACCEPT;
}

/* K1 (migration gate, lifecycle-aware): the wire now carries signer_pk(32). Before
 * the Step-10 checks, fail-closed with DISTINCT new verdicts:
 *   (a) signer_pk == trust.current_src_pk  else V_UNKNOWN_KEY
 *   (b) signer_pk NOT in revoked[]         else V_REVOKED_KEY
 * then verify the signature under signer_pk (== current key) and run checks (2)-(5).
 * Note: signature verification under signer_pk (a verified-current key) is genuine
 * source auth - the current key is itself authenticated by the AUTH-signed lifecycle. */
static uint32_t migrate_verify_lifecycle(const uint8_t signed_msg[SIGNED_LEN], const uint8_t signer_pk[32],
                                         const cvsasx_oid_t *served_root){
    /* (a) is this the current source key? (covers downgrade-to-old-key too) */
    if(!TRUST.has_current || pk_eq(signer_pk, TRUST.current_src_pk)!=0) return V_UNKNOWN_KEY;
    /* (b) revoked? (DISTINCT from unknown - a key that WAS current but is killed) */
    if(ts_is_revoked(signer_pk)) return V_REVOKED_KEY;
    /* signature under the (current, non-revoked) signer key */
    uint8_t recovered[SIGNED_LEN]; unsigned long long rlen=0;
    if(crypto_sign_open(recovered, &rlen, signed_msg, SIGNED_LEN, signer_pk) != 0) return V_BAD_SIG;
    return authz_checks_2to5(recovered, rlen, served_root);
}

static void d_source(const cvsasx_oid_t *base_root);   /* fwd */
static void d_dest(const cvsasx_oid_t *base_root);
static void k_source(const cvsasx_oid_t *base_root);   /* Step 12 fwd */
static void k_dest(const cvsasx_oid_t *base_root);
static void dm_source(const cvsasx_oid_t *base_root);  /* Step 13 fwd (authority-set migration) */
static void dm_dest(const cvsasx_oid_t *base_root);

/* ===========================================================================
 * MAIN - role select then run the stages.
 * ===========================================================================*/
static void run_source(void){
    sputs("\n##### ROLE = A (SOURCE) #####\n");

    /* B1 */
    if(!b1_source()){ sputs("B1 [A] FAILED - stalling here.\n"); hcf(); }
    sputs("B1 [A] DONE.\n");

    /* B2: build the Merkle workload in store A, cold-sync to B. */
    cvsasx_store_init(&storeA, storeA_arena, sizeof storeA_arena, storeA_idx, 8192);
    cvsasx_sls_init(&SA, &storeA);
    make_leaves(&SA, 0, 0);
    cvsasx_oid_t root = build_tree(&SA, leaves, N_LEAVES);
    if(g_tree_err){ sputs("B2 [A] tree build error (store full?) - stall.\n"); hcf(); }
    sputs("B2 [A] built Merkle workload: "); sdec(storeA.index_count); sputs(" objects, "); sdec(storeA.bytes_stored);
    sputs(" total bytes; root="); sputs(hx(root.b,8)); sputc('\n');

    /* cold sync */
    sputs("B2 [A] COLD sync: serving destination-pull...\n");
    if(!a_serve(&SA, &root)){ sputs("B2 [A] cold-serve FAILED - stall.\n"); hcf(); }
    uint64_t cold = a_bytes_sent;
    sputs("B2 [A] cold sync done. bytes-on-wire(cold)="); sdec(cold); sputc('\n');

    /* B3+B4: THREE successive warm migrations, each a DIFFERENT change set. Each hop
     * rebuilds a fresh epoch on A and re-serves; a_serve resets a_bytes_sent so each
     * hop's bytes-on-wire are isolated. The hops touch DIFFERENT node paths and
     * DIFFERENT leaf counts so the per-hop bytes must DIFFER (no fixed constant). */
    static const mut_t HOP1[]={{0,0xEE}};                  /* 1 leaf, subtree-0 path  */
    static const mut_t HOP2[]={{500,0xAB}};                /* 1 leaf, a DIFFERENT subtree path */
    static const mut_t HOP3[]={{0,0xCD},{500,0xCD}};       /* 2 leaves, two paths      */
    struct { const char*name; const mut_t*mut; uint32_t n; } hops[3] = {
        {"hop1: mutate leaf 0           (1 leaf )", HOP1, 1},
        {"hop2: mutate leaf 500 (diff subtree)   (1 leaf )", HOP2, 1},
        {"hop3: mutate leaf 0 + leaf 500 (2 paths)(2 leaves)", HOP3, 2},
    };
    uint64_t hop_bytes[3];
    for(int h=0; h<3; h++){
        sputs("\nB4 [A] "); sputs(hops[h].name); sputs(" -> rebuild + warm re-sync\n");
        make_leaves(&SA, hops[h].mut, hops[h].n);
        cvsasx_oid_t rh = build_tree(&SA, leaves, N_LEAVES);
        if(g_tree_err){ sputs("B4 [A] tree build error - stall.\n"); hcf(); }
        sputs("B4 [A] new root="); sputs(hx(rh.b,8)); sputc('\n');
        if(!a_serve(&SA, &rh)){ sputs("B4 [A] warm-serve FAILED - stall.\n"); hcf(); }
        hop_bytes[h]=a_bytes_sent;
        sputs("B4 [A] hop bytes-on-wire="); sdec(hop_bytes[h]); sputc('\n');
    }

    sputs("\n===== B3/B4 MEASURED BYTES-ON-WIRE (per hop) =====\n");
    sputs("  total-state-size = "); sdec(storeA.bytes_stored); sputs(" B (all objects, cumulative)\n");
    sputs("  cold sync        = "); sdec(cold); sputs(" B (full transfer)\n");
    sputs("  warm hop1 (1 leaf , subtree A)        = "); sdec(hop_bytes[0]); sputs(" B\n");
    sputs("  warm hop2 (1 leaf , subtree B)        = "); sdec(hop_bytes[1]); sputs(" B\n");
    sputs("  warm hop3 (2 leaves, subtrees A+B)    = "); sdec(hop_bytes[2]); sputs(" B\n");
    /* the load-bearing assertion: hop3 (2 leaves) > hop1 (1 leaf) -> tracks change set */
    int b4_ok = (hop_bytes[2] > hop_bytes[0]) && hop_bytes[0] && hop_bytes[1] && (hop_bytes[0]*5 < cold);
    sputs("  B4 CHECK hop3>hop1 (2-leaf > 1-leaf)? "); sputs((hop_bytes[2]>hop_bytes[0])?"y":"n");
    sputs("  -> "); sputs(b4_ok?"PROVEN: each hop tracks ITS OWN change set, not a constant\n":"*** FAIL ***\n");

    /* ===== STEP 10 D1/D2: signed-authorization adversarial suite (A side) ===== */
    d_source(&root);
    /* ===== STEP 12 K: key-lifecycle suite (A side). Bound to the cold-sync root. */
    k_source(&root);
    dm_source(&root);   /* Step 13: authority-SET migration, AFTER k (matches dm_dest ordering on B) */
    sputs("##### A DONE - all stages observed #####\n");
}

/* A: for each case, build a (possibly malicious) signed AuthRec + serve a root,
 * post the case, wait for B's verdict, print A's framing. The legitimate case is
 * case 0; A1..A7 are the adversaries. The leaf/node TAMPER (A7) is the B5 on-wire
 * hash path, cross-referenced here (proven separately in B5). */
static uint8_t sk_use[64];
static void d_source(const cvsasx_oid_t *base_root){
    sputs("\n##### STEP 10: TRUST-BOUNDARY AUTHORIZATION (signed re-mint gate) #####\n");
    sputs("D0 [A] crypto = Ed25519 (vendored tweetnacl ref, freestanding); record binds root+scope+ceiling+nonce+expiry\n");
    /* a DIFFERENT graph root R' for the A2 root-mismatch case (serve R' but sign R). */
    make_leaves(&SA, (const mut_t[]){{7,0x77}}, 1);
    cvsasx_oid_t other_root = build_tree(&SA, leaves, N_LEAVES);

    for(uint32_t c=0; c<8; c++){
        authrec_t r;
        for(int i=0;i<32;i++) r.epoch_root_hash[i]=base_root->b[i];
        r.computation_id=EXPECT_COMP; r.dest_id=MY_DEST_ID;
        r.authority_ceiling=SRC_CEILING; r.nonce=0x1000+c; r.expiry=0xFFFFFFFF; /* far future */
        for(int i=0;i<64;i++) sk_use[i]=SRC_SK[i];
        cvsasx_oid_t served = *base_root;
        const char *label="case0 LEGIT (in-scope, fresh, signed)";
        switch(c){
            case 0: break;                                            /* legit ACCEPT */
            case 1: for(int i=0;i<64;i++) sk_use[i]=WRONG_SK[i]; label="A1 wrong-key sign      "; break;
            case 2: served=other_root; label="A2 sign R, serve R'    "; break;  /* root-mismatch */
            case 3: r.nonce=0x1000+c; for(int i=0;i<64;i++)sk_use[i]=SRC_SK[i]; r.dest_id=0xDEAD; label="A4 dest_id != B        "; break;
            case 4: r.computation_id=0xBADC0DE; label="A5 wrong computation_id"; break;
            case 5: r.authority_ceiling=SRC_CEILING+64; label="A6 ceiling > source    "; break; /* anti-amp */
            case 6: r.nonce=0x1000; label="A3 replay used nonce   "; break;     /* nonce reused (case0's) */
            case 7: r.expiry=0; label="A3b expired record     "; break;         /* expiry in past */
        }
        uint8_t signed_msg[SIGNED_LEN];
        { uint8_t flat[AUTHREC_LEN]; authrec_pack(&r, flat);
          unsigned long long smlen=0;
          crypto_sign(signed_msg, &smlen, flat, AUTHREC_LEN, sk_use);   /* sm = sig(64)||flat(64) */
        }
        /* publish: signed message (128) + served root (32) + case id, bump A seq */
        for(uint32_t i=0;i<SIGNED_LEN;i++) AUTHZ()[i]=signed_msg[i];
        bcopy_to(SHM+AUTHZ_ROOT, served.b, 32);
        *authz_case()=c; mfence(); (*authz_seqA())++; mfence();
        /* wait B verdict */
        uint64_t last=*authz_seqB();
        if(!wait_seq(authz_seqB(), last, WAIT_BUDGET)){ sputs("D2 [A] TIMEOUT case "); sdec(c); sputc('\n'); hcf(); }
        uint32_t v=*authz_verd();
        sputs("D2 [A] "); sputs(label); sputs("  -> B verdict: "); sputs(verd_name(v)); sputc('\n');
    }
    sputs("D2 [A] suite complete.\n");
}

static void run_dest(void){
    sputs("\n##### ROLE = B (DESTINATION) #####\n");

    /* B1 */
    if(!b1_dest()){ sputs("B1 [B] FAILED - stalling here.\n"); hcf(); }
    sputs("B1 [B] DONE.\n");

    /* B2: empty store, pull cold. */
    cvsasx_store_init(&storeB, storeB_arena, sizeof storeB_arena, storeB_idx, 8192);
    cvsasx_sls_init(&SB, &storeB);
    sputs("B2 [B] COLD pull from A (store empty)...\n");
    if(!b_sync(&SB)){ sputs("B2 [B] cold pull FAILED - stall.\n"); hcf(); }
    sputs("B2 [B] cold pull done. blocks="); sdec(b_blocks); sputs(" pruned="); sdec(b_pruned);
    sputs(" rejected="); sdec(b_rejected); sputs(" bytes-received="); sdec(b_received);
    sputs(" store-objects="); sdec(storeB.index_count); sputc('\n');
    uint64_t cold_blocks=b_blocks, cold_recv=b_received;
    uint32_t cold_rej=b_rejected, cold_obj=(uint32_t)storeB.index_count;

    /* B3+B4: THREE successive warm pulls. B keeps its store across hops, so each hop
     * fetches ONLY its own change set; identical subtrees are pruned by store_exists. */
    uint64_t hop_blocks[3], hop_recv[3]; uint32_t hop_rej[3], hop_obj[3];
    for(int h=0; h<3; h++){
        sputs("\nB4 [B] WARM pull hop"); sdec((uint64_t)(h+1)); sputs(" (B keeps store; identical subtrees pruned)...\n");
        if(!b_sync(&SB)){ sputs("B4 [B] warm pull FAILED - stall.\n"); hcf(); }
        hop_blocks[h]=b_blocks; hop_recv[h]=b_received; hop_rej[h]=b_rejected; hop_obj[h]=(uint32_t)storeB.index_count;
        sputs("B4 [B] hop"); sdec((uint64_t)(h+1)); sputs(" done. blocks="); sdec(b_blocks);
        sputs(" rejected="); sdec(b_rejected); sputs(" bytes-received="); sdec(b_received);
        sputs(" store-objects="); sdec(storeB.index_count); sputc('\n');
    }

    sputs("\n===== B3/B4 [B] MEASURED (per hop) =====\n");
    sputs("  cold : blocks="); sdec(cold_blocks); sputs(" recv="); sdec(cold_recv); sputs(" B rej="); sdec(cold_rej);
    sputs(" objs="); sdec(cold_obj); sputc('\n');
    for(int h=0;h<3;h++){
        sputs("  hop"); sdec((uint64_t)(h+1)); sputs(": blocks="); sdec(hop_blocks[h]); sputs(" recv="); sdec(hop_recv[h]);
        sputs(" B rej="); sdec(hop_rej[h]); sputs(" objs="); sdec(hop_obj[h]); sputc('\n');
    }
    sputs("  -> hop3 (2 leaves) fetched MORE than hop1 (1 leaf): "); sputs((hop_recv[2]>hop_recv[0])?"y":"n");
    sputs("  (warm tracks each hop's OWN change set, not a constant)\n");

    /* ===== STEP 10 D1/D2: signed-authorization adversarial suite (B side) ===== */
    /* the legit migration's root = the cold-sync root (root B installed in B2). */
    d_dest(&b2_root);
    /* ===== STEP 12 K: key-lifecycle suite (B side). Bound to the same root. */
    k_dest(&b2_root);
    /* ===== STEP 13 DM: authority-SET migration (B side). Bound to the same root. */
    dm_dest(&b2_root);
    sputs("##### B DONE - all stages observed #####\n");
}

/* B: verify each posted case through the gate, post the distinct verdict, print
 * the accept/reject TABLE - the trust-boundary proof. */
static void d_dest(const cvsasx_oid_t *base_root){
    (void)base_root;
    sputs("\n##### STEP 10: TRUST-BOUNDARY AUTHORIZATION (verify-before-remint) #####\n");
    sputs("D1 [B] gate order: (1)sig (2)root-bind (3)scope (4)nonce+expiry (5)anti-amp ceiling; each fail-closed, DISTINCT reason\n");
    b_now = 100;   /* B's clock; expiry < 100 => expired */
    seen_n = 0;
    static const char* CASEID[8]={"case0 LEGIT","A1 wrong-key","A2 root-mismatch","A4 wrong-dest",
                                  "A5 wrong-comp","A6 ceiling>src","A3 replay-nonce","A3b expired"};
    static const uint32_t WANT[8]={V_ACCEPT,V_BAD_SIG,V_ROOT_MISMATCH,V_WRONG_SCOPE,
                                   V_WRONG_SCOPE,V_ANTIAMP,V_REPLAY_OR_EXPIRED,V_REPLAY_OR_EXPIRED};
    uint32_t got[8]; int all_distinct_ok=1;
    uint64_t alast=*authz_seqA();
    for(uint32_t c=0;c<8;c++){
        if(!wait_seq(authz_seqA(), alast, WAIT_BUDGET)){ sputs("D2 [B] TIMEOUT case "); sdec(c); sputc('\n'); hcf(); }
        alast=*authz_seqA();
        uint8_t signed_msg[SIGNED_LEN]; for(uint32_t i=0;i<SIGNED_LEN;i++) signed_msg[i]=AUTHZ()[i];
        cvsasx_oid_t served; bcopy_from(served.b, SHM+AUTHZ_ROOT, 32);
        uint32_t v = authz_verify_and_remint(signed_msg, &served);
        got[c]=v;
        *authz_verd()=v; mfence(); (*authz_seqB())++; mfence();
        int ok = (v==WANT[c]); if(!ok) all_distinct_ok=0;
        sputs("  ["); sputs(CASEID[c]); sputs("] -> "); sputs(verd_name(v));
        sputs(ok?"   (intended check)\n":"   *** WRONG REASON ***\n");
    }
    sputs("\n===== D2 TRUST-BOUNDARY TABLE (each adversary rejected by its OWN distinct reason) =====\n");
    int legit_accept = (got[0]==V_ACCEPT);
    int all_rej = 1; for(int c=1;c<8;c++) if(got[c]==V_ACCEPT) all_rej=0;
    sputs("  legit case0 ACCEPT? "); sputs(legit_accept?"y":"n");
    sputs("   all 7 adversaries REJECTED? "); sputs(all_rej?"y":"n");
    sputs("   each by INTENDED check? "); sputs(all_distinct_ok?"y":"n");
    sputs("\n  -> "); sputs((legit_accept&&all_rej&&all_distinct_ok)?
        "TRUST BOUNDARY CLOSED: integrity(hash)+authorization(signed record) both enforced\n":
        "*** FAIL - authorization gate not sound ***\n");
    sputs("  (A7 tamper = the B5 leaf+node BLAKE3 rejection, proven separately; integrity backstop intact)\n");
}

/* ===========================================================================
 * STEP 12 K2 - the key-lifecycle adversarial suite. A drives a fixed script of
 * lifecycle ops + migration attempts; B applies/verifies each through the K1
 * gates and prints the KEY-LIFECYCLE TABLE. Both sides walk the SAME ordered
 * script (the step descriptors below) so they stay in lockstep by index.
 *
 * Script (monotonic epochs; each KA rejected by its OWN distinct verdict):
 *  0 L  enroll(SRC ,e1) AUTH         -> ACCEPT  (current := SRC)
 *  1 M  migrate signer=SRC           -> ACCEPT  (K1 happy: rotated-trust migrate)
 *  2 M  KA1 migrate signer=SRC2(new) -> UNKNOWN_KEY (never enrolled)
 *  3 L  KA2 rotate(SRC2) by WRONG    -> BAD_AUTHORITY (forged rotate)
 *  4 M  migrate signer=SRC           -> ACCEPT  (KA2 no effect: current still SRC)
 *  5 L  rotate(SRC2,e2) AUTH         -> ACCEPT  (current := SRC2)
 *  6 M  migrate signer=SRC2          -> ACCEPT  (K1 rotate happy path)
 *  7 M  KA4 migrate signer=SRC(old)  -> UNKNOWN_KEY (key-downgrade)
 *  8 L  KA3 replay rotate(SRC2,e2)   -> STALE_EPOCH (epoch <= stored)
 *  9 M  migrate signer=SRC2          -> ACCEPT  (KA3 didn't roll the key back)
 * 10 L  KA5 revoke(SRC2,e3) AUTH     -> ACCEPT  (SRC2 revoked)
 * 11 M  KA5 migrate signer=SRC2      -> REVOKED_KEY (use-after-revoke, DISTINCT)
 * 12 L  rotate(SRC,e5) AUTH          -> ACCEPT  (re-establish a good key = SRC)
 * 13 L  KA6 revoke(SRC,e6) by WRONG  -> BAD_AUTHORITY (forged revoke)
 * 14 M  migrate signer=SRC           -> ACCEPT  (KA6 no DoS: good key still works)
 * ===========================================================================*/
#define K_NSTEP 15
/* per-step expected verdict + label (B side checks each got==want). */
static const uint32_t KWANT[K_NSTEP]={
    V_ACCEPT, V_ACCEPT, V_UNKNOWN_KEY, V_BAD_AUTHORITY, V_ACCEPT,
    V_ACCEPT, V_ACCEPT, V_UNKNOWN_KEY, V_STALE_EPOCH, V_ACCEPT,
    V_ACCEPT, V_REVOKED_KEY, V_ACCEPT, V_BAD_AUTHORITY, V_ACCEPT };
static const char* KLABEL[K_NSTEP]={
    "L  enroll(SRC,e1) AUTH        ","M  migrate signer=SRC         ","KA1 migrate signer=SRC2(new)  ",
    "KA2 rotate(SRC2) by WRONG     ","M  migrate signer=SRC         ","L  rotate(SRC2,e2) AUTH       ",
    "M  migrate signer=SRC2        ","KA4 migrate signer=SRC(old)   ","KA3 replay rotate(SRC2,e2)    ",
    "M  migrate signer=SRC2        ","KA5 revoke(SRC2,e3) AUTH      ","KA5 migrate signer=SRC2(revkd)",
    "L  rotate(SRC,e5) AUTH        ","KA6 revoke(SRC,e6) by WRONG   ","M  migrate signer=SRC         " };

/* The wire signer_pk we present must MATCH the key used to sign (else V_BAD_SIG
 * would mask the lifecycle verdict). For unknown/old keys we present the genuine
 * pk so the lifecycle check (a)/(b) - not the signature - is what fires. */

/* A side: build + publish each scripted step, wait B's verdict, print framing. */
static void k_source(const cvsasx_oid_t *base_root){
    sputs("\n##### STEP 12: KEY-LIFECYCLE (distribution / rotation / revocation) #####\n");
    sputs("K0 [A] AUTHORITY=AUTH_PK signs enroll/rotate/revoke; migration gated on the CURRENT non-revoked key\n");
    for(uint32_t s=0; s<K_NSTEP; s++){
        uint32_t kind; uint8_t signer[32]; cvsasx_oid_t served=*base_root;
        uint8_t msg[SIGNED_LEN]; unsigned long long smlen=0;
        switch(s){
            /* ---- lifecycle ops (signed by AUTH_SK, or WRONG_SK for forgeries) ---- */
            case 0: case 3: case 5: case 8: case 10: case 12: case 13: {
                kind=K_LIFE; liferec_t lr;
                const uint8_t *lsk=AUTH_SK;
                switch(s){
                    case 0:  lr.op=LC_ENROLL; for(int i=0;i<32;i++)lr.subject_pk[i]=SRC_PK[i];  lr.epoch=1; break;
                    case 3:  lr.op=LC_ROTATE; for(int i=0;i<32;i++)lr.subject_pk[i]=SRC2_PK[i]; lr.epoch=9; lsk=WRONG_SK; break; /* forged */
                    case 5:  lr.op=LC_ROTATE; for(int i=0;i<32;i++)lr.subject_pk[i]=SRC2_PK[i]; lr.epoch=2; break;
                    case 8:  lr.op=LC_ROTATE; for(int i=0;i<32;i++)lr.subject_pk[i]=SRC2_PK[i]; lr.epoch=2; break; /* replay: <= stored */
                    case 10: lr.op=LC_REVOKE; for(int i=0;i<32;i++)lr.subject_pk[i]=SRC2_PK[i]; lr.epoch=3; break;
                    case 12: lr.op=LC_ROTATE; for(int i=0;i<32;i++)lr.subject_pk[i]=SRC_PK[i];  lr.epoch=5; break;
                    case 13: lr.op=LC_REVOKE; for(int i=0;i<32;i++)lr.subject_pk[i]=SRC_PK[i];  lr.epoch=6; lsk=WRONG_SK; break; /* forged */
                }
                uint8_t flat[LIFEREC_LEN]; liferec_pack(&lr, flat);
                crypto_sign(msg, &smlen, flat, LIFEREC_LEN, lsk);
                for(int i=0;i<32;i++) signer[i]=0;   /* unused for lifecycle */
            } break;
            /* ---- migration attempts (signed by the presented source key) -------- */
            default: {
                kind=K_MIGR;
                const uint8_t *msk; const uint8_t *spk;
                switch(s){
                    case 2:  msk=SRC2_SK; spk=SRC2_PK; break;            /* KA1 never-enrolled */
                    case 7:  msk=SRC_SK;  spk=SRC_PK;  break;            /* KA4 old key (downgrade) */
                    case 6: case 9: msk=SRC2_SK; spk=SRC2_PK; break;     /* current = SRC2 */
                    case 11: msk=SRC2_SK; spk=SRC2_PK; break;           /* KA5 revoked SRC2 */
                    default: msk=SRC_SK; spk=SRC_PK; break;             /* steps 1,4,14: current = SRC */
                }
                authrec_t r;
                for(int i=0;i<32;i++) r.epoch_root_hash[i]=base_root->b[i];
                r.computation_id=EXPECT_COMP; r.dest_id=MY_DEST_ID;
                r.authority_ceiling=SRC_CEILING; r.nonce=0x2000+s; r.expiry=0xFFFFFFFF;
                uint8_t flat[AUTHREC_LEN]; authrec_pack(&r, flat);
                crypto_sign(msg, &smlen, flat, AUTHREC_LEN, msk);
                for(int i=0;i<32;i++) signer[i]=spk[i];
            } break;
        }
        /* publish step: kind + msg + signer + served root, set A seq = s+1
         * (ABSOLUTE so a late-starting B still rendezvouses). */
        *k_kind()=kind;
        for(uint32_t i=0;i<SIGNED_LEN;i++) k_msg()[i]=msg[i];
        bcopy_to(k_signer(), signer, 32);
        bcopy_to(k_root(), served.b, 32);
        mfence(); *k_seqA()=s+1; mfence();
        if(!wait_seq_ge(k_seqB(), s+1, WAIT_BUDGET)){ sputs("K [A] TIMEOUT step "); sdec(s); sputc('\n'); hcf(); }
        uint32_t v=*k_verd();
        sputs("K [A] step "); if(s<10)sputc(' '); sdec(s); sputs("  "); sputs(KLABEL[s]);
        sputs(" -> "); sputs(verd_name(v)); sputc('\n');
    }
    sputs("K [A] suite complete.\n");
}

/* B side: receive each scripted step, dispatch to the K1 lifecycle/migration gate,
 * post the distinct verdict, and print the KEY-LIFECYCLE TABLE (the proof). */
static void k_dest(const cvsasx_oid_t *base_root){
    (void)base_root;
    sputs("\n##### STEP 12: KEY-LIFECYCLE (apply lifecycle + lifecycle-gated migrate) #####\n");
    sputs("K1 [B] migrate gate order: (a)current-key (b)not-revoked (1)sig (2)root (3)scope (4)nonce+expiry (5)anti-amp; DISTINCT reasons\n");
    ts_init();
    b_now=100; seen_n=0;
    uint32_t got[K_NSTEP]; int all_ok=1;
    for(uint32_t s=0;s<K_NSTEP;s++){
        /* wait for A's step s (ABSOLUTE: k_seqA reaches s+1) - race-free vs start time */
        if(!wait_seq_ge(k_seqA(), s+1, WAIT_BUDGET)){ sputs("K [B] TIMEOUT step "); sdec(s); sputc('\n'); hcf(); }
        uint32_t kind=*k_kind();
        uint8_t msg[SIGNED_LEN]; for(uint32_t i=0;i<SIGNED_LEN;i++) msg[i]=k_msg()[i];
        uint32_t v;
        if(kind==K_LIFE){
            v = lifecycle_apply(msg);   /* msg holds the 112-byte lifecycle sm */
        } else {
            uint8_t signer[32]; bcopy_from(signer, k_signer(), 32);
            cvsasx_oid_t served; bcopy_from(served.b, k_root(), 32);
            v = migrate_verify_lifecycle(msg, signer, &served);
        }
        got[s]=v;
        *k_verd()=v; mfence(); *k_seqB()=s+1; mfence();
        int ok=(v==KWANT[s]); if(!ok) all_ok=0;
        sputs("  ["); if(s<10)sputc(' '); sdec(s); sputs("] "); sputs(KLABEL[s]); sputs(" -> "); sputs(verd_name(v));
        sputs(ok?"   (intended)\n":"   *** WRONG REASON ***\n");
    }
    sputs("\n===== KEY-LIFECYCLE TABLE (each KA rejected by its OWN distinct verdict) =====\n");
    /* the load-bearing assertions */
    int ka1=(got[2]==V_UNKNOWN_KEY), ka2=(got[3]==V_BAD_AUTHORITY && got[4]==V_ACCEPT);
    int ka3=(got[8]==V_STALE_EPOCH && got[9]==V_ACCEPT), ka4=(got[7]==V_UNKNOWN_KEY);
    int ka5=(got[11]==V_REVOKED_KEY), ka6=(got[13]==V_BAD_AUTHORITY && got[14]==V_ACCEPT);
    int legit=(got[0]==V_ACCEPT && got[1]==V_ACCEPT && got[5]==V_ACCEPT && got[6]==V_ACCEPT);
    sputs("  legit enroll->migrate + rotate->migrate ACCEPT?  "); sputs(legit?"y":"n"); sputc('\n');
    sputs("  KA1 unknown-key      -> V_UNKNOWN_KEY            : "); sputs(ka1?"y":"n"); sputc('\n');
    sputs("  KA2 rotation-forgery -> V_BAD_AUTHORITY, key kept: "); sputs(ka2?"y":"n"); sputc('\n');
    sputs("  KA3 rotation-replay  -> V_STALE_EPOCH, key kept  : "); sputs(ka3?"y":"n"); sputc('\n');
    sputs("  KA4 key-downgrade    -> V_UNKNOWN_KEY (old key)  : "); sputs(ka4?"y":"n"); sputc('\n');
    sputs("  KA5 use-after-revoke -> V_REVOKED_KEY (distinct) : "); sputs(ka5?"y":"n"); sputc('\n');
    sputs("  KA6 revoke-forgery   -> V_BAD_AUTHORITY, no DoS  : "); sputs(ka6?"y":"n"); sputc('\n');
    sputs("  KA7 Step-10 D2 suite still green (same boot, above): see D2 TRUST-BOUNDARY TABLE\n");
    int pass = legit&&ka1&&ka2&&ka3&&ka4&&ka5&&ka6&&all_ok;
    sputs("\n  -> "); sputs(pass?
        "KEY LIFECYCLE CLOSED: distribution+rotation+revocation enforced; anti-amp backstop intact\n":
        "*** FAIL - key-lifecycle gate not sound ***\n");
}

/* per-cap bounded re-mint for the authority SET: mint a cap of cap_len as a SUB-range
 * of the source object (kept at its true length SRC_CEILING, so the region is well-formed),
 * bounded by the UNMODIFIED swcap gate. cap_len <= SRC_CEILING ACCEPTS; a cap_len beyond
 * the source authority REJECTS (BAD_BOUNDS = the anti-amp backstop). Distinct from
 * antiamp_remint (which tests the whole-object ceiling); this tests a set member's length. */
static uint32_t dm_remint(uint64_t cap_len, const cvsasx_oid_t *served_root){
    cvsasx_swcap_t croot={ (uint64_t)(uintptr_t)remint_arena, SRC_CEILING,
                           CVSASX_PERM_LOAD|CVSASX_PERM_STORE|CVSASX_PERM_GLOBAL, 1 };
    cvsasx_sw_custodian_t cust; cvsasx_sw_custodian_init(&cust, croot);
    cvsasx_sw_region_t reg; reg.object_cap=croot; reg.object_base_addr=(uint64_t)(uintptr_t)remint_arena; reg.object_length=SRC_CEILING; /* == object_cap.length (well-formed region) */
    for(int i=0;i<(int)CVSASX_BLAKE3_LEN;i++) reg.hash[i]=served_root->b[i];
    cvsasx_referent_t ref; for(int i=0;i<(int)CVSASX_BLAKE3_LEN;i++) ref.hash[i]=served_root->b[i];
    ref.object_base_addr=(uint64_t)(uintptr_t)remint_arena; ref.object_length=SRC_CEILING;
    cvsasx_pir_t pir; cvsasx_sw_cap_strip(croot,&ref,&pir);
    pir.length = cap_len;              /* request THIS cap's sub-length; the gate bounds it by the source */
    cvsasx_swcap_t out; cvsasx_status_t st = cvsasx_sw_cap_remint(&cust,&pir,&reg,&out);
    return (st==CVSASX_OK && out.valid) ? V_ACCEPT : V_ANTIAMP;
}

/* A side: build the authority SET, sign it canonically, drive the 4 cases. */
static void dm_source(const cvsasx_oid_t *base_root){
    sputs("\n##### STEP 13: AUTHORITY-SET MIGRATION (canonical set, signed, bounded re-mint) #####\n");
    dm_cap_t set[DM_NCAP];
    for(int i=0;i<32;i++){ set[0].hash[i]=base_root->b[i]; set[1].hash[i]=base_root->b[i]^0x11; set[2].hash[i]=base_root->b[i]^0x22; }
    set[0].rights=CVSASX_PERM_LOAD;                    set[0].length=128;
    set[1].rights=CVSASX_PERM_LOAD|CVSASX_PERM_STORE;  set[1].length=64;
    set[2].rights=CVSASX_PERM_LOAD;                    set[2].length=256;   /* all <= SRC_CEILING: within source authority */
    /* DM1: canonical serialization - the set and a SHUFFLED copy pack to identical bytes. */
    uint8_t flatA[DM_SET_BYTES], flatB[DM_SET_BYTES];
    dm_set_pack(set, DM_NCAP, flatA);
    dm_cap_t shuf[DM_NCAP]={set[2],set[0],set[1]}; dm_set_pack(shuf, DM_NCAP, flatB);
    int canon=1; for(uint32_t i=0;i<DM_SET_BYTES;i++) if(flatA[i]!=flatB[i]){canon=0;break;}
    sputs("DM1 [A] canonical authority-set serialization: 3 caps, "); sdec(DM_SET_BYTES);
    sputs(" bytes; reserialize (incl a shuffled order) is byte-identical="); sputs(canon?"y":"n"); sputc('\n');
    /* DM8 measure: serialize + Ed25519 sign. */
    uint64_t t0=rdtsc(); for(int i=0;i<64;i++) dm_set_pack(set,DM_NCAP,flatA); uint64_t ser=(rdtsc()-t0)/64;
    { uint8_t sm[DM_SIGNED_LEN]; unsigned long long sl=0; uint8_t sk[64]; for(int j=0;j<64;j++) sk[j]=SRC_SK[j];
      uint64_t t1=rdtsc(); crypto_sign(sm,&sl,flatA,DM_SET_BYTES,sk); uint64_t sg=rdtsc()-t1;   /* single shot: Ed25519 is ~tens of Mcyc, well above rdtsc noise */
      sputs("DM8 [A] rdtsc: serialize="); sdec(ser); sputs(" cyc; Ed25519 sign="); sdec(sg); sputs(" cyc\n"); }
    /* DM2..DM6: publish each case; B verifies and gates. */
    static const char* LBL[4]={"DMcase0 LEGIT set (3 caps, in-authority)","DM-adv1 wrong-key sign             ",
                               "DM-adv2 tampered set (cap widened post-sign = amplify)","DM-adv3 corrupt signature          "};
    for(uint32_t c=0;c<4;c++){
        uint8_t flat[DM_SET_BYTES]; dm_set_pack(set,DM_NCAP,flat);
        uint8_t sk[64]; for(int j=0;j<64;j++) sk[j]=(c==1)?WRONG_SK[j]:SRC_SK[j];
        uint8_t signed_msg[DM_SIGNED_LEN]; unsigned long long smlen=0;
        crypto_sign(signed_msg,&smlen,flat,DM_SET_BYTES,sk);           /* DM2: sign the canonical set */
        if(c==2) signed_msg[64+36] ^= 0x02;   /* widen cap0's length field AFTER signing (amplification attempt) */
        if(c==3) signed_msg[8]     ^= 0xFF;   /* corrupt the signature */
        bcopy_to(SHM+DM_MSG_OFF, signed_msg, DM_SIGNED_LEN);
        bcopy_to(SHM+DM_ROOT_OFF, base_root->b, 32);
        *dm_case()=c; mfence(); *dm_seqA()=(uint64_t)(c+1); mfence();    /* absolute seq (no toggle race) */
        if(!wait_seq_ge(dm_seqB(), (uint64_t)(c+1), WAIT_BUDGET)){ sputs("DM [A] TIMEOUT case "); sdec(c); sputc('\n'); hcf(); }
        sputs("DM [A] "); sputs(LBL[c]); sputs("  -> B verdict: "); sputs(verd_name(*dm_verd()));
        sputs("  |C_B'|="); sdec(*dm_nrem()); sputc('\n');
    }
    sputs("DM [A] authority-set suite complete.\n");
}

/* B side: verify the signature over the canonical set under A's pre-shared key, and
 * ONLY on success re-mint each cap bounded by the unmodified swcap gate (mint nothing
 * beyond the verified set). Any failure rejects fail-closed, re-minting nothing. */
static void dm_dest(const cvsasx_oid_t *base_root){
    sputs("\n##### STEP 13: AUTHORITY-SET MIGRATION (verify-before-remint, B side) #####\n");
    sputs("DM4/DM5 [B] verify Ed25519 over the canonical set under A's pre-shared key, THEN re-mint each cap bounded by the anti-amp gate; nothing minted beyond the verified set\n");
    static const char* CID[4]={"DMcase0 LEGIT","DM-adv1 wrong-key","DM-adv2 tampered-set","DM-adv3 corrupt-sig"};
    uint32_t got[4], nrem[4]; uint64_t vcyc=0;
    for(uint32_t c=0;c<4;c++){
        if(!wait_seq_ge(dm_seqA(), (uint64_t)(c+1), WAIT_BUDGET)){ sputs("DM [B] TIMEOUT case "); sdec(c); sputc('\n'); hcf(); }
        uint8_t signed_msg[DM_SIGNED_LEN]; bcopy_from(signed_msg, SHM+DM_MSG_OFF, DM_SIGNED_LEN);
        uint8_t recovered[DM_SIGNED_LEN]; unsigned long long rlen=0;
        uint64_t tv=rdtsc();
        int okc = crypto_sign_open(recovered,&rlen,signed_msg,DM_SIGNED_LEN,SRC_PK);   /* DM4: REAL Ed25519, A's pre-shared key */
        if(c==0) vcyc=rdtsc()-tv;
        uint32_t v; uint32_t nr=0;
        if(okc!=0 || rlen!=DM_SET_BYTES){ v=V_BAD_SIG; }   /* DM6/DM3: fail-closed, re-mint NOTHING */
        else {
            v=V_ACCEPT;                                    /* DM5: verified -> bounded re-mint of EACH cap */
            for(uint32_t k=0;k<DM_NCAP;k++){ uint32_t b=k*DM_CAP_BYTES;
                uint64_t len=0; for(int i=0;i<8;i++) len|=(uint64_t)recovered[b+36+i]<<(8*i);
                if(dm_remint(len,base_root)!=V_ACCEPT){ v=V_ANTIAMP; break; }   /* bound each cap by the swcap gate over B's re-verified root */
                nr++;   /* one local cap re-minted, bounded, for a verified set member (nothing beyond it) */
            }
        }
        got[c]=v; nrem[c]=nr;
        *dm_nrem()=nr; *dm_verd()=v; mfence(); *dm_seqB()=(uint64_t)(c+1); mfence();   /* absolute seq */
        sputs("  ["); sputs(CID[c]); sputs("] -> "); sputs(verd_name(v)); sputs("  |C_B'|="); sdec(nr);
        sputs(c==0?"  (== |C_A|=3, minted nothing beyond the verified set)\n":"  (fail-closed: nothing re-minted)\n");
    }
    { uint64_t t=rdtsc(); for(int i=0;i<16;i++) dm_remint(128,base_root); uint64_t rm=(rdtsc()-t)/16;
      sputs("DM8 [B] rdtsc: Ed25519 verify="); sdec(vcyc); sputs(" cyc; per-cap bounded re-mint="); sdec(rm); sputs(" cyc\n"); }
    int legit_ok=(got[0]==V_ACCEPT)&&(nrem[0]==DM_NCAP);
    int adv_rej=(got[1]!=V_ACCEPT)&&(got[2]!=V_ACCEPT)&&(got[3]!=V_ACCEPT)&&(nrem[1]==0)&&(nrem[2]==0)&&(nrem[3]==0);
    sputs("\n===== DM AUTHORITY-SET TABLE (trusted cluster: C_B' subset-or-equal C_A, B mints nothing) =====\n");
    sputs("  legit set ACCEPT, C_B'=C_A (3 caps), minted nothing beyond? "); sputs(legit_ok?"y":"n");
    sputs("   all 3 adversaries (wrong-key, tampered-set, corrupt-sig) REJECTED fail-closed (nothing re-minted)? "); sputs(adv_rej?"y":"n");
    sputs("\n  -> "); sputs((legit_ok&&adv_rej)?
        "DISTRIBUTED ANTI-AMPLIFICATION (trusted cluster): a migrant cannot arrive with more authority than A signed; a post-sign amplification breaks the Ed25519 and is rejected\n":
        "*** FAIL - authority-set gate not sound ***\n");
    sputs("  DM SCOPE: trusted-cluster (B trusts A's pre-shared key, assumes A honest about the set it signs); NOT the distrusting case (that needs hardware attestation); single-CPU per machine; the two machines are TWO QEMU instances sharing one ivshmem region.\n");
}

void kmain(void);
void kmain(void){
    serial_init();
    sputs("\n=== CARMIX nettest: two-machine rematerialization over ivshmem ===\n");
    if(!LIMINE_BASE_REVISION_SUPPORTED){ sputs("FATAL: Limine base revision unsupported\n"); hcf(); }
    if(!hhdm_req.response){ sputs("FATAL: no HHDM\n"); hcf(); }
    hhdm_off=hhdm_req.response->offset;
    sputs("B0  serial OK; HHDM="); sx64(hhdm_off); sputc('\n');

    /* frame allocator over the largest usable RAM region (for our BAR mapping) */
    uint64_t bb=0, bl=0;
    if(mm_req.response){ struct limine_memmap_response*r=mm_req.response;
        for(uint64_t i=0;i<r->entry_count;i++){ struct limine_memmap_entry*e=r->entries[i];
            if(e->type==LIMINE_MEMMAP_USABLE && e->length>bl){ bb=e->base; bl=e->length; } } }
    free_base=(bb+0xfff)&~0xfffULL; free_top=bb+bl; ram_lo=free_base; ram_hi=free_top;

    /* B0/B1: find ivshmem and map its BAR2 */
    uint8_t bus,dev,fn;
    uint64_t bar2 = find_ivshmem_bar2(&bus,&dev,&fn);
    if(!bar2){ sputs("B1  ivshmem PCI device (1af4:1110) NOT FOUND - stall.\n"); hcf(); }
    sputs("B1  ivshmem found bus="); sdec(bus); sputs(" dev="); sdec(dev); sputs(" fn="); sdec(fn);
    sputs("  BAR2 phys="); sx64(bar2); sputc('\n');

    /* map 4 MiB of BAR2 at a fixed virtual window; RW, present */
    uint64_t SHV = 0x0000600000000000ULL;
    int mapped=1;
    for(uint64_t o=0;o<(4u<<20);o+=4096){ if(!map_page(SHV+o, bar2+o, 0x3)){ mapped=0; break; } }
    if(!mapped){ sputs("B1  BAR2 map FAILED - stall.\n"); hcf(); }
    SHM=(volatile uint8_t*)SHV;
    sputs("B1  BAR2 mapped @ "); sx64(SHV); sputs(" -> probing shared memory...\n");

    /* sanity: can we read/write the shared region? write a probe, read it back. */
    volatile uint64_t *probe = (volatile uint64_t*)(SHM + 0x800);
    *probe = 0xABCDEF0123456789ULL; mfence();
    uint64_t pr = *probe;
    sputs("B1  shm probe write/read = "); sx64(pr);
    sputs(pr==0xABCDEF0123456789ULL ? "  (RW OK)\n" : "  (FAIL - BAR not RAM-backed)\n");
    if(pr!=0xABCDEF0123456789ULL){ sputs("B1  shared region not writable - stall.\n"); hcf(); }

    /* ROLE SELECTION: A is launched first by the harness and claims the magic;
     * B is launched second and sees it already claimed. */
    volatile uint64_t *magic = (volatile uint64_t*)(SHM + SH_MAGIC_OFF);
    uint64_t cur = *magic;
    int is_A;
    if(cur != SH_MAGIC){
        /* unclaimed -> I'm A: zero the B1 mailboxes AND the B2 + K seq counters, claim. */
        A2B()->flag=MB_EMPTY; B2A()->flag=MB_EMPTY;
        *seqA()=0; *seqB()=0; *k_seqA()=0; *k_seqB()=0; mfence();
        *magic = SH_MAGIC; mfence();
        is_A = 1;
    } else {
        is_A = 0;
    }
    sputs("B0  role-claim magic="); sx64(*magic); sputs(is_A?"  -> I am A (claimed)\n":"  -> I am B (saw claim)\n");

    if(is_A) run_source(); else run_dest();
    hcf();
}
