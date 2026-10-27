static int sprite_command(const solar_os_jet_command_t &q,solar_os_jet_reply_t &r) {
    SpriteResource *old=q.opcode==SOLAR_OS_JET_SPRITE ? nullptr : resource(active->sprites,q.handle);
    if(q.opcode!=SOLAR_OS_JET_SPRITE && !old) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if(q.opcode==SOLAR_OS_JET_SPRITE_INFO) {surface_reply(*old,r);return 0;}
    if(!valid_values(q,jet_sprite_fields,JET_SPR_COUNT) || duplicate_name(active->sprites,q.name,q.handle))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    double v[SOLAR_OS_JET_VALUES]={0};merge_values(v,q,jet_sprite_fields,JET_SPR_COUNT,old ? old->values : nullptr);
    auto fresh=old ? nullptr : std::make_unique<SpriteResource>();auto *entry=old ? old : fresh.get();
    Material *mat=material(uint32_t(v[JET_SPR_material]),&entry->fallback);
    if(!mat) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if(mat->shader)return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    entry->fallback.color=rgb565(uint32_t(v[JET_SPR_color]));
    auto &sp=entry->value;sp.material=mat;sp.x=v[JET_SPR_x];sp.y=v[JET_SPR_y];
    sp.width=v[JET_SPR_width];sp.height=v[JET_SPR_height];sp.alpha=v[JET_SPR_alpha];
    sp.zOrder=v[JET_SPR_z_order];sp.enabled=v[JET_SPR_enabled];sp.blendMode=BlendMode(int(v[JET_SPR_blend]));sp.scale=v[JET_SPR_scale];
    sp.textureFlags=(v[JET_SPR_flip_x] ? Sprite2D::FLIP_X : 0)|(v[JET_SPR_flip_y] ? Sprite2D::FLIP_Y : 0)|
        (v[JET_SPR_mirror_x] ? Sprite2D::MIRROR_X : 0)|(v[JET_SPR_mirror_y] ? Sprite2D::MIRROR_Y : 0);
    std::memcpy(entry->values,v,sizeof(v));
    if(q.fields&(1U<<JET_SPR_name))std::memcpy(entry->name,q.name,sizeof(entry->name));
    if(old) {r.handle=old->id;return 0;}
    if(active->next_id==UINT32_MAX)return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    for(auto &slot:active->sprites)if(!slot) {
        active->scene.addSprite(&sp);entry->id=++active->next_id;r.handle=entry->id;slot=std::move(fresh);return 0;
    }
    return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
}
static int remove_sprite(uint32_t id) {
    for(auto &slot:active->sprites)if(slot && slot->id==id) {
        auto &sprites=active->scene.getSprites();sprites.erase(std::remove(sprites.begin(),sprites.end(),&slot->value),sprites.end());
        slot.reset();return 0;
    }
    return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}
static void snapshot_picks() {
    active->pick_count=active->query_count;
    const auto *results=active->scene.getPickResults();
    for(unsigned i=0;i<active->pick_count;++i) {
        const auto &source=results[i];auto &dest=active->picks[i];dest={};
        dest.hit=source.hit;dest.triangle=source.triangleIndex;dest.depth=source.depth;
        dest.x=source.hit ? source.x : active->query_points[i].x;dest.y=source.hit ? source.y : active->query_points[i].y;
        if(source.hit)for(auto &item:active->items)if(item && item->object.get()==source.object) {dest.object=item->id;break;}
        dest.instance=source.instanceIndex;
        if(source.hit) {
            if(source.instanceIndex<0) {
                for(auto &item:active->items)if(item&&item->object.get()==source.mesh) {dest.batch=item->id;break;}
            }
            if(source.instanceIndex>=0)for(auto &owner:active->items)if(owner&&owner->object.get()==source.object) {
                auto matches=[&](const Object *object) {return size_t(source.instanceIndex)<object->instances.size()&&
                    object->instances[size_t(source.instanceIndex)].mesh.get()==source.mesh;};
                if(matches(owner->object.get()))dest.batch=owner->id;
                else for(auto *lod:owner->object->lodMeshes)if(matches(lod)) {
                    for(auto &item:active->items)if(item&&item->object.get()==lod) {dest.batch=item->id;break;}
                    break;
                }
                break;
            }
            for(auto &item:active->items)if(item&&item->object.get()==source.mesh) {dest.mesh=item->id;break;}
            for(auto &mesh:active->prototypes)if(mesh&&mesh->value.get()==source.mesh) {dest.mesh=mesh->id;break;}
            if(!dest.mesh)for(auto &item:active->items)if(item)for(size_t j=0;j<item->object->instances.size();++j)
                if(item->object->instances[j].mesh.get()==source.mesh) {dest.mesh=item->instance_poses[j].prototype;break;}
        }
    }
}
static int overlay_command(const solar_os_jet_command_t &q,solar_os_jet_reply_t &r) {
    switch(q.opcode) {
    case SOLAR_OS_JET_SPRITE:case SOLAR_OS_JET_SPRITE_SET:case SOLAR_OS_JET_SPRITE_INFO:return sprite_command(q,r);
    case SOLAR_OS_JET_PICK: {
        size_t count=q.bytes/sizeof(solar_os_jet_pick_t);
        if(q.bytes%sizeof(solar_os_jet_pick_t) || count>SOLAR_OS_JET_MAX_PICKS || (count && !q.data))
            return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        PickQuery points[SOLAR_OS_JET_MAX_PICKS];
        for(size_t i=0;i<count;++i) {
            solar_os_jet_pick_t p;std::memcpy(&p,static_cast<const uint8_t *>(q.data)+i*sizeof(p),sizeof(p));
            if(p.x<-1 || p.y<-1 || p.x>=active->width || p.y>=active->height) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
            points[i]={p.x,p.y};
        }
        active->scene.setPickQueries(points,count);active->pick_count=0;active->query_count=count;
        for(size_t i=0;i<count;++i)active->query_points[i]={points[i].x,points[i].y};
        return 0;
    }
    case SOLAR_OS_JET_PICK_RESULT: {
        r.count=active->pick_count;
        if(!r.count)return 0;
        double index=q.values[0];if(!std::isfinite(index) || index<0 || index>=r.count || index!=std::floor(index))
            return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        const auto &p=active->picks[unsigned(index)];
        r.handle=p.object;r.values[0]=p.hit;r.values[1]=p.triangle;r.values[2]=p.depth;r.values[3]=p.x;r.values[4]=p.y;
        r.values[5]=p.mesh;r.values[6]=p.instance;r.values[7]=p.batch;return 0;
    }
    default:return asset_command(q,r);
    }
}
