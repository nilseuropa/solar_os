// Bounded in-memory asset readers. No filesystem calls or borrowed asset data
// survive a command; a failed load leaves scene resources unchanged.
using AssetView=std::string_view;
static bool asset_space(char c) {return c==' '||c=='\t'||c=='\r'||c=='\f';}
static AssetView asset_trim(AssetView v) {
    while(!v.empty()&&asset_space(v.front()))v.remove_prefix(1);
    while(!v.empty()&&asset_space(v.back()))v.remove_suffix(1);
    return v;
}
static AssetView asset_word(AssetView &v) {
    v=asset_trim(v);size_t n=0;while(n<v.size()&&!asset_space(v[n]))++n;
    AssetView word=v.substr(0,n);v.remove_prefix(n);v=asset_trim(v);return word;
}
struct AssetLines {
    AssetView remaining;bool valid=true;
    bool next(AssetView &key,AssetView &rest) {
        while(!remaining.empty()) {
            size_t end=remaining.find('\n');if(end==AssetView::npos)end=remaining.size();
            rest=remaining.substr(0,end);remaining.remove_prefix(end+(end<remaining.size()));
            if(rest.size()>4096) {valid=false;return false;}
            size_t comment=rest.find('#');if(comment!=AssetView::npos)rest=rest.substr(0,comment);
            rest=asset_trim(rest);if(rest.empty())continue;key=asset_word(rest);return true;
        }
        return false;
    }
};
static bool asset_text(const solar_os_jet_command_t &q,size_t limit,AssetView &out) {
    if(!q.data||!q.bytes||q.bytes>limit||std::memchr(q.data,0,q.bytes))return false;
    out={static_cast<const char *>(q.data),q.bytes};
    if(out.size()>=3&&uint8_t(out[0])==0xef&&uint8_t(out[1])==0xbb&&uint8_t(out[2])==0xbf)out.remove_prefix(3);
    return true;
}
static bool asset_number(AssetView token,double &value) {
    if(token.empty()||token.size()>64)return false;
    size_t i=0;bool negative=token[0]=='-';if(token[0]=='+'||negative)++i;
    bool digits=false;value=0;
    while(i<token.size()&&token[i]>='0'&&token[i]<='9') {value=value*10+token[i++]-'0';digits=true;}
    if(i<token.size()&&token[i]=='.') {
        ++i;double fraction=.1;
        while(i<token.size()&&token[i]>='0'&&token[i]<='9') {value+=(token[i++]-'0')*fraction;fraction*=.1;digits=true;}
    }
    if(!digits)return false;
    if(i<token.size()&&(token[i]=='e'||token[i]=='E')) {
        ++i;bool minus=i<token.size()&&token[i]=='-';if(i<token.size()&&(token[i]=='+'||minus))++i;
        unsigned exponent=0;size_t first=i;
        while(i<token.size()&&token[i]>='0'&&token[i]<='9') {
            exponent=exponent*10+token[i++]-'0';if(exponent>38)return false;
        }
        if(i==first)return false;
        while(exponent--)value*=minus ? .1 : 10;
    }
    if(negative)value=-value;
    return i==token.size()&&std::isfinite(value);
}
static int asset_numbers(AssetView rest,double *out,unsigned minimum,unsigned maximum) {
    unsigned count=0;
    while(!rest.empty()) {
        if(count==maximum||!asset_number(asset_word(rest),out[count]))return -1;
        ++count;
    }
    return count>=minimum ? int(count) : -1;
}
static bool asset_name(AssetView view,char *name) {
    if(view.empty()||view.size()>=SOLAR_OS_JET_NAME)return false;
    std::memset(name,0,SOLAR_OS_JET_NAME);std::memcpy(name,view.data(),view.size());return true;
}
template<class T,size_t N> static T *asset_named(std::unique_ptr<T> (&entries)[N],AssetView name) {
    if(name.empty())return nullptr;
    for(auto &entry:entries)if(entry&&std::strlen(entry->name)==name.size()&&!std::memcmp(entry->name,name.data(),name.size()))return entry.get();
    return nullptr;
}
static bool asset_integer(AssetView token,int &result,unsigned base=10) {
    if(token.empty())return false;
    bool negative=token.front()=='-';if(negative||token.front()=='+')token.remove_prefix(1);
    if(base==16&&token.size()>2&&token[0]=='0'&&(token[1]=='x'||token[1]=='X'))token.remove_prefix(2);
    if(token.empty())return false;
    unsigned value=0;
    for(char c:token) {
        unsigned digit=c>='0'&&c<='9' ? c-'0' : c>='a'&&c<='f' ? c-'a'+10 : c>='A'&&c<='F' ? c-'A'+10 : 99;
        if(digit>=base||value>(unsigned(INT32_MAX)-digit)/base)return false;
        value=value*base+digit;
    }
    result=negative ? -int(value) : int(value);return true;
}
static bool asset_index(AssetView token,size_t count,int &result) {
    int index;if(!asset_integer(token,index)||!index)return false;
    result=index>0 ? index-1 : int(count)+index;return result>=0&&size_t(result)<count;
}
static Vector3 asset_normal(const Vector3 &a,const Vector3 &b,const Vector3 &c) {
    int64_t ux=int64_t(b.x)-a.x,uy=int64_t(b.y)-a.y,uz=int64_t(b.z)-a.z;
    int64_t vx=int64_t(c.x)-a.x,vy=int64_t(c.y)-a.y,vz=int64_t(c.z)-a.z;
    int64_t x=uy*vz-uz*vy,y=uz*vx-ux*vz,z=ux*vy-uy*vx;
    double magnitude=std::sqrt(double(x)*x+double(y)*y+double(z)*z);
    return magnitude ? Vector3{int32_t(x*1024/magnitude),int32_t(y*1024/magnitude),int32_t(z*1024/magnitude)} : Vector3{};
}
static int load_obj(const solar_os_jet_command_t &q,solar_os_jet_reply_t &r) {
    AssetView text;
    if(!asset_text(q,1024*1024,text)||!valid_values(q,jet_obj_load_fields,JET_LOAD_COUNT)||object_name_used(q.name))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    double options[SOLAR_OS_JET_VALUES]={0};merge_values(options,q,jet_obj_load_fields,JET_LOAD_COUNT);
    auto item=std::make_unique<Item>(uint32_t(options[JET_LOAD_color]));item->object=std::make_unique<Object>();
    Material *fallback=material(uint32_t(options[JET_LOAD_material]),&item->material),*current=fallback;
    if(!fallback)return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    std::vector<Vector3> positions,normals;std::vector<Vector2> uvs;
    AssetLines lines{text};AssetView key,rest;
    while(lines.next(key,rest)) {
        if(key=="v") {
            double a[4]={0,0,0,1};if(asset_numbers(rest,a,3,4)<0||a[3]==0)return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
            for(unsigned i=0;i<3;++i) {a[i]=a[i]*128*options[JET_LOAD_scale]/a[3];if(!std::isfinite(a[i])||a[i]<-4096||a[i]>4096)return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;}
            if(positions.size()==SOLAR_OS_JET_MAX_VERTICES)return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
            positions.push_back({int32_t(a[0]),int32_t(a[1]),int32_t(a[2])});
        } else if(key=="vt"||key=="vn") {
            double a[3]={0};unsigned count=key=="vn" ? 3 : 2;
            if(asset_numbers(rest,a,count,3)<0)return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
            double limit=key=="vn" ? 1 : 32;
            for(unsigned i=0;i<count;++i)if(a[i]<-limit||a[i]>limit)return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
            if(uvs.size()+normals.size()>=8192)return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
            if(key=="vn")normals.push_back({int32_t(a[0]*1024),int32_t(a[1]*1024),int32_t(a[2]*1024)});
            else uvs.push_back({int32_t(a[0]*1024),int32_t(a[1]*1024)});
        } else if(key=="usemtl") {
            auto *entry=asset_named(active->materials,rest);if(!entry)return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;current=&entry->value;
        } else if(key=="md"||key=="mr")current=fallback;
        else if(key=="mt") {
            int color,alpha=255;if(!asset_integer(asset_word(rest),color,16)||color<0||color>65535)return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
            if(!rest.empty()&&(!asset_integer(asset_word(rest),alpha)||alpha<0||alpha>255))return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
            if(!rest.empty()||item->face_materials.size()>=32)return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
            auto mat=std::make_unique<Material>(uint16_t(color));mat->name=nullptr;mat->alpha=alpha;current=mat.get();item->face_materials.push_back(std::move(mat));
        } else if(key=="f") {
            Object::Vertex face[16];bool supplied[16]={};unsigned count=0;
            while(!rest.empty()) {
                if(count==16)return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
                AssetView token=asset_word(rest),parts[3];unsigned fields=0;
                do {
                    if(fields==3)return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
                    size_t slash=token.find('/');parts[fields++]=token.substr(0,slash);
                    if(slash==AssetView::npos)break;
                    token.remove_prefix(slash+1);
                } while(true);
                int p,uv=-1,n=-1;if(!asset_index(parts[0],positions.size(),p))return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
                if(fields>1&&!parts[1].empty()&&!asset_index(parts[1],uvs.size(),uv))return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
                if(fields>2&&!parts[2].empty()&&!asset_index(parts[2],normals.size(),n))return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
                face[count].position=positions[p];if(uv>=0)face[count].uv=uvs[uv];
                if(n>=0)face[count].normal=normals[n];
                supplied[count]=n>=0;++count;
            }
            if(count<3)return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
            auto &obj=*item->object;
            if(obj.triangles.size()+count-2>SOLAR_OS_JET_MAX_TRIANGLES)return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
            for(unsigned i=1;i+1<count;++i) {
                Vector3 normal=asset_normal(face[0].position,face[i].position,face[i+1].position);unsigned base=obj.vertices.size();
                for(unsigned index:{0U,i,i+1}) {auto v=face[index];if(!supplied[index])v.normal=normal;obj.addVertex(v);}
                obj.addTriangle(base,base+1,base+2,current);
            }
        }
    }
    if(!lines.valid||item->object->triangles.empty())return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    item->object->calculateBoundingBox();std::memcpy(item->name,q.name,sizeof(item->name));return admit(std::move(item),&r.handle);
}
static int load_mtl(const solar_os_jet_command_t &q,solar_os_jet_reply_t &r) {
    AssetView text;if(q.fields||!asset_text(q,256*1024,text))return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    std::vector<std::unique_ptr<MaterialResource>> entries;MaterialResource *current=nullptr;
    AssetLines lines{text};AssetView key,rest;
    while(lines.next(key,rest)) {
        if(key=="newmtl") {
            auto entry=std::make_unique<MaterialResource>();
            if(!asset_name(rest,entry->name)||asset_named(active->materials,rest))return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
            for(auto &other:entries)if(!std::strcmp(other->name,entry->name))return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
            if(entries.size()==SOLAR_OS_JET_MAX_MATERIALS)return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
            solar_os_jet_command_t defaults={};merge_values(entry->values,defaults,jet_material_fields,JET_MAT_COUNT);
            current=entry.get();entries.push_back(std::move(entry));
        } else if(key=="Kd"||key=="Ks"||key=="d"||key=="Tr"||key=="Ns"||key=="illum") {
            if(!current)return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
            double a[3]={0};unsigned count=(key=="Kd"||key=="Ks") ? 3 : 1;
            if(asset_numbers(rest,a,count,count)<0)return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
            double limit=key=="Ns" ? 1000 : key=="illum" ? 10 : 1;
            for(unsigned i=0;i<count;++i)if(a[i]<0||a[i]>limit)return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
            auto *v=current->values;
            if(key=="Kd")v[JET_MAT_color]=(uint32_t(a[0]*255)<<16)|(uint32_t(a[1]*255)<<8)|uint32_t(a[2]*255);
            else if(key=="Ks")v[JET_MAT_specular]=std::floor((a[0]+a[1]+a[2])*255/3);
            else if(key=="d"||key=="Tr")v[JET_MAT_alpha]=std::floor((key=="Tr" ? 1-a[0] : a[0])*255);
            else if(key=="Ns")v[JET_MAT_gloss]=std::floor(std::min(a[0],255.0));
            else {
                if(a[0]!=std::floor(a[0]))return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
                v[JET_MAT_shading]=a[0]==0 ? 4 : a[0]==1 ? 1 : 2;
            }
        } else if(key=="map_Kd") {
            if(!current)return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
            auto *texture=asset_named(active->textures,rest);if(!texture)return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
            current->values[JET_MAT_texture]=texture->id;
        }
    }
    if(!lines.valid||entries.empty())return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    size_t free=0;for(auto &entry:active->materials)free+=!entry;
    if(entries.size()>free)return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    if(entries.size()>UINT32_MAX-active->next_id)return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    // Everything after validation is allocation-free; publish the whole library.
    for(auto &entry:entries) {
        auto *tex=resource(active->textures,uint32_t(entry->values[JET_MAT_texture]));
        material_values(entry->value,entry->values,tex ? tex->value.get() : nullptr);
        entry->id=++active->next_id;r.handles[r.count++]=entry->id;
        for(auto &slot:active->materials)if(!slot) {slot=std::move(entry);break;}
    }
    return 0;
}
static uint32_t asset_u32(const uint8_t *p) {return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
static unsigned asset_u16(const uint8_t *p) {return p[0]|(unsigned(p[1])<<8);}
static int load_bmp(const solar_os_jet_command_t &q,solar_os_jet_reply_t &r) {
    if(!q.data||q.bytes<54||q.bytes>8*1024*1024||!valid_values(q,jet_texture_fields,JET_TEX_COUNT)||
        (q.fields&((1U<<JET_TEX_pixels)|(1U<<JET_TEX_palette)))||duplicate_name(active->textures,q.name,0))return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    double v[SOLAR_OS_JET_VALUES]={0};merge_values(v,q,jet_texture_fields,JET_TEX_COUNT);
    if(v[JET_TEX_format]!=0||v[JET_TEX_tiled])return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    auto *data=static_cast<const uint8_t *>(q.data);size_t offset=asset_u32(data+10),dib=asset_u32(data+14);
    if(data[0]!='B'||data[1]!='M'||dib<40||dib>q.bytes-14||offset<14+dib||offset>q.bytes||asset_u16(data+26)!=1)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    int32_t width=int32_t(asset_u32(data+18)),signed_height=int32_t(asset_u32(data+22));
    if(width<1||width>1024||signed_height==0||signed_height<-1024||signed_height>1024)return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    unsigned height=signed_height<0 ? -signed_height : signed_height,bits=asset_u16(data+28),compression=asset_u32(data+30);
    if(bits!=1&&bits!=4&&bits!=8&&bits!=16&&bits!=24&&bits!=32)return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if(compression!=0&&(compression!=3||(bits!=16&&bits!=32)))return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    size_t stride=(size_t(width)*bits+31)/32*4,required=offset+stride*height;
    uint32_t declared=asset_u32(data+2);if(required>q.bytes||(declared&&(declared>q.bytes||declared<required)))return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    bool green6=false;
    if(compression==3) {
        if((dib==40&&offset<14+dib+12)||(dib>40&&dib<52))return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        uint32_t red=asset_u32(data+54),green=asset_u32(data+58),blue=asset_u32(data+62);
        if(bits==16) {
            green6=red==0xf800&&green==0x07e0&&blue==0x001f;
            if(!green6&&(red!=0x7c00||green!=0x03e0||blue!=0x001f))return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        } else if(red!=0x00ff0000||green!=0x0000ff00||blue!=0x000000ff)return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    unsigned palette_count=0;const uint8_t *palette=data+14+dib;
    if(bits<=8) {
        palette_count=asset_u32(data+46);if(!palette_count)palette_count=1U<<bits;
        if(palette_count>(1U<<bits)||size_t(palette_count)*4>offset-(14+dib))return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    auto entry=std::make_unique<TextureResource>();entry->width=width;entry->height=height;entry->pixels.resize(size_t(width)*height);
    for(unsigned y=0;y<height;++y) {
        const uint8_t *row=data+offset+(signed_height<0 ? y : height-1-y)*stride;
        for(int x=0;x<width;++x) {
            uint16_t color;
            if(bits<=8) {
                unsigned index=bits==8 ? row[x] : bits==4 ? (row[x/2]>>((1-(x%2))*4))&15 : (row[x/8]>>(7-x%8))&1;
                if(index>=palette_count)return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
                auto *p=palette+index*4;color=rgb565(uint32_t(p[2])*65536+unsigned(p[1])*256+p[0]);
            } else if(bits==16) {
                unsigned pixel=asset_u16(row+x*2);
                color=green6 ? pixel : uint16_t(((pixel&0x7c00)<<1)|((pixel&0x03e0)<<1)|((pixel&0x0200)>>4)|(pixel&31));
            } else {
                auto *p=row+size_t(x)*(bits/8);color=rgb565(uint32_t(p[2])*65536+unsigned(p[1])*256+p[0]);
            }
            entry->pixels[size_t(y)*width+x]=color;
        }
    }
    entry->value=std::make_unique<Texture>(width,height,entry->pixels.data());entry->value->name=entry->name;
    entry->value->palette=nullptr;entry->value->paletteSize=0;texture_settings(*entry->value,v);
    std::memcpy(entry->values,v,sizeof(v));return store(active->textures,std::move(entry),q,r);
}
static int asset_command(const solar_os_jet_command_t &q,solar_os_jet_reply_t &r) {
    switch(q.opcode) {
    case SOLAR_OS_JET_LOAD_OBJ:return load_obj(q,r);
    case SOLAR_OS_JET_LOAD_MTL:return load_mtl(q,r);
    case SOLAR_OS_JET_LOAD_BMP:return load_bmp(q,r);
    default:return instance_command(q,r);
    }
}
