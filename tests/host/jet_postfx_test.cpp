#include "PostFX.hpp"
#include "Framebuffer.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>
using namespace Renderer;
int main(int argc,char **argv) {
    assert(argc==2);FILE *out=std::fopen(argv[1],"wb");assert(out);
    constexpr int w=8,h=8,n=w*h,guard=16;constexpr uint16_t sentinel=0xa15c;
    std::vector<uint16_t> storage(n+guard*2,sentinel);auto *frame=storage.data()+guard;
    auto fill=[&](uint16_t color) {std::fill_n(frame,n,framebufferEncode(color));};
    auto record=[&] {
        for(int i=0;i<guard;++i)assert(storage[i]==sentinel&&storage[n+guard+i]==sentinel);
        assert(std::fwrite(frame,2,n,out)==n);
    };
    PostFX fx(w,h);assert(fx.scratchBytes()==0&&!fx.hasHistory());
    fill(0xf800);fx.applyCRT(frame,255,false,false);
    assert(frame[0]==framebufferEncode(0xf800)&&frame[w]==0);record();
    fill(0);for(int y=2;y<6;++y)for(int x=3;x<7;++x)frame[y*w+x]=framebufferEncode(0xffff);
    fx.applyBloom(frame);
    assert(frame[3*w+2]!=0);record();
    fill(0);for(int y=0;y<h;++y)for(int x=4;x<w;++x)frame[y*w+x]=framebufferEncode(0xffff);
    fx.applyFXAA(frame);assert(frame[3*w+3]!=0&&frame[3*w+3]!=0xffff);record();
    fill(0);frame[0]=framebufferEncode(0xf800);frame[1]=framebufferEncode(0x07e0);
    frame[2]=framebufferEncode(0x001f);frame[3]=framebufferEncode(0xffff);fx.applyChromatic(frame,1);
    assert(frame[0]==0&&frame[1]==framebufferEncode(0x07e0)&&frame[2]==framebufferEncode(0xf800));record();
    fill(0);frame[0]=framebufferEncode(0xf800);frame[3]=framebufferEncode(0x001f);
    fx.applyPixelate(frame,3);assert(frame[2*w+2]==framebufferEncode(0xf800)&&frame[2*w+3]==framebufferEncode(0x001f));record();
    fill(0xf800);fx.applyMotionBlur(frame,50);assert(frame[0]==framebufferEncode(0xf800));
    fill(0x001f);fx.applyMotionBlur(frame,50);assert(frame[0]==framebufferEncode(0x780f));record();
    fx.resetHistory();fill(0x07e0);fx.applyMotionBlur(frame,100);assert(frame[0]==framebufferEncode(0x07e0));record();
    assert(fx.scratchBytes()==n*6+w*10);std::fclose(out);
}
