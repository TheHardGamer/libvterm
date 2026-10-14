/* Random-stress harness for libvterm. Feeds biased garbage (escape sequences,
 * UTF-8, combining marks, wide characters) into vterm_input_write(), mixed with
 * random resizes, scrollback callbacks, keyboard and mouse calls, under
 * AddressSanitizer and UBSan. A failure prints the seed that reproduces it.
 *
 * Build (GCC or Clang):
 *   cc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
 *      -std=gnu99 -Iinclude -Isrc t/fuzz.c src/*.c -o fuzz
 * Run:
 *   ./fuzz [seconds] [first-seed]     random seeds for a while, e.g. ./fuzz 60 1
 *   ./fuzz 1 4341                     replay one seed
 * Environment:
 *   FUZZ_REFLOW=0|1|2   screen reflow off, on, or random per case (default 2)
 *   FUZZ_CHUNK1=1       feed input one byte at a time
 */
#include "vterm.h"
#include <sanitizer/common_interface_defs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>

static uint64_t rng_state;
static uint32_t rnd(void){ rng_state = rng_state*6364136223846793005ULL + 1442695040888963407ULL; return (uint32_t)(rng_state>>33); }
static int rn(int n){ return n>0 ? (int)(rnd()%n) : 0; }

static uint64_t cur_seed; static int cur_rows,cur_cols;
static int damage(VTermRect r,void*u){(void)u;
  if(r.start_row<0||r.start_col<0){
    fprintf(stderr,"BAD DAMAGE seed=%llu screen=%dx%d rect rows %d..%d cols %d..%d\n",(unsigned long long)cur_seed,cur_rows,cur_cols,r.start_row,r.end_row,r.start_col,r.end_col); abort(); }
  return 1;}
static int moverect(VTermRect d,VTermRect s,void*u){(void)d;(void)s;(void)u;return 1;}
static int movecursor(VTermPos p,VTermPos o,int v,void*u){(void)p;(void)o;(void)v;(void)u;return 1;}
static int settermprop(VTermProp p,VTermValue*v,void*u){(void)p;(void)v;(void)u;return 1;}
static int bell(void*u){(void)u;return 1;}
static int resize_cb(int r,int c,VTermStateFields*f,void*u){(void)r;(void)c;(void)f;(void)u;return 1;}

#define SBMAX 64
static VTermScreenCell *sb[SBMAX]; static int sbcols[SBMAX]; static int sbn;
static int pushline(int cols,const VTermScreenCell*cells,void*u){(void)u;
    if(sbn==SBMAX){ free(sb[0]); memmove(sb,sb+1,(SBMAX-1)*sizeof(*sb)); memmove(sbcols,sbcols+1,(SBMAX-1)*sizeof(int)); sbn--; }
    sb[sbn]=malloc((size_t)cols*sizeof(VTermScreenCell)); memcpy(sb[sbn],cells,(size_t)cols*sizeof(VTermScreenCell)); sbcols[sbn]=cols; sbn++; return 1; }
static int popline(int cols,VTermScreenCell*cells,void*u){(void)u;
    if(!sbn) return 0; sbn--; int n=sbcols[sbn]<cols?sbcols[sbn]:cols;
    for(int i=0;i<cols;i++){ memset(&cells[i],0,sizeof(cells[i])); cells[i].width=1; }
    memcpy(cells,sb[sbn],(size_t)n*sizeof(VTermScreenCell)); free(sb[sbn]); return 1; }

static const char *frag[] = {
  "\x1b[","\x1b]","\x1bP","\x1b_","\x1b^","\x1bX","\x1b\\","\x07","\x1b","\x1b[?","\x1b[>","\x1b[<","\x1b[=",
  "m","H","J","K","L","M","P","@","X","S","T","A","B","C","D","E","F","G","d","h","l","r","s","u","c","n","p","q","t","b","I","Z",
  "0","1","2","3","4","5","7","8","9","10","24","80","200","2004","2026","1049","1000","1006","25","7","38;2;","38;5;","48;5;",";",":",
  "\r","\n","\t","\b","\x0e","\x0f","\x84","\x85","\x8d","\x9b","\x9d","\x90",
  "\xcc\x81","\xcc\x82","\xcd\xaf","\xe2\x80\x8d","\xef\xb8\x8f","\xe4\xb8\x80","\xef\xbc\x90","\xf0\x9f\x98\x80","\xc3\xa9","\xce\xb1","\xd0\x96",
  "\xff","\xc0","\xe2\x28","\xf0\x28",
  "hello","The quick brown fox jumps over the lazy dog 0123456789","AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
  "\x1b(0","\x1b(B","\x1b(A","\x1b#3","\x1b#4","\x1b#5","\x1b#6","\x1b[2J","\x1b[3J","\x1bc","\x1b[!p","\x1b7","\x1b8"
};
#define NFRAG ((int)(sizeof(frag)/sizeof(frag[0])))

static size_t gen(char *b,size_t cap){
  size_t n=0; int pieces=1+rn(40);
  while(pieces-- && n+300<cap){
    if(rn(10)==0){ int k=1+rn(30); while(k-- && n<cap) b[n++]=(char)rn(256); }
    else { const char*f=frag[rn(NFRAG)]; size_t l=strlen(f); memcpy(b+n,f,l); n+=l; }
  }
  return n;
}

static void on_alarm(int sig){ (void)sig; __sanitizer_print_stack_trace(); fprintf(stderr,"HANG seed=%llu screen=%dx%d\n",(unsigned long long)cur_seed,cur_rows,cur_cols); abort(); }
static void on_death(void){ fprintf(stderr,"FAILING SEED %llu (screen %dx%d)\n",(unsigned long long)cur_seed,cur_rows,cur_cols); }
int main(int argc,char**argv){
  __sanitizer_set_death_callback(on_death); signal(SIGALRM,on_alarm);
  int secs = argc>1?atoi(argv[1]):30;
  uint64_t seed0 = argc>2?strtoull(argv[2],0,10):1;
  time_t end=time(0)+secs; uint64_t iters=0, bytes=0;
  for(uint64_t seed=seed0; time(0)<end; seed++){
    rng_state=seed*2654435761ULL+12345; cur_seed=seed; alarm(6);
    int rows=1+rn(40), cols=1+rn(160); cur_rows=rows; cur_cols=cols;
    VTerm *vt=vterm_new(rows,cols); vterm_set_utf8(vt,rn(8)!=0);
    VTermState *st=vterm_obtain_state(vt); (void)st;
    static VTermStateCallbacks scb; scb.movecursor=movecursor; scb.settermprop=settermprop; scb.bell=bell; scb.resize=resize_cb;
    vterm_state_set_callbacks(st,&scb,NULL);
    VTermScreen *sc=vterm_obtain_screen(vt);
    static VTermScreenCallbacks cb; cb.damage=damage; cb.moverect=moverect; cb.movecursor=movecursor; cb.settermprop=settermprop; cb.bell=bell;
    if(rn(4)) { cb.sb_pushline=pushline; cb.sb_popline=popline; }
    else { cb.sb_pushline=NULL; cb.sb_popline=NULL; }
    vterm_screen_set_callbacks(sc,&cb,NULL);
    { const char*e=getenv("FUZZ_REFLOW"); int m=e?atoi(e):2; int r=rn(2); vterm_screen_enable_reflow(sc, m==2?r:m); }
    vterm_screen_set_sb_trim(sc,rn(2));
    vterm_screen_set_damage_merge(sc,(VTermDamageSize)rn(4));
    vterm_screen_reset(sc,1);
    int ops=5+rn(60);
    while(ops--){
      int what=rn(10);
      if(what<7){ static char buf[8192]; size_t n=gen(buf,sizeof buf); size_t off=0;
        while(off<n){ size_t c=1+rn(256); if(getenv("FUZZ_CHUNK1")) c=1; if(off+c>n)c=n-off;
          VTermPos before; vterm_state_get_cursorpos(st,&before);
          vterm_input_write(vt,buf+off,c);
          VTermPos cp; vterm_state_get_cursorpos(st,&cp);
          if(cp.row<0||cp.row>=rows||cp.col<0||cp.col>=cols){
            fprintf(stderr,"CURSOR OUT OF RANGE seed=%llu screen=%dx%d cursor=(%d,%d) before=(%d,%d) input:",(unsigned long long)cur_seed,rows,cols,cp.row,cp.col,before.row,before.col);
            for(size_t q=0;q<c;q++) fprintf(stderr," %02x",(unsigned char)buf[off+q]); fprintf(stderr,"\n"); abort(); }
          off+=c; } bytes+=n; }
      else if(what==7){ rows=1+rn(50); cols=1+rn(200); cur_rows=rows; cur_cols=cols; vterm_set_size(vt,rows,cols); }
      else if(what==8){ vterm_screen_flush_damage(sc);
        VTermScreenCell cell; for(int k=0;k<20;k++){ VTermPos p={rn(rows),rn(cols)}; vterm_screen_get_cell(sc,p,&cell); }
        char text[4096]; VTermRect r={0,rows,0,cols}; vterm_screen_get_text(sc,text,sizeof text,r); }
      else { vterm_keyboard_unichar(vt,0x41+rn(30),VTERM_MOD_NONE); vterm_keyboard_key(vt,(VTermKey)(1+rn(12)),VTERM_MOD_NONE);
             vterm_mouse_move(vt,rn(rows),rn(cols),VTERM_MOD_NONE); vterm_mouse_button(vt,1+rn(3),rn(2),VTERM_MOD_NONE);
             size_t n=vterm_output_get_buffer_current(vt); (void)n; vterm_output_read(vt,(char[256]){0},256); }
    }
    vterm_free(vt);
    for(int i=0;i<sbn;i++) free(sb[i]); sbn=0;
    iters++;
    if(iters%2000==0){ fprintf(stderr,"  ...%llu cases, last seed %llu\n",(unsigned long long)iters,(unsigned long long)seed); }
  }
  printf("fuzz: %llu cases, %.1f MB of input, seeds %llu..%llu, no crashes\n",(unsigned long long)iters,bytes/1e6,(unsigned long long)seed0,(unsigned long long)(seed0+iters-1));
  return 0;
}
