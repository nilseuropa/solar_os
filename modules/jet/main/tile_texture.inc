static void tile_palette(TileData &tiles,const TextureResource &entry,const double *values) {
    for(size_t i=0;i<tiles.palette.size();++i)tiles.palette[i]=entry.palette[(i+unsigned(values[JET_TEX_offset]))%entry.palette.size()];
}
static std::unique_ptr<TileData> prepare_tiles(const TextureResource &entry,const double *values) {
    if(!values[JET_TEX_tiled])return nullptr;
    auto tiles=std::make_unique<TileData>();auto &view=tiles->view;
    view.width=entry.width;view.height=entry.height;view.pagesX=(entry.width+31)/32;view.direct=true;
    tiles->palette.resize(entry.palette.size());tile_palette(*tiles,entry,values);view.palette=tiles->palette.data();
    auto *indices=reinterpret_cast<const uint8_t *>(entry.pixels.data());unsigned level=values[JET_TEX_mip_level];
    if(level) {
        unsigned scale=1U<<level;view.coarseOnly=true;view.coarseWidth=(entry.width+scale-1)/scale;view.coarseHeight=(entry.height+scale-1)/scale;
        tiles->coarse.resize(size_t(view.coarseWidth)*view.coarseHeight);
        for(unsigned y=0;y<view.coarseHeight;++y)for(unsigned x=0;x<view.coarseWidth;++x) {
            unsigned sx=std::min(x*scale+scale/2,entry.width-1),sy=std::min(y*scale+scale/2,entry.height-1);
            tiles->coarse[size_t(y)*view.coarseWidth+x]=indices[size_t(sy)*entry.width+sx];
        }
        view.coarse=tiles->coarse.data();
    } else {
        tiles->pages.resize(size_t(view.pagesX)*((entry.height+31)/32)*1024);
        for(unsigned y=0;y<entry.height;++y)for(unsigned x=0;x<entry.width;++x)
            tiles->pages[size_t((y>>5)*view.pagesX+(x>>5))*1024+((y&31)<<5)+(x&31)]=indices[size_t(y)*entry.width+x];
        view.backing=tiles->pages.data();
    }
    if(values[JET_TEX_filter]==3) {
        tiles->filtered.resize(1024*1024);
        for(unsigned y=0;y<1024;++y)for(unsigned x=0;x<1024;++x)
            tiles->filtered[size_t(y)*1024+x]=view.sampleBilinear(x,y);
        view.filteredUV=tiles->filtered.data();
    }
    return tiles;
}
static void bind_tiles(Texture &texture,const TileData *tiles,const double *values) {
    texture.tiled=tiles ? &tiles->view : nullptr;
    if(tiles) {
        unsigned filter=values[JET_TEX_filter];texture.bilinear=filter!=0;
        texture.tiledFilter=filter==2 ? TileFilter::ThreePoint : TileFilter::Bilinear;
    }
}
