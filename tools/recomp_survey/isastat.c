/* Instruction statistics over the functions in funcs.txt (ehframe.py): counts, ISA sets,
 * indirect branches, segment use, top mnemonics. usage: isastat image.bin funcs.txt */
#include <Zydis/Zydis.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char *img; static size_t imgsz;
static unsigned long long mn[ZYDIS_MNEMONIC_MAX_VALUE+1], isa[ZYDIS_ISA_SET_MAX_VALUE+1], cat[ZYDIS_CATEGORY_MAX_VALUE+1];
int main(int argc,char**argv){
  FILE*f=fopen(argv[1],"rb"); fseek(f,0,SEEK_END); imgsz=ftell(f); fseek(f,0,SEEK_SET); img=malloc(imgsz); fread(img,1,imgsz,f); fclose(f);
  ZydisDecoder d; ZydisDecoderInit(&d,ZYDIS_MACHINE_MODE_LONG_64,ZYDIS_STACK_WIDTH_64);
  FILE*fl=fopen(argv[2],"r"); unsigned long long s,r;
  unsigned long long insns=0,fails=0,icall=0,ijmp=0,ijmp_mem=0,fsuse=0,gsuse=0,sysc=0,rdtsc=0,cpuid=0,lockp=0,ymm=0,vex=0,x87=0,rep=0,funcs=0,ret=0,dcall=0,ripmem=0,stack_rsp_write=0;
  unsigned long long jt_like=0;
  while(fscanf(fl,"%llx %llx",&s,&r)==2){ funcs++;
    unsigned long long a=s;
    while(a<s+r && a<imgsz){
      ZydisDecodedInstruction in; ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
      if(!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&d,img+a,s+r-a,&in,ops))){fails++;a++;continue;}
      insns++; mn[in.mnemonic]++; isa[in.meta.isa_set]++; cat[in.meta.category]++;
      if(in.attributes&ZYDIS_ATTRIB_HAS_LOCK) lockp++;
      if(in.attributes&(ZYDIS_ATTRIB_HAS_REP|ZYDIS_ATTRIB_HAS_REPE|ZYDIS_ATTRIB_HAS_REPNE)) rep++;
      if(in.encoding==ZYDIS_INSTRUCTION_ENCODING_VEX) vex++;
      if(in.meta.category==ZYDIS_CATEGORY_X87_ALU||in.meta.isa_set==ZYDIS_ISA_SET_X87) x87++;
      for(int i=0;i<in.operand_count_visible;i++){
        if(ops[i].type==ZYDIS_OPERAND_TYPE_REGISTER && ops[i].reg.value>=ZYDIS_REGISTER_YMM0 && ops[i].reg.value<=ZYDIS_REGISTER_YMM31){ymm++;break;}
      }
      for(int i=0;i<in.operand_count_visible;i++) if(ops[i].type==ZYDIS_OPERAND_TYPE_MEMORY){
        if(ops[i].mem.segment==ZYDIS_REGISTER_FS) fsuse++;
        if(ops[i].mem.segment==ZYDIS_REGISTER_GS) gsuse++;
        if(ops[i].mem.base==ZYDIS_REGISTER_RIP) ripmem++;
      }
      if(in.mnemonic==ZYDIS_MNEMONIC_CALL){ if(ops[0].type==ZYDIS_OPERAND_TYPE_IMMEDIATE) dcall++; else icall++; }
      if(in.mnemonic==ZYDIS_MNEMONIC_JMP && ops[0].type!=ZYDIS_OPERAND_TYPE_IMMEDIATE){ ijmp++; if(ops[0].type==ZYDIS_OPERAND_TYPE_MEMORY) ijmp_mem++; }
      if(in.mnemonic==ZYDIS_MNEMONIC_RET) ret++;
      if(in.mnemonic==ZYDIS_MNEMONIC_SYSCALL) sysc++;
      if(in.mnemonic==ZYDIS_MNEMONIC_RDTSC||in.mnemonic==ZYDIS_MNEMONIC_RDTSCP) rdtsc++;
      if(in.mnemonic==ZYDIS_MNEMONIC_CPUID) cpuid++;
      a+=in.length;
    }
  }
  printf("functions %llu instructions %llu decode-failures %llu\n",funcs,insns,fails);
  printf("direct calls %llu indirect calls %llu indirect jmps %llu (via memory %llu) rets %llu\n",dcall,icall,ijmp,ijmp_mem,ret);
  printf("fs-mem %llu gs-mem %llu rip-relative mem %llu syscall %llu rdtsc %llu cpuid %llu lock %llu rep %llu\n",fsuse,gsuse,ripmem,sysc,rdtsc,cpuid,lockp,rep);
  printf("VEX %llu ymm-using %llu x87 %llu\n",vex,ymm,x87);
  printf("-- ISA sets\n"); for(int i=0;i<=ZYDIS_ISA_SET_MAX_VALUE;i++) if(isa[i]) printf("%s %llu\n",ZydisISASetGetString(i),isa[i]);
  printf("-- distinct mnemonics: "); int dm=0; for(int i=0;i<=ZYDIS_MNEMONIC_MAX_VALUE;i++) if(mn[i]) dm++; printf("%d\n",dm);
  printf("-- top mnemonics\n");
  for(int k=0;k<40;k++){int b=-1;for(int i=0;i<=ZYDIS_MNEMONIC_MAX_VALUE;i++) if(mn[i]&&(b<0||mn[i]>mn[b])) b=i; if(b<0)break; printf("%s %llu\n",ZydisMnemonicGetString(b),mn[b]); mn[b]=0;}
  return 0;
}
