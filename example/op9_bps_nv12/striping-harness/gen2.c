#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LOG(...) do{ fprintf(stderr,__VA_ARGS__); fflush(stderr);}while(0)
typedef int32_t (*pCreate)(const void*,void*,uint32_t,uint32_t,void**,uint32_t*,uint32_t*);
typedef int32_t (*pScratch)(const void*,uint32_t*);
typedef int32_t (*pExec)(void*,const void*,void*,void*,void*);
static void* lf(const char*p,size_t*n){FILE*f=fopen(p,"rb");if(!f){LOG("openfail %s\n",p);exit(2);}
  fseek(f,0,SEEK_END);long s=ftell(f);fseek(f,0,SEEK_SET);void*b=calloc(1,s);fread(b,1,s,f);fclose(f);if(n)*n=s;return b;}
int main(int c,char**v){
  if(c<5){LOG("usage\n");return 2;}
  size_t cn,in; unsigned char*cfg0=lf(v[1],&cn); void*iq0=lf(v[2],&in);
  unsigned char*cfg=calloc(1,65536); memcpy(cfg,cfg0,cn);
  void*iq=calloc(1,65536); memcpy(iq,iq0,in);
  LOG("M1 cfg=%zuB iq=%zuB; img1 fmt@0x60=%u Ystride@0xa0=%u\n",cn,in,*(uint32_t*)(cfg+0x60),*(uint32_t*)(cfg+0xa0));
  *(uint32_t*)(cfg+0xa0)=4096; *(uint32_t*)(cfg+0xa8)=4096;   // retarget OUT_FULL Y/UV stride
  LOG("M2 before dlopen %s\n",v[3]);
  void*h=dlopen(v[3],RTLD_NOW|RTLD_LOCAL);
  if(!h){LOG("dlopen FAIL: %s\n",dlerror());return 1;}
  LOG("M3 dlopen ok h=%p\n",h);
  pCreate Create=(pCreate)dlsym(h,"BPSStripingLibraryContextCreate");
  pScratch Scratch=(pScratch)dlsym(h,"IPEStripingLibraryCalculateScratchBufSize");
  pExec Exec=(pExec)dlsym(h,"BPSStripingLibraryExecute");
  LOG("M4 syms C=%p S=%p E=%p\n",(void*)Create,(void*)Scratch,(void*)Exec);
  if(!Create||!Scratch||!Exec){LOG("dlsym fail\n");return 1;}
  void*handle=0; uint32_t bufSize=0,cdmSize=0;
  LOG("M5 before Create(titan=0x60400)\n");
  int rc=Create(cfg,0,0x60400,0x60400,&handle,&bufSize,&cdmSize);
  LOG("M6 Create rc=%d handle=%p bufSize=%u cdmSize=%u\n",rc,handle,bufSize,cdmSize);
  if(rc||!handle){LOG("create fail\n");return 1;}
  LOG("M7 ctx magic=0x%016llx (expect 0x425053534c435458)\n",(unsigned long long)*(uint64_t*)handle);
  uint32_t sc=0; rc=Scratch(handle,&sc); LOG("M8 Scratch rc=%d size=%u\n",rc,sc);
  if(!sc)sc=1<<20; void*scratch=calloc(1,(size_t)sc+4096);
  size_t os=bufSize?bufSize:16384; void*out=calloc(1,os+4096);
  void*meta=calloc(1,65536);
  LOG("M8b ptrs scratch=%p out=%p meta=%p\n",scratch,out,meta);
  if(!scratch||!out||!meta){LOG("ALLOC FAIL\n");return 1;}
  struct{const void*iq;uint64_t mid;uint32_t cores;}exe={iq,0,1};  /* cores@16 per disasm */
  LOG("M9 before Execute cores=1 (iq@0 mid@8=0 cores@16)\n");
  rc=Exec(handle,&exe,out,meta,scratch); LOG("M10 Exec rc=%d (0=ok 0xa0001=badarg)\n",rc);
  if(rc){exe.cores=2; rc=Exec(handle,&exe,out,meta,scratch); LOG("M10b Exec(cores=2) rc=%d\n",rc);}
  if(!rc){FILE*f=fopen(v[4],"wb");fwrite(out,1,os,f);fclose(f);
    LOG("WROTE %s %zuB hdr=%u %u %u\n",v[4],os,((uint16_t*)out)[0],((uint16_t*)out)[1],((uint16_t*)out)[2]);}
  return rc;
}
