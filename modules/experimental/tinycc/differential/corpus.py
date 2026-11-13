"""Defined-input code and ABI cases shared verbatim by GCC and both TinyCC hosts."""
import random


def curated():
    # Each entry resets its state. No observables contain absolute addresses,
    # padding bytes, signed overflow, invalid shifts or unsequenced side effects.
    return {
        "unsigned_arithmetic": ("", r'''
    uint32_t d = y | 1U;
    out[0]=x+y; out[1]=x-y; out[2]=x*y; out[3]=x/d; out[4]=x%d;
    out[5]=(x^y)+(x&y); out[6]=~x; out[7]=(x|y)^(x&~y);
    return (x+0x80000000U)*(y+0xffff0001U);
'''),
        "signed_arithmetic": ("", r'''
    int32_t a=(int32_t)(x%20001U)-10000, b=(int32_t)(y%201U)-100;
    if (!b) b=-1;
    out[0]=a+b; out[1]=a-b; out[2]=a*b; out[3]=a/b; out[4]=a%b;
    out[5]=-a; out[6]=(a/b)*b+a%b; out[7]=(uint32_t)a/((uint32_t)b|1U);
    return (uint32_t)((a+10001)*(b+101));
'''),
        "shifts": ("", r'''
    unsigned s=y&31U;
    int32_t a=(int32_t)(x%2000001U)-1000000;
    out[0]=x<<s; out[1]=x>>s; out[2]=a>>s; out[3]=(uint32_t)a<<s;
    out[4]=(x<<s)|(x>>((32U-s)&31U)); out[5]=(x>>s)^(y<<((s+7U)&31U));
    return (x<<31)^(x>>31)^((uint32_t)(a>>31));
'''),
        "casts": ("", r'''
    signed char a=(signed char)x; unsigned char b=(unsigned char)x;
    short c=(short)y; unsigned short d=(unsigned short)y;
    volatile signed char aa=a; volatile unsigned char bb=b;
    volatile short cc=c; volatile unsigned short dd=d;
    out[0]=a; out[1]=b; out[2]=c; out[3]=d;
    out[4]=aa; out[5]=bb; out[6]=cc; out[7]=dd;
    out[8]=(uint32_t)(int32_t)(int8_t)(x+129U);
    out[9]=(uint32_t)(int32_t)(int16_t)(y+32769U);
    out[10]=(char)255; out[11]=sizeof(char)+16U*sizeof(short)+256U*sizeof(long);
    return (uint32_t)(aa+bb+cc+dd);
'''),
        "booleans": ("", r'''
    _Bool a=x, b=y, c=(x==y); volatile _Bool stored=a;
    out[0]=a; out[1]=b; out[2]=c; out[3]=stored;
    out[4]=!x; out[5]=!!y; out[6]=(x<y)+(x>y)*2U+(x==y)*4U;
    a=(x&255U); b=(y&256U); stored=(x^y);
    out[7]=a; out[8]=b; out[9]=stored;
    return (a?0xdeadbeefU:0x12345678U)^(b?x:y);
'''),
        "comparisons": ("", r'''
    int32_t a=(int32_t)(x%2000001U)-1000000, b=(int32_t)(y%2000001U)-1000000;
    out[0]=(x<y); out[1]=(x<=y); out[2]=(x>y); out[3]=(x>=y);
    out[4]=(x==y); out[5]=(x!=y); out[6]=(a<b); out[7]=(a<=b);
    out[8]=(a>b); out[9]=(a>=b); out[10]=(a==b); out[11]=(a!=b);
    out[12]=((uint32_t)a<y); out[13]=((uint32_t)b>=x);
    out[14]=(a<0)?x:y; out[15]=(x<y)?(a<b):(a>=b);
    return ((a<b)&&(x>=y))|(((a==b)||(x!=y))<<1);
'''),
        "short_circuit": (r'''
static uint32_t count;
static uint32_t step(uint32_t value) { count++; return value; }
''', r'''
    count=0;
    out[0]=x&&step(y); out[1]=count;
    out[2]=x||step(y); out[3]=count;
    out[4]=(x&1U)?step(x+1U):step(y+1U); out[5]=count;
    out[6]=(x&&step(y))||(step(x^y)&&step(7)); out[7]=count;
    out[8]=(step(x),step(y),count); out[9]=count;
    return count;
'''),
        "loops_switch": ("", r'''
    uint32_t a=x, b=y;
    for (unsigned i=0; i<19; i++) {
        if (i==3) continue;
        a=(a*33U)^i;
        if ((a&31U)==7) break;
        b+=a;
    }
    unsigned n=x&7U;
    do { a^=b; b=b*17U+1U; } while (n--);
    while (a&3U) { a--; b++; }
    switch (y&7U) {
    case 0: a+=1; break; case 1: a+=11;
    case 2: a+=22; break; case 3: a+=33; break;
    case 6: a+=66; break; default: a+=99;
    }
    out[0]=a; out[1]=b; return a^b;
'''),
        "pointer_array": ("", r'''
    uint32_t a[24]; for (unsigned i=0;i<24;i++) a[i]=x+i*y;
    uint32_t *p=a+(x%8U), *q=a+16+(y%8U);
    out[0]=*p++; out[1]=*++p; out[2]=*q--; out[3]=*--q;
    out[4]=q-p; out[5]=p<q; out[6]=p!=q; out[7]=q[-2];
    *p+=y; out[8]=a[x%8U+2];
    const uint32_t *r=(x&1U)?p:q; out[9]=r-a;
    return r[0]^r[-1];
'''),
        "stack_offsets": ("", r'''
    volatile uint32_t a[384];
    for (unsigned i=0;i<384;i++) a[i]=x^(y+i);
    out[0]=a[0]; out[1]=a[255]; out[2]=a[256]; out[3]=a[383];
    unsigned n=x%384U; a[n]+=0x89abcdefU;
    out[4]=a[n]; return a[0]+a[383]+a[(n+113U)%384U];
'''),
        "struct_members": ("", r'''
    struct S { uint8_t a; int16_t b; uint32_t c; uint8_t d[5]; } s;
    struct S *p=&s; s.a=x; s.b=y; s.c=x^y;
    for (unsigned i=0;i<5;i++) s.d[i]=(uint8_t)(x+i);
    out[0]=p->a; out[1]=p->b; out[2]=p->c; out[3]=p->d[y%5U];
    out[4]=sizeof(s); out[5]=(unsigned char *)&s.c-(unsigned char *)&s;
    p->b=(int16_t)(p->b+1); out[6]=p->b;
    return p->c+p->a+(uint32_t)p->b;
'''),
        "bitfields": ("", r'''
    struct B { unsigned a:3; signed b:5; unsigned c:17; unsigned d:7; } s;
    s.a=x; s.b=x>>3; s.c=y; s.d=y>>17;
    out[0]=s.a; out[1]=s.b; out[2]=s.c; out[3]=s.d;
    s.a+=1; s.b=-3; s.c^=0x12345U;
    out[4]=s.a; out[5]=s.b; out[6]=s.c; out[7]=sizeof(s);
    return s.a+(uint32_t)s.b+s.c+s.d;
'''),
        "object_bytes": ("", r'''
    uint32_t a[2]={x,y}; unsigned char *p=(unsigned char *)a;
    for (unsigned i=0;i<8;i++) out[i]=p[i];
    p[1]^=0xff; p[6]=0x80; out[8]=a[0]; out[9]=a[1];
    return a[0]^a[1];
'''),
        "global_relocations": (r'''
static uint32_t values[8];
static uint32_t *pointers[]={values+1,values+4,values+7};
static const char *names[]={"SolarOS","native","compiler"};
static uint32_t bss;
''', r'''
    bss=x+y; for (unsigned i=0;i<8;i++) values[i]=bss+i*y;
    out[0]=*pointers[0]; out[1]=*pointers[1]; out[2]=*pointers[2];
    out[3]=names[0][x%7U]; out[4]=names[1][y%6U];
    out[5]=pointers[2]-values; out[6]=names[2][8];
    return bss^*pointers[x%3U];
'''),
        "recursion": (r'''
static uint32_t recurse(uint32_t a,uint32_t b,unsigned n) {
    if (!n) return a^b;
    uint32_t keep=a*17U+b;
    return keep+recurse(b+1U,a^0x87654321U,n-1);
}
''', r'''
    out[0]=recurse(x,y,1); out[1]=recurse(x,y,4); out[2]=recurse(x,y,12);
    return recurse(x^y,y+7U,8);
'''),
        "stack_arguments": (r'''
static uint32_t many(uint32_t a,uint32_t b,uint32_t c,uint32_t d,
 uint32_t e,uint32_t f,uint32_t g,uint32_t h,uint32_t i,uint32_t j,
 uint32_t k,uint32_t l) {
 return a+3U*b+5U*c+7U*d+11U*e+13U*f+17U*g+19U*h+23U*i+29U*j+31U*k+37U*l;
}
''', r'''
    out[0]=many(x,y,x+y,x-y,x^y,0,0xffffffffU,x*3U,y*5U,19,23,42);
    out[1]=many(y,x,1,2,3,4,5,6,7,8,9,10);
    out[2]=api->callback12(many,x,y);
    return many(out[0],out[1],x,y,0,1,2,3,4,5,6,7);
'''),
        "gcc_calls": ("", r'''
    out[0]=api->mix8(x,y,x+y,x-y,x^y,0xffffffffU,0,0x80000000U);
    out[1]=api->narrow((int8_t)x,(uint8_t)y,(int16_t)x,(uint16_t)y);
    out[2]=api->variadic(10,x,y,1U,2U,3U,4U,5U,6U,7U,8U);
    out[3]=api->variadic(3,(signed char)-7,(unsigned short)65535,(short)-32768);
    out[4]=api->signed_byte(x); out[5]=api->unsigned_byte(x);
    out[6]=api->signed_half(y); out[7]=api->unsigned_half(y);
    return api->mix8(out[0],out[1],out[2],out[3],out[4],out[5],out[6],out[7]);
'''),
        "callbacks": (r'''
static uint32_t cb(uint32_t a,uint32_t b) { return (a*33U)^(b+0x87654321U); }
static uint32_t (*functions[2])(uint32_t,uint32_t)={cb,cb};
''', r'''
    out[0]=api->callback(cb,x,y);
    out[1]=functions[x&1U](y,x);
    out[2]=api->callback(functions[y&1U],out[0],out[1]);
    return functions[0](out[2],api->callback(cb,x^y,y+1U));
'''),
        "register_pressure": (r'''
static uint32_t m(uint32_t a,uint32_t b) { return (a+0x12345678U)^(b*31U); }
''', r'''
    uint32_t a=x+1U,b=y+2U,c=x^3U,d=y^4U,e=x*5U,f=y*6U,g=x-7U,h=y-8U;
    out[0]=m(a,b)+m(c,d)+m(e,f)+m(g,h);
    out[1]=m(m(a,b),m(m(c,d),m(e,f)))^m(g,m(h,a));
    out[2]=api->mix8(m(a,b),m(b,c),m(c,d),m(d,e),m(e,f),m(f,g),m(g,h),m(h,a));
    out[3]=a+b+c+d+e+f+g+h;
    return m(out[0],out[1])+m(out[2],out[3]);
'''),
        "lvalue_updates": (r'''
static uint32_t update(uint32_t x) { return x*17U+1U; }
''', r'''
    struct E { uint32_t field; } data[8];
    for (unsigned i=0;i<8;i++) data[i].field=x+i*y;
    struct E *p=data;
    out[0]=p++->field++; out[1]=++p->field; out[2]=p-data;
    (p++)->field+=update(x); out[3]=p-data;
    data[y&7U].field^=update(x^y);
    for (unsigned i=0;i<8;i++) out[4+i]=data[i].field;
    return data[0].field+data[1].field+data[2].field;
'''),
        "alignment": (r'''
static uint32_t aligned_global[4] __attribute__((aligned(16)))={1,2,3,4};
''', r'''
    uint32_t local[4] __attribute__((aligned(16)));
    local[0]=x; local[1]=y; local[2]=x+y; local[3]=x-y;
    out[0]=(uintptr_t)local&15U; out[1]=(uintptr_t)aligned_global&15U;
    out[2]=sizeof(local); out[3]=local[x&3U];
    return aligned_global[y&3U]+local[y&3U];
'''),
        "memory_helpers": (r'''
extern void *memcpy(void *,const void *,unsigned);
extern void *memmove(void *,const void *,unsigned);
extern void *memset(void *,int,unsigned);
''', r'''
    unsigned char data[40],copy[40];
    memset(data,(int)(x&255U),sizeof(data));
    for (unsigned i=0;i<17;i++) data[i]=(unsigned char)(y+i);
    memcpy(copy,data,sizeof(data)); memmove(copy+3,copy,29);
    for (unsigned i=0;i<24;i++) out[i]=copy[i];
    out[24]=(memset(data,0,sizeof(data))==data);
    return copy[31]+copy[32]+data[0];
'''),
    }


def expression(rng, depth):
    if not depth or rng.randrange(5) == 0:
        return rng.choice(("x", "y", "a", "b", f"0x{rng.getrandbits(32):08x}U"))
    left, right = expression(rng, depth-1), expression(rng, depth-1)
    op = rng.choice(("+", "-", "*", "^", "|", "&", "/", "%", "<<", ">>", "<", "=="))
    if op in ("/", "%"): right = f"(({right})|1U)"
    if op in ("<<", ">>"): right = f"(({right})&31U)"
    # Relational results are explicitly unsigned, keeping all arithmetic modulo
    # 2**32 even when an entire generated subtree contains only comparisons.
    return f"((uint32_t)(({left}){op}({right})))"


def wide_cases():
    return {
        "wide_arithmetic": ("", r'''
    uint64_t a=((uint64_t)x<<32)|y,b=((uint64_t)y<<32)|(x^0x87654321U);
    STORE64(0,a+b); STORE64(2,a-b); STORE64(4,a*b);
    STORE64(6,a^b); STORE64(8,a&b); STORE64(10,a|b); STORE64(12,~a);
    return (uint32_t)((a+b)>>32)^(uint32_t)((a*b)>>32);
'''),
        "wide_division": ("", r'''
    uint64_t a=((uint64_t)x<<32)|y,b=(((uint64_t)y<<32)|x)|1ULL;
    STORE64(0,a/b); STORE64(2,a%b);
    int64_t sa=(int64_t)(int32_t)x*65537LL,sb=(int64_t)(int32_t)y*127LL;
    if(!sb) sb=-1;
    STORE64(4,sa/sb); STORE64(6,sa%sb); STORE64(8,sa+sb); STORE64(10,sa-sb);
    STORE64(12,(int64_t)(int32_t)x*(int64_t)(int32_t)y);
    return (uint32_t)(a/b)^(uint32_t)(sa/sb);
'''),
        "wide_shifts": ("", r'''
    uint64_t a=((uint64_t)x<<32)|y; unsigned s=y&63U;
    int64_t b=(int64_t)(int32_t)x*4294967296LL+y;
    STORE64(0,a<<s); STORE64(2,a>>s); STORE64(4,b>>s);
    STORE64(6,a<<31); STORE64(8,a>>32); STORE64(10,a<<33); STORE64(12,b>>63);
    return (uint32_t)((a<<s)>>32);
'''),
        "wide_comparisons": ("", r'''
    uint64_t a=((uint64_t)x<<32)|y,b=((uint64_t)y<<32)|x;
    int64_t sa=(int64_t)(int32_t)x*4294967296LL+y,sb=(int64_t)(int32_t)y*4294967296LL+x;
    out[0]=a<b;out[1]=a<=b;out[2]=a>b;out[3]=a>=b;out[4]=a==b;out[5]=a!=b;
    out[6]=sa<sb;out[7]=sa<=sb;out[8]=sa>sb;out[9]=sa>=sb;out[10]=sa==sb;out[11]=sa!=sb;
    out[12]=(a&&b);out[13]=a?1:0;STORE64(14,(a<b)?a:b);
    switch(((uint64_t)(x&3U)<<32)|(y&1U)) {
    case 0:out[16]=17;break;case 1:out[16]=23;break;
    case 0x100000000ULL:out[16]=31;break;case 0x100000001ULL:out[16]=37;break;
    case 0x200000000ULL:out[16]=41;break;case 0x200000001ULL:out[16]=43;break;
    default:out[16]=47;break;
    }
    return (sa<sb)+(sa>sb)*2U+(a>=b)*4U;
'''),
        "wide_calls": (r'''
static uint64_t mixed(uint32_t a,uint64_t b,uint32_t c,uint64_t d,uint32_t e)
{ return (b*0x100000001ULL)^(d+a+c+e); }
static uint64_t edge(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint64_t f,uint32_t g)
{ return f+a+b+c+d+e+g; }
''', r'''
    uint64_t a=((uint64_t)x<<32)|y,b=((uint64_t)y<<32)|x;
    STORE64(0,mixed(x,a,y,b,x^y));STORE64(2,api->wide(x,a,y,b,x^y));
    STORE64(4,api->wide_callback(mixed,x,y));STORE64(6,edge(1,2,3,4,5,a,y));
    STORE64(8,api->wide_variadic(3,a,b,a^b));
    return (uint32_t)(mixed(x,a,y,b,7)>>32);
'''),
        "wide_memory": ("", r'''
    uint64_t values[8];for(unsigned i=0;i<8;i++) values[i]=((uint64_t)(x+i)<<32)|(y-i);
    uint64_t *p=values+(x&3U);STORE64(0,*p++);STORE64(2,*p);
    *p+=0x80000000ffffffffULL;STORE64(4,*p);STORE64(6,values[y&7U]);
    volatile uint64_t saved=values[0];STORE64(8,saved);
    struct W {uint32_t a;uint64_t b;uint32_t c;} value={x,saved,y};
    STORE64(10,value.b);out[12]=sizeof(value);out[13]=(_Bool)value.b;
    return (uint32_t)(saved>>32);
'''),
        "time_abi": ("", r'''
    const solar_os_native_time_api_v1_t *clock=solar_os_native_host_v1()->get_service("time",1,sizeof(*clock));
    if(!clock) return 9;
    int64_t a=clock->now_us(),b=clock->now_us();
    out[0]=a>=0;out[1]=b>=a;out[2]=(uint64_t)(b-a)<0x100000000ULL;
    return out[0]+out[1]+out[2];
'''),
    }


def extended_cases():
    return {
        "floating_literals": ("", r'''
    F32(0,0x1p-149f);F32(1,-0.0f);F32(2,1.23456789f);
    F64(4,0x1.0000000000001p-1022);F64(6,0x1.fffffffffffffp1023);
    F64(8,1.2345678901234567);F64(10,-0.0);F64(12,0x1p-1074);
    return out[0]^out[4]^out[8];
'''),
        "float_arithmetic": ("", r'''
    float a=(float)((int32_t)(x%2000001U)-1000000)/33.0f;
    float b=(float)((y%1001U)+1U)/17.0f;
    F32(0,a+b);F32(1,a-b);F32(2,a*b);F32(3,a/b);F32(4,-a);
    out[5]=a<b;out[6]=a>=b;out[7]=a==b;out[8]=a!=b;
    out[9]=a?1:0;out[10]=(int32_t)(a/1000.0f);
    return out[0]^out[2]^out[4];
'''),
        "double_arithmetic": ("", r'''
    double a=(double)(int32_t)x*0.25+0.5,b=(double)(y%1001U)+1.0;
    F64(0,a+b);F64(2,a-b);F64(4,a*b);F64(6,a/b);F64(8,-a);
    out[10]=a<b;out[11]=a>=b;out[12]=a==b;out[13]=a!=b;
    long double l=(long double)a+1.25L;F64(14,(double)l);
    return out[0]^out[4]^out[8];
'''),
        "float_conversions": ("", r'''
    uint64_t wide=((uint64_t)x<<32)|y;int64_t signed_wide=(int64_t)(int32_t)x*4294967296LL+y;
    F32(0,(float)x);F32(1,(float)(int32_t)x);F32(2,(float)wide);F32(3,(float)signed_wide);
    F64(4,(double)x);F64(6,(double)(int32_t)x);F64(8,(double)wide);F64(10,(double)signed_wide);
    double a=(double)((int32_t)(x%2000001U)-1000000)+0.25;
    float b=(float)((y&65535U)+1U)+0.5f;
    out[12]=(int32_t)a;out[13]=(uint32_t)b;STORE64(14,(int64_t)a);STORE64(16,(uint64_t)b);
    F32(18,(float)a);F64(20,(double)b);
    return out[0]^out[8]^out[14]^out[18];
'''),
        "float_special": ("", r'''
    static const uint32_t bits[]={0,0x80000000U,0x7f800000U,0xff800000U,0x7fc12345U,1,0x7f7fffffU,0x3f800000U};
    union {uint32_t bits;float value;} a,b;a.bits=bits[x&7U];b.bits=bits[y&7U];
    out[0]=a.value==b.value;out[1]=a.value!=b.value;out[2]=a.value<b.value;
    out[3]=a.value<=b.value;out[4]=a.value>b.value;out[5]=a.value>=b.value;
    out[6]=a.value!=a.value;out[7]=!a.value;out[8]=(_Bool)a.value;
    return out[0]+out[1]*2U+out[6]*4U;
'''),
        "floating_abi": (r'''
static double floating(double a,float b,uint64_t c,uint32_t d,double e)
{return (a+(double)b)+(double)(c&65535U)+(double)d+e;}
''', r'''
    double a=(double)(int32_t)x/4.0,b=(double)(y%1024U)/8.0;
    uint64_t w=((uint64_t)x<<32)|y;
    F64(0,api->floating(a,(float)b,w,x&65535U,b));
    const solar_os_native_libc_api_v1_t *lib=solar_os_native_host_v1()->get_service("libc",1,sizeof(*lib));
    if(!lib)return 1;
    char *end;F32(2,lib->parse_float("1.25",&end));out[3]=*end;
    F64(4,lib->parse_double("9007199254740991",&end));out[6]=*end;
    F64(8,lib->floor_value(a));F32(10,lib->sqrt_float((float)(y&65535U)));
    F64(12,floating(a,(float)b,w,x&65535U,b));
    F64(14,api->floating_callback(floating,a,(float)b,w,x&65535U,b));
    return out[0]^out[4]^out[8];
'''),
        "struct_returns": (r'''
static diff_triple_t tfn(diff_triple_t s,uint32_t n){s.a+=n;s.b^=n;s.c*=n|1U;return s;}
static diff_large_t lfn(diff_large_t s,uint32_t n){for(unsigned i=0;i<5;i++)s.words[i]=s.words[i]*33U+n+i;return s;}
''', r'''
    diff_triple_t a={x,y,x^y};a=tfn(a,17);out[0]=a.a;out[1]=a.b;out[2]=a.c;
    a=api->triple(a,23);out[3]=a.a;out[4]=a.b;out[5]=a.c;
    out[6]=api->triple_callback(tfn,x,y);
    diff_large_t b={{x,y,x+y,x-y,x^y}};b=lfn(b,19);
    for(unsigned i=0;i<5;i++)out[7+i]=b.words[i];
    b=api->large(b,31);for(unsigned i=0;i<5;i++)out[12+i]=b.words[i];
    out[17]=api->large_callback(lfn,x,y);
    return out[0]^out[7]^out[17];
'''),
        "struct_arguments": (r'''
struct S {unsigned char bytes[3];};
static struct S tiny(struct S s,unsigned x){s.bytes[1]^=x;return s;}
static uint32_t stack(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,diff_triple_t s,uint32_t f)
{return a+b+c+d+e+s.a+s.b+s.c+f;}
struct A {uint64_t wide;uint32_t n;};
static uint64_t aligned(uint32_t a,uint32_t b,uint32_t c,struct A value,uint32_t d)
{return value.wide+a+b+c+value.n+d;}
''', r'''
    struct S a={{(unsigned char)x,(unsigned char)y,0x80}};a=tiny(a,x>>8);
    out[0]=a.bytes[0];out[1]=a.bytes[1];out[2]=a.bytes[2];
    diff_triple_t triple={x,y,x^y};out[3]=stack(1,2,3,4,5,triple,17);
    struct A value={((uint64_t)x<<32)|y,x^y};STORE64(4,aligned(1,2,3,value,7));
    return out[0]^out[3]^out[4];
'''),
        "variadic_definitions": (r'''
static uint64_t collect(unsigned count,...){
 va_list ap,copy;va_start(ap,count);va_copy(copy,ap);
 unsigned first=va_arg(ap,unsigned);uint64_t second=va_arg(ap,uint64_t);
 double third=va_arg(ap,double);unsigned fourth=va_arg(ap,unsigned);uint64_t fifth=va_arg(ap,uint64_t);
 unsigned again=va_arg(copy,unsigned);va_end(copy);va_end(ap);
 return second+fifth+first+again+fourth+(uint64_t)(third*4.0);
}
static unsigned integers(unsigned n,...){va_list ap;va_start(ap,n);unsigned v=0;while(n--)v=v*33U+va_arg(ap,unsigned);va_end(ap);return v;}
static unsigned format(char *buffer,unsigned cap,const char *pattern,...){
 va_list args;va_start(args,pattern);
 const solar_os_native_libc_api_v1_t *lib=solar_os_native_host_v1()->get_service("libc",1,12);
 unsigned n=lib->format(buffer,cap,pattern,args);va_end(args);return n;
}
''', r'''
    STORE64(0,collect(5,x,((uint64_t)y<<32)|x,1.25,(unsigned)17,0x8765432100000001ULL));
    STORE64(2,api->variadic_callback(collect,x,y));
    out[4]=integers(10,x,y,1U,2U,3U,4U,5U,6U,7U,8U);
    char buffer[64];out[5]=format(buffer,sizeof(buffer),"%u:%llu:%.2f",x,((uint64_t)y<<32)|x,1.25);
    for(unsigned i=0;i<20;i++)out[6+i]=(unsigned char)buffer[i%((out[5]<63?out[5]:63)+1)];
    return out[0]^out[2]^out[5];
'''),
        "aggregate_boundaries": (r'''
static diff_quad_t quad(uint32_t x,diff_quad_t value,uint32_t y)
{for(unsigned i=0;i<4;i++)value.words[i]=(value.words[i]+x)^(y+i);return value;}
struct Two {unsigned char a,b;};
static struct Two pair(struct Two value,unsigned x){value.a+=x;value.b^=x;return value;}
''', r'''
    diff_quad_t value={{x,y,x+y,x-y}};value=api->quad(17,value,23);
    for(unsigned i=0;i<4;i++)out[i]=value.words[i];
    out[4]=api->quad_callback(quad,x,y);
    diff_small_t tiny={{(uint8_t)x,(uint8_t)y,0x80}};tiny=api->small(x,tiny,y);
    out[5]=tiny.bytes[0];out[6]=tiny.bytes[1];out[7]=tiny.bytes[2];
    struct Two two={(unsigned char)x,(unsigned char)y};two=pair(two,17);out[8]=two.a;out[9]=two.b;
    return out[0]^out[4]^out[7];
'''),
        "variadic_stack": (r'''
static uint64_t stack(unsigned a,unsigned b,unsigned c,unsigned d,unsigned e,unsigned f,unsigned g,...){
 va_list ap;va_start(ap,g);uint64_t x=va_arg(ap,uint64_t);double y=va_arg(ap,double);
 diff_triple_t triple=va_arg(ap,diff_triple_t);va_end(ap);
 return x+(uint64_t)(y*4.0)+a+b+c+d+e+f+g+triple.a+triple.b+triple.c;
}
''', r'''
    diff_triple_t triple={x,y,x^y};
    STORE64(0,stack(1,2,3,4,5,6,7,((uint64_t)x<<32)|y,1.25,triple));
    return out[0]^out[1];
'''),
    }


def cases(seed, random_cases):
    result = curated()
    result.update(wide_cases())
    result.update(extended_cases())
    rng = random.Random(seed)
    for i in range(random_cases):
        expressions = [expression(rng, 4) for _ in range(12)]
        body = "uint32_t a=x^0xdeadbeefU,b=y+0x89abcdefU;\n"
        body += "\n".join(f"out[{j}]={value};" for j, value in enumerate(expressions))
        body += "\nreturn out[0]^out[5]^out[11];\n"
        result[f"expressions_{i:02d}"] = ("", body)
    return {name: '#include "diff_abi.h"\n#include "solar_os_native_abi.h"\n#include "solar_os_native_time_abi.h"\n#include "solar_os_native_libc_abi.h"\n#include <stdarg.h>\n'
            '#define F32(n,expr) do {union{float f;uint32_t u;}cc_bits;cc_bits.f=(expr);out[n]=cc_bits.u;}while(0)\n'
            '#define F64(n,expr) do {union{double f;uint64_t u;}cc_bits;cc_bits.f=(expr);STORE64(n,cc_bits.u);}while(0)\n'
            '#define STORE64(n,expr) do {uint64_t cc_value=(uint64_t)(expr);out[n]=(uint32_t)cc_value;out[(n)+1]=(uint32_t)(cc_value>>32);}while(0)\n' + helpers +
            '\nuint32_t diff_probe(uint32_t x,uint32_t y,uint32_t *out,const diff_api_t *api) {\n'
            '(void)x; (void)y; (void)out; (void)api; (void)solar_os_native_host_v1();\n' + body + '}\n'
            for name, (helpers, body) in result.items()}


def vectors(seed, count):
    edges = (0, 1, 2, 7, 31, 32, 127, 128, 255, 256, 32767, 32768,
             65535, 65536, 0x7fffffff, 0x80000000, 0x80000001, 0xfffffffe, 0xffffffff)
    pairs = [(value, value) for value in edges]
    pairs += [(value, edges[-1-i]) for i, value in enumerate(edges)]
    pairs += [(value, 0) for value in edges]
    pairs += [(edges[i%len(edges)],i) for i in range(32)]
    rng = random.Random(seed ^ 0x53534f53)
    while len(pairs) < count: pairs.append((rng.getrandbits(32), rng.getrandbits(32)))
    return pairs[:count]
