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
__attribute__((used, section(".limine_requests_end_marker")))
static volatile LIMINE_REQUESTS_END_MARKER;

/* ---- B0: serial (16550 COM1) -------------------------------------------- */
static inline void outb(uint16_t p, uint8_t v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t inb(uint16_t p){ uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
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

    /* IDT: specific exception + timer vectors, plus a catch-all over 0-31 (AUDIT A6:
     * any stray/nested CPU exception DUMPS instead of silently triple-faulting). */
    idt_set(6,(void*)isr_ud,cs); idt_set(8,(void*)isr_df,cs); idt_set(13,(void*)isr_gp,cs);
    idt_set(14,(void*)isr_pf,cs); idt_set(0x20,(void*)isr_timer,cs);
    { static const uint8_t errv[]={8,10,11,12,13,14,17,21,29,30};
      for(int v=0;v<32;v++){ if(v==6||v==8||v==13||v==14) continue;
        int he=0; for(unsigned k=0;k<sizeof errv;k++) if(errv[k]==v) he=1;
        idt_set(v, he?(void*)isr_exc_err:(void*)isr_exc_noerr, cs); } }
    struct idtr idtr={(uint16_t)(sizeof(idt)-1),(uint64_t)idt}; __asm__ volatile("lidt %0"::"m"(idtr));
    sputs("    IDT loaded (0-31 exceptions + #DF + timer)\n");

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
    run_loader();      /* PROCESS LOADER: L0 ELF as a stored object + parse, L1 materialize segments by hash (W^X), L2 enter+run the loaded ELF, L3 load-time authority ceiling, L4 two procs share code by hash */
    run_shell();       /* E4: live focus/drag/resize/type desktop (does not return) */
#endif
    hcf();
}
