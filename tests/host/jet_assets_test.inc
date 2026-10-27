static void bmp_u32(std::vector<uint8_t> &data,size_t at,uint32_t value) {
    for(unsigned i=0;i<4;++i)data[at+i]=value>>(i*8);
}
static std::vector<uint8_t> asset_bmp(unsigned bits=24,bool top_down=false) {
    unsigned offset=bits<=8 ? 62 : bits==16 ? 66 : 54;
    unsigned stride=bits==24||bits==32 ? 8 : 4;
    std::vector<uint8_t> bmp(offset+stride*2);bmp[0]='B';bmp[1]='M';
    bmp_u32(bmp,2,bmp.size());bmp_u32(bmp,10,offset);bmp_u32(bmp,14,40);bmp_u32(bmp,18,2);bmp_u32(bmp,22,2);
    bmp[26]=1;bmp[28]=bits==555 ? 16 : bits;bmp_u32(bmp,34,stride*2);
    if(bits<=8) {
        bmp_u32(bmp,46,2);bmp[56]=255;bmp[59]=255; // Red and green palette entries.
        bmp[offset]=bits==1 ? 0x40 : bits==4 ? 0x01 : 0;bmp[offset+stride]=bits==1 ? 0x80 : bits==4 ? 0x10 : 1;
        if(bits==8) {bmp[offset+1]=1;bmp[offset+stride+1]=0;}
    } else if(bits==16||bits==555) {
        if(bits==16) {bmp_u32(bmp,30,3);bmp_u32(bmp,54,0xf800);bmp_u32(bmp,58,0x07e0);bmp_u32(bmp,62,0x001f);}
        uint8_t pixels[]={31,0,255,255,0,248,224,7};std::memcpy(bmp.data()+offset,pixels,sizeof(pixels));
        if(bits==555) {
            // BI_RGB 16-bit data uses RGB555, not RGB565.
            bmp_u32(bmp,30,0);uint8_t rgb555[]={31,0,255,127,0,124,224,3};std::memcpy(bmp.data()+offset,rgb555,sizeof(rgb555));
        }
    } else {
        uint8_t rgb[]={255,0,0,255,255,255,0,0,0,0,255,0,255,0,0,0};
        if(bits==24)std::memcpy(bmp.data()+offset,rgb,sizeof(rgb));
        else {
            uint8_t rgba[]={255,0,0,0,255,255,255,0,0,0,255,0,0,255,0,0};
            std::memcpy(bmp.data()+offset,rgba,sizeof(rgba));
        }
    }
    if(top_down) {bmp_u32(bmp,22,uint32_t(-2));for(unsigned i=0;i<stride;++i)std::swap(bmp[offset+i],bmp[offset+stride+i]);}
    return bmp;
}
static int asset_chain(const solar_os_jet_api_v1_t *api,void *scene,uint16_t *color,size_t bytes) try {
    solar_os_jet_command_t q={};solar_os_jet_reply_t r;
    auto execute=[&] {return api->extensions->execute(scene,&q,&r);};
    auto bmp=asset_bmp();q.opcode=SOLAR_OS_JET_LOAD_BMP;q.data=bmp.data();q.bytes=bmp.size();std::strcpy(q.name,"check");
    int err=execute();if(err)return err;uint32_t texture=r.handle;
    const char mtl[]="newmtl surface\nKd .2 .4 .8\nKs 1 1 1\nNs 32\nd .75\nmap_Kd check\nnewmtl second\nKd 1 0 0\n";
    q={};q.opcode=SOLAR_OS_JET_LOAD_MTL;q.data=mtl;q.bytes=sizeof(mtl)-1;
    err=execute();if(err)return err;assert(r.count==2);uint32_t mat=r.handles[0],second=r.handles[1];
    const char obj[]="v -1 -1 0\nv 0 1 0\nv 1 -1 0\nvt 0 0\nvt .5 1\nvt 1 0\nvn 0 0 -1\nusemtl surface\nf 1/1/1 2/2/1 3/3/1\nmt f800 127\nf -3 -2 -1\nmd\nf 1 2 3\n";
    q={};q.opcode=SOLAR_OS_JET_LOAD_OBJ;q.data=obj;q.bytes=sizeof(obj)-1;
    err=execute();if(err)return err;uint32_t object=r.handle;
    q={};q.opcode=SOLAR_OS_JET_OBJECT_ACTION;q.handle=object;q.fields=1U<<JET_OACT_action;q.values[JET_OACT_action]=7;
    err=execute();if(err)return err;uint32_t clone=r.handle;
    assert(api->remove(scene,object)==0&&api->remove(scene,mat)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    err=api->render(scene,color,bytes,nullptr,nullptr);if(err)return err;
    assert(api->remove(scene,clone)==0&&api->remove(scene,mat)==0&&api->remove(scene,second)==0&&api->remove(scene,texture)==0);
    return 0;
} catch(const std::bad_alloc &) {
    return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
}
static void asset_validation(const solar_os_jet_api_v1_t *api,void *scene,uint16_t *color,size_t bytes) {
    solar_os_jet_command_t q={};solar_os_jet_reply_t r;
    auto execute=[&] {return api->extensions->execute(scene,&q,&r);};
    for(unsigned bits:{1U,4U,8U,16U,24U,32U,555U})for(bool top_down:{false,true}) {
        auto bmp=asset_bmp(bits,top_down);q={};q.opcode=SOLAR_OS_JET_LOAD_BMP;q.data=bmp.data();
        long before=live;
        for(size_t n=0;n<bmp.size();++n) {q.bytes=n;assert(execute()!=0&&live==before);}
        q.bytes=bmp.size();assert(execute()==0);uint32_t texture=r.handle;
        std::fill(bmp.begin(),bmp.end(),0); // The provider owns the decoded pixels.
        q={};q.opcode=SOLAR_OS_JET_MATERIAL;q.fields=1U<<JET_MAT_texture;q.values[JET_MAT_texture]=texture;
        assert(execute()==0);uint32_t mat=r.handle;
        q={};q.opcode=SOLAR_OS_JET_SPRITE;q.fields=1U<<JET_SPR_material;q.values[JET_SPR_material]=mat;
        assert(execute()==0);uint32_t sprite=r.handle;
        assert(api->render(scene,color,bytes,nullptr,nullptr)==0);
        const uint8_t *pixels=reinterpret_cast<uint8_t *>(color);
        assert(pixels[0]==(bits<=8 ? 0x07 : 0xf8)&&pixels[1]==(bits<=8 ? 0xe0 : 0));
        assert(api->remove(scene,sprite)==0&&api->remove(scene,mat)==0&&api->remove(scene,texture)==0);
    }
    auto bmp=asset_bmp();
    for(unsigned byte=0;byte<54;++byte)for(unsigned value:{0U,1U,255U}) {
        auto changed=bmp;changed[byte]=value;q={};q.opcode=SOLAR_OS_JET_LOAD_BMP;q.data=changed.data();q.bytes=changed.size();
        long before=live;int err=execute();if(!err)assert(api->remove(scene,r.handle)==0);assert(live==before);
    }
    const char malformed[]="newmtl transient\nKd 1 0 0\nnewmtl broken\nKd nan 0 0\n";
    q={};q.opcode=SOLAR_OS_JET_LOAD_MTL;q.data=malformed;q.bytes=sizeof(malformed)-1;
    assert(execute()==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    q={};q.opcode=SOLAR_OS_JET_MATERIAL;std::strcpy(q.name,"transient");assert(execute()==0&&api->remove(scene,r.handle)==0);
    const char *bad[]={"v 0 0 0\nf 0 1 2\n","v nan 0 0\n","v 1e38 0 0\n","v 0 0 0 0\n", "v 0 0 0\nf 1///2 1 1\n", "usemtl missing\n"};
    for(auto text:bad) {
        q={};q.opcode=SOLAR_OS_JET_LOAD_OBJ;q.data=text;q.bytes=std::strlen(text);long before=live;
        assert(execute()!=0&&live==before);
    }
    uint32_t seed=1;
    for(unsigned sample=0;sample<256;++sample) {
        char text[256];const char alphabet[]="vftn /123-.e#\n";
        for(char &c:text) {seed=seed*1664525+1013904223;c=alphabet[seed%(sizeof(alphabet)-1)];}
        q={};q.opcode=SOLAR_OS_JET_LOAD_OBJ;q.data=text;q.bytes=sizeof(text);long before=live;
        int err=execute();if(!err)assert(api->remove(scene,r.handle)==0);assert(live==before);
    }
    std::vector<uint8_t> indices(33*35);indices.back()=1;uint32_t palette[]={0xff0000,0x00ff00};
    q={};q.opcode=SOLAR_OS_JET_TEXTURE;q.data=indices.data();q.bytes=indices.size();q.extra=palette;q.extra_bytes=sizeof(palette);
    q.values[16]=33;q.values[17]=35;q.fields=(1U<<JET_TEX_format)|(1U<<JET_TEX_tiled)|(1U<<JET_TEX_address);
    q.values[JET_TEX_format]=2;q.values[JET_TEX_tiled]=1;q.values[JET_TEX_address]=1;assert(execute()==0);uint32_t tiled=r.handle;
    q={};q.opcode=SOLAR_OS_JET_SAMPLE_TEXTURE;q.handle=tiled;q.values[0]=q.values[1]=1;
    assert(execute()==0&&r.values[0]==0x00ff00);assert(api->remove(scene,tiled)==0);
}
