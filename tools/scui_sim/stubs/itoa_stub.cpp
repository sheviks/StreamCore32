extern "C" {
#include <stdio.h>
#include <string.h>
static char* conv(unsigned long v, char* s, int base, int neg){ char t[40]; int i=0; do{ int d=v%base; t[i++]= d<10?'0'+d:'a'+d-10; v/=base;}while(v); int j=0; if(neg) s[j++]='-'; while(i) s[j++]=t[--i]; s[j]=0; return s; }
char* itoa(int v, char* s, int b){ return v<0&&b==10?conv(-(long)v,s,b,1):conv((unsigned)v,s,b,0); }
char* utoa(unsigned v, char* s, int b){ return conv(v,s,b,0); }
char* ltoa(long v, char* s, int b){ return v<0&&b==10?conv(-v,s,b,1):conv((unsigned long)v,s,b,0); }
char* ultoa(unsigned long v, char* s, int b){ return conv(v,s,b,0); }
char* dtostrf(double v, signed char w, unsigned char p, char* s){ sprintf(s,"%*.*f",w,p,v); return s; }
}
