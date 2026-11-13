"""Defined-input scalar32 cases shared verbatim by GCC and both TinyCC hosts."""
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


def cases(seed, random_cases):
    result = curated()
    rng = random.Random(seed)
    for i in range(random_cases):
        expressions = [expression(rng, 4) for _ in range(12)]
        body = "uint32_t a=x^0xdeadbeefU,b=y+0x89abcdefU;\n"
        body += "\n".join(f"out[{j}]={value};" for j, value in enumerate(expressions))
        body += "\nreturn out[0]^out[5]^out[11];\n"
        result[f"expressions_{i:02d}"] = ("", body)
    return {name: '#include "diff_abi.h"\n#include "solar_os_native_abi.h"\n' + helpers +
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
