#!/usr/bin/env python3
"""Compare RAM copy fusion with a scalar pipeline over budgets and guard misses."""
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    core = Path(__file__).resolve().parents[1]
    source = r'''
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "c66x_core_int.h"
#include "tools/c14_copy_burst.h"

static void scalar(c66x_core *c, unsigned k, uint32_t dst,
                   uint64_t q0, uint64_t q1, uint64_t tail[2]) {
 for (unsigned i=0;i<k;i++) {
  c->reg[3]=dst;
  uint64_t next; memcpy(&next,c->ram[0].host+c->reg[2]-c->ram[0].base,8);
  c->cycle++;
  c->reg[4]=(uint32_t)q0; c->reg[5]=(uint32_t)(q0>>32); c->reg[2]+=8;
  memcpy(c->ram[1].host+dst-c->ram[1].base,&q0,8);
  c->cr[CR_ILC]--; c->spl.last_iter=(int)((c->cycle-c->spl.t0+1)/2);
  c->cycle++; q0=q1; q1=next; dst+=8;
 }
 c->last_ram=1; tail[0]=q0;tail[1]=q1;
}
int main(void) {
 c66x_core *a=calloc(1,sizeof *a), *b=calloc(1,sizeof *b), *saved=malloc(sizeof *a);
 assert(a && b && saved);
 uint8_t *src=malloc(4096), *da=malloc(4096), *db=malloc(4096);
 uint8_t sp[1]={0},dp[1]={0}; assert(src && da && db);
 uint64_t seed=1;
 for (unsigned trial=0;trial<2000;trial++) {
  memset(a,0,sizeof *a); memset(da,0xa5,4096);memset(db,0xa5,4096);
  for(unsigned i=0;i<4096;i++) {seed=seed*6364136223846793005ull+1;src[i]=(uint8_t)(seed>>32);}
  a->ram[0]=(ramreg){.base=0x1000,.size=4096,.host=src,.codepage=sp};
  a->ram[1]=(ramreg){.base=0x3000,.size=4096,.host=da,.codepage=dp};
  a->nram=2;a->last_ram=1;a->cycle=101;a->spl.t0=3;a->reg[2]=0x1000+(trial%31);
  a->reg[3]=0x3000+(trial%37);a->cr[CR_ILC]=trial%300;
  if(trial & 1) { dp[0]=CP_IDLE; a->idle_rs_n=1; a->idle_rs_bloom[31]=1u<<31; }
  else dp[0]=0;
  uint32_t dst=a->reg[3]+8; uint64_t end=a->cycle+trial%701;
  uint64_t q0=seed,q1=~seed,ta[2]={0},tb[2]={0};
  memcpy(b,a,sizeof *a);b->ram[1].host=db;memcpy(saved,a,sizeof *a);
  unsigned k=jit_copy_burst(a,end,0,2,3,4,dst,q0,q1,ta);
  unsigned expect=(unsigned)((end-saved->cycle)/2);
  if(expect>saved->cr[CR_ILC]) expect=saved->cr[CR_ILC];
  if(expect>256) expect=256;
  if(expect<4) {assert(!k);assert(!memcmp(a,saved,sizeof *a));continue;}
  assert(k==expect);scalar(b,k,dst,q0,q1,tb);
  assert(!memcmp(da,db,4096));assert(!memcmp(ta,tb,sizeof ta));
  assert(!memcmp(a->reg,b->reg,sizeof a->reg));assert(!memcmp(a->cr,b->cr,sizeof a->cr));
  assert(a->cycle==b->cycle && a->last_ram==b->last_ram);
  assert(!memcmp(&a->spl,&b->spl,sizeof a->spl));
 }
 /* Each miss must leave every core byte and the destination unchanged. */
 memset(a,0,sizeof *a);
 a->ram[0]=(ramreg){.base=0x1000,.size=4096,.host=src,.codepage=sp};
 a->ram[1]=(ramreg){.base=0x3000,.size=4096,.host=da,.codepage=dp};
 a->nram=2;a->last_ram=1;a->cycle=101;a->spl.t0=3;a->reg[2]=0x1000;a->cr[CR_ILC]=32;
 for(unsigned reason=0;reason<12;reason++) {
  memcpy(b,a,sizeof *a);dp[0]=0;
  switch(reason) {
   case 0: b->ifr=1u<<C66X_INT_NMI;b->cr[CR_IER]=IER_NMIE;break;
   case 1: b->idle_armed=1;break;
   case 2: b->idle_isr_disarmed=1;break;
   case 3: b->wqn[7]=1;break;
   case 4: dp[0]=CP_CODE;break;
   case 5: dp[0]=CP_IDLE;b->idle_rs_n=1;b->idle_rs_bloom[0]=1;break;
   case 6: b->ram[1].host=src;break;
   case 7: b->ram[0].size=4;break;
   case 8: b->nhooks=1;break;
   case 9: b->npst=1;break;
   case 10: b->code_gen=1;break;
   case 11: b->imm_n=1;break;
  }
  memcpy(saved,b,sizeof *b);uint64_t tail[2]={0};
  assert(!jit_copy_burst(b,1000,0,2,3,4,0x3000,1,2,tail));
  assert(!memcmp(b,saved,sizeof *b));
 }
 /* Bitmap masks must match the core's word-by-word test, including wrap. */
 for(unsigned trial=0;trial<10000;trial++) {
  memset(a->idle_rs_bloom,0,sizeof a->idle_rs_bloom);a->idle_rs_n=1;
  seed=seed*6364136223846793005ull+1;
  uint32_t off=(uint32_t)(seed>>32)%200000,n=1+(uint32_t)seed%2048;
  uint32_t bit=(uint32_t)(seed>>16)&(IDLE_RS_BLOOM-1);
  a->idle_rs_bloom[bit/32]=1u<<(bit%32);
  int quiet=1;
  for(uint32_t w=off>>2;w<=(off+n-1)>>2;w++) {
   uint32_t k=w&(IDLE_RS_BLOOM-1);
   if(a->idle_rs_bloom[k/32]&(1u<<(k%32))) quiet=0;
  }
  assert(jit_copy_idle_quiet(a,off,n)==quiet);
 }
 free(a);free(b);free(saved);free(src);free(da);free(db);
 return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='cdj-copy-burst-test-') as directory:
        p = Path(directory) / 'check.c'
        binary = Path(directory) / 'check'
        p.write_text(source)
        subprocess.run([os.environ.get('CC', 'cc'), '-O2', '-Wall', '-Wextra',
                        '-I', str(core), str(p), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print('copy burst scalar equivalence: 2000 budget/pipeline cases, 12 guard misses and 10000 bitmap cases PASS')


if __name__ == '__main__':
    main()
