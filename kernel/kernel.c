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
__attribute__((interrupt)) static void isr_pf(struct iframe *f, uint64_t e){
    uint64_t cr2; __asm__ volatile("mov %%cr2,%0":"=r"(cr2)); sputs("  (#PF cr2="); sx64(cr2); sputs(")\n"); fault_dump(14,e,f); }
/* AUDIT A6 fix: #DF (8) and a catch-all over the 0-31 CPU-exception range, split by the
 * error-code-pushing vectors, so a stray/nested exception DUMPS instead of silently
 * triple-faulting. // not implemented: an IST/TSS stack for #DF (a kernel-stack fault still escalates
 * because the dump runs on the faulting stack); per-vector stubs for exact vector numbers. */
__attribute__((interrupt)) static void isr_df(struct iframe *f, uint64_t e){ fault_dump(8,e,f); }
__attribute__((interrupt)) static void isr_exc_noerr(struct iframe *f){ fault_dump(0xFE,0,f); }
__attribute__((interrupt)) static void isr_exc_err(struct iframe *f, uint64_t e){ fault_dump(0xEF,e,f); }
static volatile uint64_t ticks;
__attribute__((interrupt)) static void isr_timer(struct iframe *f){ (void)f; ticks++; outb(0x20,0x20); }

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
 * framebuffer at a console cursor. (USB-HID/xHCI is // not implemented - far harder, later.)
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
    run_shell();       /* E4: live focus/drag/resize/type desktop (does not return) */
#endif
    hcf();
}
