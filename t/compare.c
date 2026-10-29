/* Differential driver: runs the same random input as t/fuzz.c would, then dumps
 * everything observable (screen cells, cursor, scrolled-off rows) as text, so
 * two builds of libvterm, or two ways of keeping scrollback, can be diffed.
 *
 *   cc -O1 -std=gnu99 -Iinclude -Isrc t/compare.c src/*.c -o cmp_new
 *   MODE=cb  ./cmp_new 1 2000 > a.txt     scrollback through sb_pushline/sb_popline
 *   MODE=lib ./cmp_new 1 2000 > b.txt     scrollback kept by the library
 *   diff a.txt b.txt
 *
 * Build with -DNO_LIBSB against a libvterm that predates vterm_screen_enable_scrollback()
 * (MODE=cb only). Trimming is on in both modes and the store holds SBMAX rows.
 * DECSCNM (reverse screen) is left out of the input: the library store applies it
 * to stored rows when they are read, the callbacks received it as it was at push time. */
#include "vterm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rng_state;
static uint32_t rnd(void){ rng_state = rng_state*6364136223846793005ULL + 1442695040888963407ULL; return (uint32_t)(rng_state>>33); }
static int rn(int n){ return n>0 ? (int)(rnd()%n) : 0; }

static int damage(VTermRect r,void*u){(void)r;(void)u;return 1;}
static int settermprop(VTermProp p,VTermValue*v,void*u){(void)p;(void)v;(void)u;return 1;}

#define SBMAX 64
static VTermScreenCell *sb[SBMAX]; static int sbcols[SBMAX]; static int sbn;
static VTermScreenCell blank;
static int pushline(int cols,const VTermScreenCell*cells,void*u){(void)u;
    if(sbn==SBMAX){ free(sb[0]); memmove(sb,sb+1,(SBMAX-1)*sizeof(*sb)); memmove(sbcols,sbcols+1,(SBMAX-1)*sizeof(int)); sbn--; }
    sb[sbn]=malloc((size_t)cols*sizeof(VTermScreenCell)); memcpy(sb[sbn],cells,(size_t)cols*sizeof(VTermScreenCell)); sbcols[sbn]=cols; sbn++; return 1; }
static int clearsb(void*u){(void)u; for(int i=0;i<sbn;i++) free(sb[i]); sbn=0; return 1;}
static int popline(int cols,VTermScreenCell*cells,void*u){(void)u;
    if(!sbn) return 0; sbn--; int n=sbcols[sbn]<cols?sbcols[sbn]:cols;
    for(int i=0;i<cols;i++) cells[i]=blank;
    memcpy(cells,sb[sbn],(size_t)n*sizeof(VTermScreenCell)); free(sb[sbn]); return 1; }

static const char *frag[] = {
  "\033[","\033]","\033[?7l","\033[?7h","\033[?6h","\033[?4h","\033[4l","\033[?25l","\x07","\033",
  "m","H","J","K","L","M","P","@","X","S","T","A","B","C","D","E","F","G","d","h","l","r","s","u","b","I","Z",
  "0","1","2","3","4","5","7","8","9","10","24","80","1049","25","7","38;2;","38;5;","48;5;","58;2;",";",":","1;31","4","7","9","53",
  "\r","\n","\t","\b","\x0e","\x0f",
  "\xcc\x81","\xcc\x82","\xe2\x80\x8d","\xef\xb8\x8f","\xe4\xb8\x80","\xef\xbc\x90","\xf0\x9f\x98\x80","\xc3\xa9","\xce\xb1",
  "hello","The quick brown fox jumps over the lazy dog 0123456789","AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
  "\xe4\xb8\x80\xe4\xba\x8c\xe4\xb8\x89\xe5\x9b\x9b\xe4\xba\x94\xe5\x85\xad\xe4\xb8\x83\xe5\x85\xab\xe4\xb9\x9d\xe5\x8d\x81",
  "\033#3","\033#4","\033#6","\033[2J","\033[3J","\033c","\0337","\0338","\033]8;;http://x\x07","\033]8;;\x07"
};
#define NFRAG ((int)(sizeof(frag)/sizeof(frag[0])))
/* A random CSI sequence with plausible parameters */
static size_t gen_csi(char *b){
  static const char finals[] = "ABCDEFGHIJKLMPSTXZ@`abcdefghlmnrstu^dp";
  static const char *privs[] = { "", "", "", "?", "?", ">", "!", "$" };
  size_t n = 0;
  b[n++] = 033; b[n++] = '[';
  const char *pv = privs[rn(8)];
  while(*pv) b[n++] = *pv++;
  int np = rn(5);
  for(int i = 0; i < np; i++){
    if(i) b[n++] = rn(8) ? ';' : ':';
    if(rn(6)) n += (size_t)sprintf(b+n, "%d", rn(4) ? rn(130) : rn(300));
  }
  char f = finals[rn((int)sizeof(finals)-1)];
  b[n++] = f;
  /* private modes: only ones that are safe to compare. DECSCNM (reverse screen) is left out, the
   * library store applies it to stored rows when they are read, the callbacks got it as it was at push time */
  if(b[2] == '?' && (f == 'h' || f == 'l')) {
    static const int safe[] = { 1, 6, 7, 12, 25, 45, 2004, 1049, 1047 };
    int k = rn(getenv("ALT") ? 9 : 7); /* the alternate screen modes only with ALT=1, the old library mishandles resizes while it is up */
    n = (size_t)sprintf(b, "\033[?%d%c", safe[k], f);
  }
  return n;
}

static size_t gen(char *b,size_t cap){
  size_t n=0; int pieces=1+rn(60);
  while(pieces-- && n+300<cap){
    int k = rn(14);
    if(k == 0){ int j=1+rn(20); while(j-- && n<cap) b[n++]=(char)(32+rn(95)); }
    else if(k <= 4) n += gen_csi(b+n);
    else { const char*f=frag[rn(NFRAG)]; size_t l=strlen(f); memcpy(b+n,f,l); n+=l; }
  }
  return n;
}

static void dump_cell(const VTermScreenCell *c){
  /* the second half of a wide character has no attributes worth comparing */
  if(c->chars[0]==(uint32_t)-1){ printf("wide-tail "); return; }
  for(int i=0;i<VTERM_MAX_CHARS_PER_CELL;i++){ printf("%x.",c->chars[i]); if(!c->chars[i]) break; }
  printf("/%d", c->width);
  unsigned a = c->attrs.bold | c->attrs.underline<<1 | c->attrs.italic<<3 | c->attrs.blink<<4 | c->attrs.reverse<<5 |
               c->attrs.conceal<<6 | c->attrs.strike<<7 | c->attrs.font<<8 | c->attrs.small<<12 | c->attrs.baseline<<13 |
               c->attrs.dwl<<15 | c->attrs.dhl<<16;
  printf("/%x", a);
  const VTermColor *k[3]={&c->fg,&c->bg,&c->uline};
  for(int i=0;i<3;i++){
    if(i==2 && !c->attrs.underline){ printf("/-"); continue; } /* only drawn under an underline */
    printf("/%02x:%d,%d,%d", k[i]->type, k[i]->rgb.red, k[i]->rgb.green, k[i]->rgb.blue); }
  printf("/%u ", c->link);
}

int main(int argc,char**argv){
  uint64_t seed0 = argc>1?strtoull(argv[1],0,10):1, count = argc>2?strtoull(argv[2],0,10):1000;
  const char *m = getenv("MODE"); int lib = m && !strcmp(m,"lib");
#ifdef NO_LIBSB
  if(lib){ fprintf(stderr,"no library scrollback in this build\n"); return 2; }
#endif
  for(uint64_t seed=seed0; seed<seed0+count; seed++){
    rng_state=seed*2654435761ULL+12345;
    int rows=1+rn(30), cols=1+rn(120);
    VTerm *vt=vterm_new(rows,cols); vterm_set_utf8(vt,1);
    VTermState *st=vterm_obtain_state(vt); (void)st;
    VTermScreen *sc=vterm_obtain_screen(vt);
    static VTermScreenCallbacks cb; cb.damage=damage; cb.settermprop=settermprop;
    if(!lib){ cb.sb_pushline=pushline; cb.sb_popline=popline;
    cb.sb_clear=clearsb; }
    vterm_screen_set_callbacks(sc,&cb,NULL);
    { int r = rn(2); vterm_screen_enable_reflow(sc, getenv("REFLOW") ? r : 0); }
    vterm_screen_enable_altscreen(sc, getenv("ALT") ? 1 : 0);
    vterm_screen_set_sb_trim(sc,1);
    vterm_screen_set_damage_merge(sc,VTERM_DAMAGE_ROW);
#ifndef NO_LIBSB
    if(lib) vterm_screen_enable_scrollback(sc,SBMAX,(size_t)64<<20);
#endif
    vterm_screen_reset(sc,1);
    vterm_screen_get_cell(sc,(VTermPos){0,0},&blank);
    if(getenv("TRACE")) fprintf(stderr,"init %dx%d\n",rows,cols);
    int ops=3+rn(40);
    int stop = getenv("STOP") ? atoi(getenv("STOP")) : -1, opn = 0;
    while(ops--){
      if(opn++ == stop) break;
      int what=rn(10);
      if(what<8){ static char buf[8192]; size_t n=gen(buf,sizeof buf); size_t off=0;
        while(off<n){ size_t c=1+rn(300); if(off+c>n)c=n-off; vterm_input_write(vt,buf+off,c);
          if(getenv("TRACE")){ fprintf(stderr,"op %d chunk:",opn-1); for(size_t q=0;q<c;q++) fprintf(stderr," %02x",(unsigned char)buf[off+q]); fprintf(stderr,"\n"); }
          off+=c; } }
      else { rows=1+rn(30); cols=1+rn(120); vterm_set_size(vt,rows,cols); if(getenv("TRACE")) fprintf(stderr,"op %d resize %dx%d\n",opn-1,rows,cols); }
    }
    vterm_screen_flush_damage(sc);
    VTermPos cp; vterm_state_get_cursorpos(st,&cp);
    printf("seed %llu %dx%d cursor %d,%d\n",(unsigned long long)seed,rows,cols,cp.row,cp.col);
    for(int r=0;r<rows;r++){ printf("R%d: ",r); for(int c=0;c<cols;c++){ VTermScreenCell cell; vterm_screen_get_cell(sc,(VTermPos){r,c},&cell); dump_cell(&cell);} printf("\n"); }
#ifndef NO_LIBSB
    if(lib){
      size_t n=vterm_screen_scrollback_count(sc);
      printf("scrollback %zu\n", n);
      static VTermScreenCell row[4096];
      for(size_t i=0;i<n;i++){ bool cont; int w=vterm_screen_scrollback_get(sc,i,row,4096,&cont); printf("S%zu(%d): ",i,w); for(int c=0;c<w;c++) dump_cell(&row[c]); printf("\n"); }
    } else
#endif
    {
      printf("scrollback %d\n", sbn);
      for(int i=0;i<sbn;i++){ printf("S%d(%d): ",i,sbcols[i]); for(int c=0;c<sbcols[i];c++) dump_cell(&sb[i][c]); printf("\n"); }
    }
    vterm_free(vt);
    for(int i=0;i<sbn;i++) free(sb[i]); sbn=0;
  }
  return 0;
}
