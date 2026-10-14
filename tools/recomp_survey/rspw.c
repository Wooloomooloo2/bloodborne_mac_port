/* Stack switches (mov rsp, X with X != rbp: fiber switches, stack realignment) and functions
 * whose FDE range holds undecodable bytes. usage: rspw image.bin funcs.txt */
#include <Zydis/Zydis.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char**argv){
  FILE*f=fopen(argv[1],"rb"); fseek(f,0,SEEK_END); size_t n=ftell(f); fseek(f,0,SEEK_SET); unsigned char*img=malloc(n); fread(img,1,n,f); fclose(f);
  ZydisDecoder d; ZydisDecoderInit(&d,ZYDIS_MACHINE_MODE_LONG_64,ZYDIS_STACK_WIDTH_64);
  FILE*fl=fopen(argv[2],"r"); unsigned long long s,r; int shown=0; unsigned long long cnt=0, fails_funcs=0;
  while(fscanf(fl,"%llx %llx",&s,&r)==2){ unsigned long long a=s; int bad=0;
    while(a<s+r){ ZydisDecodedInstruction in; ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
      if(!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&d,img+a,s+r-a,&in,ops))){bad=1;break;}
      if(in.mnemonic==ZYDIS_MNEMONIC_MOV && ops[0].type==ZYDIS_OPERAND_TYPE_REGISTER && ops[0].reg.value==ZYDIS_REGISTER_RSP &&
         !(ops[1].type==ZYDIS_OPERAND_TYPE_REGISTER && ops[1].reg.value==ZYDIS_REGISTER_RBP)){ cnt++; if(shown++<12) printf("mov rsp,... at %llx (func %llx)\n",a,s);}
      a+=in.length; }
    if(bad) fails_funcs++;
  }
  printf("rsp loads (not from rbp): %llu; functions with undecodable bytes: %llu\n",cnt,fails_funcs);
}
