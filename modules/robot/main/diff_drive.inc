/* Runs inside the existing left joint worker, under the provider mutex.
 * Stream/parameter adapters stay in the OS frontend. */
static solar_os_robot_model_t *find_model(uint32_t id);

static int drive_pause(drive_t *d)
{
    int result=OK;uint32_t ids[]={d->left,d->right};
    for(unsigned i=0;i<2;++i) {
        joint_t *a=find(ids[i]);
        if(a && a->controller) {
            int e=controllers_api->pause(a->controller);
            if(e!=OK && result==OK) result=e;
            solar_os_native_bound_controller_status_v1_t s;
            if(controllers_api->status_bound(a->controller,&s)==OK) sync_controller(a,&s);
        }
    }
    d->info.left_command=d->info.right_command=0;
    return result;
}
static void drive_fault(drive_t *d,int error)
{
    d->info.state=SOLAR_OS_ROBOT_FAULT;d->info.last_error=error;d->info.odometry_valid=0;
    d->info.left_command=d->info.right_command=0;
    joint_t *left=find(d->left),*right=find(d->right);
    if(left) fault(left,error);
    if(right) fault(right,error);
}

static int drive_open(uint32_t model_id,uint32_t left,uint32_t right,
    const solar_os_robot_diff_drive_io_t *io,void *context,uint32_t *out)
{
    if(!out || !left || left==right) return ARG;
    *out=0;lock();
    solar_os_robot_model_t *m=find_model(model_id);
    solar_os_robot_diff_drive_config_t c;
    int e=m?solar_os_robot_model_diff_drive(m,0,&c):MISSING;
    joint_t *l=find(left),*r=find(right);
    if(e==OK && (!l || !r)) e=MISSING;
    if(e==OK && (l->busy || r->busy || l->drive || r->drive || !l->worker || !r->worker ||
       !l->controller || !r->controller || !equal(l->info.config.name,c.left_joint) ||
       !equal(r->info.config.name,c.right_joint) || l->info.config.kind!=SOLAR_OS_ROBOT_MOTOR_VELOCITY ||
       r->info.config.kind!=SOLAR_OS_ROBOT_MOTOR_VELOCITY)) e=STATE;
    if(e==OK) {
        float velocity=fminf(l->info.config.velocity_limit,r->info.config.velocity_limit);
        if(!c.linear_limit) c.linear_limit=c.wheel_radius*velocity;
        if(!c.angular_limit) c.angular_limit=2*c.wheel_radius*velocity/c.wheel_separation;
        if(!isfinite(c.linear_limit) || c.linear_limit<=0 || !isfinite(c.angular_limit) || c.angular_limit<=0) e=ARG;
    }
    if(e==OK) {
        e=SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
        for(unsigned i=0;i<SOLAR_OS_ROBOT_MODEL_MAX && next_drive_id!=UINT32_MAX;++i) if(!drives[i].id) {
            drive_t *d=&drives[i];memset(d,0,sizeof(*d));
            d->id=++next_drive_id;d->model=model_id;d->left=left;d->right=right;d->info.config=c;
            if(io)d->io=*io;
            d->context=context;
            d->period_ms=l->info.config.period_ms;
            d->feedback_ms=l->info.config.feedback_timeout_ms<r->info.config.feedback_timeout_ms?
                l->info.config.feedback_timeout_ms:r->info.config.feedback_timeout_ms;
            d->info.state=SOLAR_OS_ROBOT_READY;l->drive=r->drive=d->id;*out=d->id;e=OK;break;
        }
    }
    unlock();return e;
}
static int drive_enable(uint32_t id)
{
    lock();drive_t *d=find_drive(id);int e=d?OK:MISSING;
    joint_t *l=d?find(d->left):NULL,*r=d?find(d->right):NULL;
    if(e==OK && (d->enabled || !l || !r || l->busy || r->busy || atomic_load(&l->stop) || atomic_load(&r->stop) ||
       !l->controller || !r->controller || !l->worker || !r->worker ||
       l->info.state==SOLAR_OS_ROBOT_FAULT || r->info.state==SOLAR_OS_ROBOT_FAULT)) e=STATE;
    if(e==OK) { d->enabled=true;d->enabled_us=now();d->info.state=SOLAR_OS_ROBOT_WAITING; }
    unlock();return e;
}
static int drive_command(uint32_t id,float linear,float angular,uint64_t linear_us,uint64_t angular_us)
{
    if(!isfinite(linear) || !isfinite(angular) || !linear_us || !angular_us) return ARG;
    lock();drive_t *d=find_drive(id);int e=d?OK:MISSING;
    uint64_t time=now(),oldest=linear_us<angular_us?linear_us:angular_us;
    if(e==OK && (!d->enabled || d->io.command || d->info.state==SOLAR_OS_ROBOT_FAULT)) e=STATE;
    if(e==OK && (linear_us>time || angular_us>time || fabsf(linear)>d->info.config.linear_limit ||
       fabsf(angular)>d->info.config.angular_limit)) e=ARG;
    if(e==OK && time-oldest>=(uint64_t)d->info.config.command_timeout_ms*1000) e=TIMEOUT;
    if(e==OK) {
        d->info.command_linear=linear;d->info.command_angular=angular;
        d->info.command_us[0]=linear_us;d->info.command_us[1]=angular_us;
    }
    unlock();return e;
}
static int drive_status(uint32_t id,solar_os_robot_diff_drive_status_t *out)
{
    if(!out) return ARG;
    lock();drive_t *d=find_drive(id);if(d)*out=d->info;unlock();return d?OK:MISSING;
}
static int drive_disable(uint32_t id)
{
    lock();drive_t *d=find_drive(id);
    if(d) { d->enabled=false;(void)drive_pause(d);d->info.state=SOLAR_OS_ROBOT_READY; }
    /* Actual stop/release errors are handled by joint close, which follows
     * disable and retains the model/drive lease until cleanup succeeds. */
    unlock();return d?OK:MISSING;
}
static int drive_close(uint32_t id)
{
    lock();drive_t *d=find_drive(id);int e=d?OK:MISSING;
    if(d && (d->enabled || d->left || d->right)) e=STATE;
    if(e==OK) memset(d,0,sizeof(*d));
    unlock();return e;
}

static void drive_cycle(joint_t *a)
{
    drive_t *d=find_drive(a->drive);
    if(!d || !d->enabled || d->left!=a->id || d->info.state==SOLAR_OS_ROBOT_FAULT) return;
    joint_t *l=find(d->left),*r=find(d->right);
    uint64_t time=now();
    if(!l || !r || l->info.state==SOLAR_OS_ROBOT_FAULT || r->info.state==SOLAR_OS_ROBOT_FAULT ||
       !l->controller || !r->controller) { drive_fault(d,STATE);return; }
    if(d->tick_us && time<d->tick_us) { drive_fault(d,TIMEOUT);return; }
    if(d->tick_us && time-d->tick_us<(uint64_t)d->period_ms*1000) return;
    d->tick_us=time;
    if(d->io.command) {
        solar_os_robot_diff_drive_command_t c={0};d->io.command(d->context,&c);
        time=now(); /* A producer may have committed after the tick began. */
        if(!isfinite(c.linear) || !isfinite(c.angular) || c.linear_us>time || c.angular_us>time ||
           fabsf(c.linear)>d->info.config.linear_limit || fabsf(c.angular)>d->info.config.angular_limit) {
            drive_fault(d,ARG);return;
        }
        d->info.command_linear=c.linear;d->info.command_angular=c.angular;
        d->info.command_us[0]=c.linear_us;d->info.command_us[1]=c.angular_us;
    }
    const solar_os_robot_joint_info_v3_t *w[]={&l->info,&r->info};
    bool feedback=true;
    for(unsigned i=0;i<2;++i) if(!w[i]->velocity_valid || !w[i]->sample_us || w[i]->sample_us>time ||
       time-w[i]->sample_us>(uint64_t)d->feedback_ms*1000 || !isfinite(w[i]->position)) feedback=false;
    if(!feedback) {
        if(time-d->enabled_us>(uint64_t)d->feedback_ms*1000) drive_fault(d,TIMEOUT);
        return;
    }
    uint64_t skew=w[0]->sample_us>w[1]->sample_us?w[0]->sample_us-w[1]->sample_us:w[1]->sample_us-w[0]->sample_us;
    if(skew>(uint64_t)d->feedback_ms*1000) { drive_fault(d,TIMEOUT);return; }
    if(!d->seeded || (w[0]->sample_us>d->samples[0] && w[1]->sample_us>d->samples[1])) {
        uint64_t sample=w[0]->sample_us/2+w[1]->sample_us/2;
        if(d->seeded) {
            double travel[2];
            for(unsigned i=0;i<2;++i) {
                /* Joint-relative angles already include encoder scaling,
                 * polarity, gear ratio and counter-wrap handling. Absolute
                 * referencing (position_valid) is unnecessary for deltas. */
                travel[i]=((double)w[i]->position-d->positions[i])*d->info.config.wheel_radius;
            }
            double distance=(travel[0]+travel[1])/2,angle=(travel[1]-travel[0])/d->info.config.wheel_separation;
            double half=angle/2,arc=fabs(half)<1e-9?distance:distance*sin(half)/half;
            double heading=d->info.yaw+half,dt=(double)(sample-d->info.sample_us)/1e6;
            if(dt<=0 || !isfinite(arc) || !isfinite(angle)) { drive_fault(d,ARG);return; }
            d->info.x+=arc*cos(heading);d->info.y+=arc*sin(heading);
            d->info.yaw=atan2(sin(d->info.yaw+angle),cos(d->info.yaw+angle));
            d->info.linear_velocity=(float)(distance/dt);d->info.angular_velocity=(float)(angle/dt);
            if(!isfinite(d->info.x) || !isfinite(d->info.y) || !isfinite(d->info.linear_velocity) ||
               !isfinite(d->info.angular_velocity)) { drive_fault(d,ARG);return; }
        }
        for(unsigned i=0;i<2;++i) { d->positions[i]=w[i]->position;d->samples[i]=w[i]->sample_us; }
        d->info.sample_us=sample;d->info.odometry_valid=1;d->seeded=true;
    }
    uint64_t stamp=d->info.command_us[0]<d->info.command_us[1]?d->info.command_us[0]:d->info.command_us[1];
    if(!stamp || time<stamp || time-stamp>=(uint64_t)d->info.config.command_timeout_ms*1000) {
        if(d->info.state==SOLAR_OS_ROBOT_ACTIVE && drive_pause(d)!=OK) { drive_fault(d,STATE);return; }
        d->info.state=SOLAR_OS_ROBOT_WAITING;d->info.last_error=stamp?TIMEOUT:OK;return;
    }
    double rotation=(double)d->info.command_angular*d->info.config.wheel_separation/2;
    double left=(d->info.command_linear-rotation)/d->info.config.wheel_radius;
    double right=(d->info.command_linear+rotation)/d->info.config.wheel_radius;
    double scale=1;
    if(fabs(left)>l->info.config.velocity_limit) scale=fmin(scale,l->info.config.velocity_limit/fabs(left));
    if(fabs(right)>r->info.config.velocity_limit) scale=fmin(scale,r->info.config.velocity_limit/fabs(right));
    float targets[]={(float)(left*scale),(float)(right*scale)};
    for(unsigned i=0;i<2;++i) {
        if(now()-stamp>=(uint64_t)d->info.config.command_timeout_ms*1000) {
            if(drive_pause(d)!=OK) drive_fault(d,STATE);
            else d->info.state=SOLAR_OS_ROBOT_WAITING;
            return;
        }
        int e=controllers_api->command(i?r->controller:l->controller,targets[i],now());
        if(e!=OK) { drive_fault(d,e);return; }
    }
    if(now()-stamp>=(uint64_t)d->info.config.command_timeout_ms*1000) {
        if(drive_pause(d)!=OK) drive_fault(d,STATE);
        else d->info.state=SOLAR_OS_ROBOT_WAITING;
        return;
    }
    d->info.left_command=targets[0];d->info.right_command=targets[1];d->info.state=SOLAR_OS_ROBOT_ACTIVE;
    d->info.last_error=OK;
}

static void drive_notify(joint_t *a)
{
    drive_t *d=find_drive(a->drive);
    if(!d || !d->enabled)return;
    joint_t *l=find(d->left),*r=find(d->right);
    if(d->info.state!=SOLAR_OS_ROBOT_FAULT && (!l || !r || l->info.state==SOLAR_OS_ROBOT_FAULT || r->info.state==SOLAR_OS_ROBOT_FAULT))
        drive_fault(d,STATE);
    if(d->io.publish)d->io.publish(d->context,&d->info);
}
