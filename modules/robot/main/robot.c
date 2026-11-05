#include "solar_os_robot_abi.h"
#include "solar_os_native_time_abi.h"
#include "solar_os_native_thread_abi.h"
#include "solar_os_native_controllers_abi.h"
#include "model.h"
#include "joint_config.h"
#include <math.h>
#include <stdatomic.h>
#include <string.h>

#define OK SOLAR_OS_NATIVE_OK
#define ARG SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT
#define STATE SOLAR_OS_NATIVE_ERROR_INVALID_STATE
#define MISSING SOLAR_OS_NATIVE_ERROR_NOT_FOUND
#define TIMEOUT SOLAR_OS_NATIVE_ERROR_TIMEOUT
#define UNSUPPORTED SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED
#define PI 3.14159265358979323846

typedef struct {
    uint32_t id, controller;
    solar_os_robot_joint_info_v3_t info;
    bool servo_open, busy, ppr_configured, ratio_configured;
    atomic_bool stop;
    void *worker;
    uint64_t last_step_us;
    char servo_name[16];
} joint_t;

static const solar_os_native_memory_api_v1_t *memory;
static const solar_os_native_time_api_v1_t *clock_api;
static const solar_os_native_thread_api_v1_t *threads;
static const solar_os_native_motion_api_v1_t *motion;
static const solar_os_native_controllers_api_v1_t *controllers_api;
static void *mutex;
static joint_t *joints[SOLAR_OS_ROBOT_JOINT_MAX];
static uint32_t next_id;
static struct { uint32_t id; solar_os_robot_model_t *object; } models[SOLAR_OS_ROBOT_MODEL_MAX];
static uint32_t next_model_id;

static bool equal(const char *a, const char *b)
{ if (!a || !b) return false; while (*a && *a == *b) { a++; b++; } return *a == *b; }
static bool text(const char *s, size_t max)
{ if (!s || !*s) return false; for (size_t i = 0; i < max; i++) if (!s[i]) return true; return false; }
static void copy(char *out, const char *in, size_t size)
{ size_t i = 0; for (; i + 1 < size && in[i]; i++) out[i] = in[i]; out[i] = 0; }
static uint64_t now(void) { return (uint64_t)clock_api->now_us(); }
static void lock(void) { threads->mutex_lock(mutex); }
static void unlock(void) { threads->mutex_unlock(mutex); }
static joint_t *find(uint32_t id)
{ for (unsigned i = 0; i < SOLAR_OS_ROBOT_JOINT_MAX; i++) if (joints[i] && joints[i]->id == id) return joints[i]; return NULL; }
static bool claims(const joint_t *a) { return a->servo_open || a->controller; }
static solar_os_native_bound_controller_config_v1_t controller_config(const joint_t *a)
{
    const solar_os_robot_joint_config_v3_t *c=&a->info.config;
    solar_os_native_bound_controller_config_v1_t b={
        .pid={.kp=c->kp,.ki=c->ki,.kd=c->kd,.derivative_tau=c->derivative_tau,
            .output_min=-c->max_drive,.output_max=c->max_drive,.max_dt=c->max_gap_ms/1000.0f,
            .period_ms=c->period_ms,.timeout_ms=c->feedback_timeout_ms,
            .setpoint_min=-c->velocity_limit,.setpoint_max=c->velocity_limit},
        .encoder_config={.ppr=c->ppr,.gear_ratio=c->gear_ratio,.polarity=c->encoder_polarity,
            .encoder_on_output=c->encoder_on_output,.velocity_tau=c->velocity_tau,.position_zero=c->position_zero},
        .motor_config={.polarity=c->motor_polarity,.max_drive=c->max_drive,.brake=c->brake},
        .acceleration_limit=c->acceleration_limit,.velocity_deadband=c->velocity_deadband,
        .friction_bias_forward=c->friction_bias_forward,.friction_bias_reverse=c->friction_bias_reverse,
        .command_timeout_ms=c->command_timeout_ms};
    copy(b.motor,c->motor,sizeof(b.motor));copy(b.encoder,c->encoder,sizeof(b.encoder));return b;
}
static void sync_controller(joint_t *a,const solar_os_native_bound_controller_status_v1_t *s)
{
    a->info.claimed=s->claimed;a->info.command_valid=s->command_valid;
    a->info.command_expires_us=s->command_expires_us;
    a->info.target=s->target;a->info.setpoint=s->setpoint;
    a->info.output=s->output*a->info.config.motor_polarity;a->info.feedforward=s->feedforward;
    a->info.p=s->p;a->info.i=s->i;a->info.d=s->d;a->info.saturated=s->saturated;
    a->info.raw_count=s->feedback.count;a->info.decoding=s->feedback.decoding;
    a->info.sample_us=s->feedback.timestamp_us;a->info.position=s->feedback.position;
    a->info.velocity=s->feedback.velocity;a->info.velocity_valid=s->feedback.valid && !s->faulted;
    a->info.encoder_counts_per_rev=s->feedback.counts_per_rev;
    a->info.steps=s->steps;a->info.faults=s->faults;a->info.last_error=s->last_error;
    a->info.state=s->faulted?SOLAR_OS_ROBOT_FAULT:s->waiting?SOLAR_OS_ROBOT_WAITING:SOLAR_OS_ROBOT_ACTIVE;
}
static bool valid(const joint_t *a)
{ return solar_os_robot_config_valid(&a->info.config, a->ppr_configured, a->ratio_configured); }

/* Called with the provider mutex held. Cleanup is output-first: a failed stop
 * pins both devices and preserves the feedback reference for retry. */
static int cleanup(joint_t *a)
{
    int err=OK;
    if(a->controller) {
        err=controllers_api->close(a->controller);
        if(err==OK) { a->controller=0;a->info.output=a->info.feedforward=0; }
    }
    if(err==OK && a->servo_open) {
        err=motion->servo_close(a->servo_name,a->servo_name);
        if(err==OK) a->servo_open=false;
    }
    a->info.claimed=claims(a);return err;
}
static void fault(joint_t *a,int error)
{
    atomic_store(&a->stop,true);
    a->info.last_error=error;
    if(a->controller) {
        (void)controllers_api->fault(a->controller,error);
        solar_os_native_bound_controller_status_v1_t s;
        if(controllers_api->status_bound(a->controller,&s)==OK) sync_controller(a,&s);
    } else {
        a->info.faults++;
        int e=a->servo_open?motion->servo_disable(a->servo_name,a->servo_name):OK;
        a->info.last_error=e!=OK?e:error;
    }
    a->info.state=SOLAR_OS_ROBOT_FAULT;a->info.velocity_valid=a->info.position_valid=0;
}
static void expire_command(joint_t *a)
{
    int err=a->servo_open?motion->servo_disable(a->servo_name,a->servo_name):OK;
    if(err!=OK) { fault(a,err);return; }
    a->info.command_valid=0;a->info.command_expires_us=0;
    a->info.target=a->info.setpoint=a->info.output=0;
    a->info.state=SOLAR_OS_ROBOT_WAITING;a->info.last_error=TIMEOUT;a->last_step_us=now();
}
static void cycle(joint_t *a)
{
    const solar_os_robot_joint_config_v3_t *c=&a->info.config;
    if(c->kind==SOLAR_OS_ROBOT_MOTOR_VELOCITY) {
        solar_os_native_bound_controller_status_v1_t s={0};
        int err=controllers_api->step_bound(a->controller,&s);
        sync_controller(a,&s);
        if(err!=OK || !s.enabled || s.faulted) fault(a,err!=OK?err:STATE);
        return;
    }
    uint64_t time=now();
    if(time<a->last_step_us || time-a->last_step_us>(uint64_t)c->max_gap_ms*1000) { fault(a,TIMEOUT);return; }
    if(time-a->last_step_us<(uint64_t)c->period_ms*1000) return;
    if(a->info.command_valid && time>=a->info.command_expires_us) { expire_command(a);return; }
    if(!a->info.command_valid) { a->last_step_us=time;a->info.steps++;return; }
    float shaft=c->motor_polarity*(a->info.target-c->position_zero)*c->gear_ratio;
    if(!isfinite(shaft) || shaft<c->servo.angle_min || shaft>c->servo.angle_max) { fault(a,ARG);return; }
    int err=motion->servo_set(a->servo_name,a->servo_name,shaft);
    if(err!=OK) { fault(a,err);return; }
    a->info.commanded_position=a->info.target;a->info.output=shaft;
    a->info.steps++;a->last_step_us=time;
}

static void worker(void *user)
{
    joint_t *a = user;
    while (!atomic_load(&a->stop)) {
        clock_api->sleep_ms(5);
        lock();
        if (!atomic_load(&a->stop) && (a->info.state == SOLAR_OS_ROBOT_ACTIVE ||
            a->info.state == SOLAR_OS_ROBOT_WAITING)) cycle(a);
        unlock();
    }
}

static int create(const char *name, uint32_t kind, uint32_t *out)
{
    if (out == NULL || !text(name, SOLAR_OS_ROBOT_JOINT_NAME_MAX) ||
        (kind != SOLAR_OS_ROBOT_SERVO_POSITION && kind != SOLAR_OS_ROBOT_MOTOR_VELOCITY)) return ARG;
    *out = 0;
    /* Initialization occurs on the module loader's serialized entry path. */
    if (!mutex || !motion) return UNSUPPORTED;
    lock();
    unsigned free_slot = SOLAR_OS_ROBOT_JOINT_MAX;
    for (unsigned i = 0; i < SOLAR_OS_ROBOT_JOINT_MAX; i++) {
        if (joints[i] && equal(joints[i]->info.config.name, name)) { unlock(); return STATE; }
        if (!joints[i] && free_slot == SOLAR_OS_ROBOT_JOINT_MAX) free_slot = i;
    }
    if (free_slot == SOLAR_OS_ROBOT_JOINT_MAX || next_id == UINT32_MAX) { unlock(); return SOLAR_OS_NATIVE_ERROR_NO_MEMORY; }
    joint_t *a = memory->alloc(sizeof(*a));
    if (!a) { unlock(); return SOLAR_OS_NATIVE_ERROR_NO_MEMORY; }
    memset(a, 0, sizeof(*a)); atomic_init(&a->stop, true);
    a->id = ++next_id; copy(a->info.config.name, name, sizeof(a->info.config.name));
    a->servo_name[0] = 'r'; a->servo_name[1] = 'a';
    const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 8; i++) a->servo_name[2 + i] = hex[(a->id >> ((7 - i) * 4)) & 15];
    solar_os_robot_joint_config_v3_t *c = &a->info.config;
    solar_os_robot_config_defaults(c, kind);
    copy(c->name, name, sizeof(c->name));
    joints[free_slot] = a; *out = a->id; unlock(); return OK;
}

static int configure(uint32_t id, const char *key, float value)
{
    if (!key || !isfinite(value)) return ARG;
    lock(); joint_t *a = find(id);
    if (!a) { unlock(); return MISSING; }
    if (a->busy || a->worker || claims(a)) { unlock(); return STATE; }
    solar_os_robot_joint_config_v3_t saved = a->info.config, *c = &a->info.config;
    int ret = solar_os_robot_config_set(c, key, value);
    if (!valid(a)) ret = ARG;
    if (ret != OK) *c = saved;
    else {
        a->info.encoder_counts_per_rev=0;
        if (equal(key, "ppr")) a->ppr_configured = true;
        if (equal(key, "gear_ratio")) a->ratio_configured = true;
    }
    if (ret == OK) a->info.command_valid = 0;
    unlock(); return ret;
}
static int bind_motor(uint32_t id, const char *motor, const char *encoder)
{
    if (!text(motor, 20) || !text(encoder, 20)) return ARG;
    lock(); joint_t *a = find(id); int ret = a ? OK : MISSING;
    if (a && (a->busy || a->worker || claims(a) || a->info.config.kind != SOLAR_OS_ROBOT_MOTOR_VELOCITY)) ret = STATE;
    if (ret == OK) {
        copy(a->info.config.motor, motor, 20); copy(a->info.config.encoder, encoder, 20);
        a->info.decoding = 0; a->info.sample_us = 0;
        a->info.velocity_valid = a->info.position_valid = 0;
    }
    unlock(); return ret;
}
static int bind_servo(uint32_t id, const solar_os_native_servo_config_v1_t *c)
{
    if (!c || !text(c->provider, sizeof(c->provider)) || !isfinite(c->angle_min) || !isfinite(c->angle_max) ||
        c->angle_min >= c->angle_max || !c->frequency_hz || c->frequency_hz > 1000 ||
        !c->pulse_min_us || c->pulse_min_us >= c->pulse_max_us || (uint64_t)c->pulse_max_us * c->frequency_hz >= 1000000) return ARG;
    lock(); joint_t *a = find(id); int ret = a ? OK : MISSING;
    if (a && (a->busy || a->worker || claims(a) || a->info.config.kind != SOLAR_OS_ROBOT_SERVO_POSITION)) ret = STATE;
    if (ret == OK) a->info.config.servo = *c;
    unlock(); return ret;
}
static int command(uint32_t id, float target)
{
    if (!isfinite(target)) return ARG;
    lock(); joint_t *a = find(id); int ret = a ? OK : MISSING;
    if (a) {
        const solar_os_robot_joint_config_v3_t *c = &a->info.config;
        if (a->busy || a->info.state == SOLAR_OS_ROBOT_FAULT) ret = STATE;
        else if (c->kind == SOLAR_OS_ROBOT_MOTOR_VELOCITY ? fabsf(target) > c->velocity_limit :
                 target < c->position_min || target > c->position_max) ret = ARG;
        else if (c->kind == SOLAR_OS_ROBOT_SERVO_POSITION &&
            (c->motor_polarity * (target - c->position_zero) * c->gear_ratio < c->servo.angle_min ||
             c->motor_polarity * (target - c->position_zero) * c->gear_ratio > c->servo.angle_max)) ret = ARG;
        if(ret==OK && a->controller) {
            ret=controllers_api->command(a->controller,target,now());
            solar_os_native_bound_controller_status_v1_t s;
            if(ret==OK) ret=controllers_api->status_bound(a->controller,&s);
            if(ret==OK) sync_controller(a,&s);
            unlock();return ret;
        }
        if (ret == OK) {
            if (c->kind == SOLAR_OS_ROBOT_MOTOR_VELOCITY && fabsf(target) <= c->velocity_deadband) target = 0;
            a->info.target = target; a->info.command_valid = 1;
            a->info.command_expires_us = now() + (uint64_t)c->command_timeout_ms * 1000;
            if (a->worker && a->info.state == SOLAR_OS_ROBOT_WAITING) a->info.state = SOLAR_OS_ROBOT_ACTIVE;
            a->info.last_error = OK;
        }
    }
    unlock(); return ret;
}
static int enable(uint32_t id)
{
    lock(); joint_t *a = find(id);
    if (!a) { unlock(); return MISSING; }
    if (a->busy || a->worker || claims(a) || a->info.state != SOLAR_OS_ROBOT_READY) { unlock(); return STATE; }
    const solar_os_robot_joint_config_v3_t *c = &a->info.config;
    int err = valid(a) ? OK : ARG;
    a->info.velocity_valid = a->info.position_valid = a->info.referenced = 0;
    if (err == OK && c->kind == SOLAR_OS_ROBOT_MOTOR_VELOCITY) {
        if (!a->ppr_configured || !a->ratio_configured) err = STATE;
        else if ((motion->capabilities & (SOLAR_OS_NATIVE_MOTION_MOTOR | SOLAR_OS_NATIVE_MOTION_ENCODER)) !=
            (SOLAR_OS_NATIVE_MOTION_MOTOR | SOLAR_OS_NATIVE_MOTION_ENCODER)) err = UNSUPPORTED;
        else if(!controllers_api) err=UNSUPPORTED;
        else {
            solar_os_native_bound_controller_config_v1_t binding=controller_config(a);
            err=controllers_api->open_bound(c->name,"robot",&binding,&a->controller);
            if(err==OK) {
                bool commanded=a->info.command_valid && now()<a->info.command_expires_us;
                float target=a->info.target;
                if(commanded) err=controllers_api->command(a->controller,target,
                    a->info.command_expires_us-(uint64_t)c->command_timeout_ms*1000);
                solar_os_native_bound_controller_status_v1_t s;
                if(err==OK) err=controllers_api->status_bound(a->controller,&s);
                if(err==OK) sync_controller(a,&s);
            }
        }
    } else if (err == OK) {
        if (!(motion->capabilities & SOLAR_OS_NATIVE_MOTION_SERVO)) err = UNSUPPORTED;
        else if (!a->info.command_valid || now() >= a->info.command_expires_us) err = STATE;
        else { err = motion->servo_open(a->servo_name, a->servo_name, &c->servo); if (err == OK) a->servo_open = true; }
    }
    if (err == OK) {
        a->info.state = a->info.command_valid ? SOLAR_OS_ROBOT_ACTIVE : SOLAR_OS_ROBOT_WAITING;
        a->info.last_error = OK; a->last_step_us = now(); atomic_store(&a->stop, false);
        if (threads->worker_start(&a->worker, worker, a, 4096, 0) != 0) err = SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    }
    if (err != OK) {
        atomic_store(&a->stop, true); int cleanup_error = cleanup(a);
        a->info.state = claims(a) ? SOLAR_OS_ROBOT_FAULT : SOLAR_OS_ROBOT_READY;
        a->info.last_error = cleanup_error != OK ? cleanup_error : err;
        if(a->controller) (void)controllers_api->fault(a->controller,a->info.last_error);
    }
    a->info.claimed = claims(a); unlock(); return err;
}
static int disable(uint32_t id)
{
    lock(); joint_t *a = find(id);
    if (!a) { unlock(); return MISSING; }
    if (a->busy) { unlock(); return STATE; }
    a->busy = true; atomic_store(&a->stop, true);
    void *w = a->worker; unlock();
    int joined = w ? threads->worker_join(w) : 0;
    lock();
    int err = joined ? STATE : cleanup(a);
    if (!joined) a->worker = NULL;
    a->busy = false; a->info.command_valid = 0; a->info.velocity_valid = 0;
    a->info.last_error = err;
    a->info.state = err == OK ? SOLAR_OS_ROBOT_READY : SOLAR_OS_ROBOT_FAULT;
    if(err!=OK && a->controller) (void)controllers_api->fault(a->controller,err);
    unlock(); return err;
}
static int status(uint32_t id, solar_os_robot_joint_info_v3_t *out)
{
    if (!out) return ARG;
    lock(); joint_t *a = find(id);
    if (a) {
        *out=a->info;
        solar_os_native_bound_controller_config_v1_t binding=controller_config(a);
        out->encoder_counts_per_rev=(float)solar_os_native_encoder_counts_per_rev(&binding.encoder_config,a->info.decoding);
    }
    unlock(); return a ? OK : MISSING;
}
static int close_joint(uint32_t id)
{
    int err = disable(id); if (err != OK) return err;
    lock(); joint_t *a = find(id);
    if (!a) { unlock(); return MISSING; }
    if (a->worker || a->busy || claims(a)) { unlock(); return STATE; }
    for (unsigned i = 0; i < SOLAR_OS_ROBOT_JOINT_MAX; i++) if (joints[i] == a) joints[i] = NULL;
    memory->free(a); unlock(); return OK;
}
static solar_os_robot_model_t *find_model(uint32_t id)
{
    for (unsigned i = 0; i < SOLAR_OS_ROBOT_MODEL_MAX; ++i) if (models[i].id == id) return models[i].object;
    return NULL;
}
static int model_load(const char *xml, size_t bytes, uint32_t *id, char *error, size_t error_bytes)
{
    if (!id) return ARG;
    *id = 0;
    solar_os_robot_model_t *m = NULL;
    /* Parsing does not hold the worker mutex: existing motor feedback loops
     * must continue while another client loads a description. */
    int e = solar_os_robot_model_parse(memory, xml, bytes, &m, error, error_bytes);
    if (e) return e;
    lock();
    for (unsigned i = 0; i < SOLAR_OS_ROBOT_MODEL_MAX; ++i) if (!models[i].object && next_model_id != UINT32_MAX) {
        models[i].object = m; models[i].id = *id = ++next_model_id;
        unlock(); return OK;
    }
    unlock(); solar_os_robot_model_destroy(m); return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
}
static int model_close(uint32_t id)
{
    lock();
    for (unsigned i = 0; i < SOLAR_OS_ROBOT_MODEL_MAX; ++i) if (id && models[i].id == id) {
        solar_os_robot_model_t *m = models[i].object;
        models[i].id = 0; models[i].object = NULL;
        unlock(); solar_os_robot_model_destroy(m); return OK;
    }
    unlock(); return MISSING;
}
#define MODEL_CALL(expression) do { \
    lock(); solar_os_robot_model_t *m = find_model(id); \
    int e = m ? (expression) : MISSING; unlock(); return e; \
} while (0)
static int model_info(uint32_t id, solar_os_robot_model_info_t *out)
{ MODEL_CALL(solar_os_robot_model_info(m, out)); }
static int model_link(uint32_t id, uint32_t index, char *out, size_t bytes)
{ MODEL_CALL(solar_os_robot_model_link(m, index, out, bytes)); }
static int model_joint(uint32_t id, uint32_t index, solar_os_robot_model_joint_info_t *out)
{ MODEL_CALL(solar_os_robot_model_joint(m, index, out)); }
static int model_set_positions(uint32_t id, const char *const *names, const float *positions, uint32_t count)
{ MODEL_CALL(solar_os_robot_model_set_positions(m, names, positions, count)); }
static int model_lookup(uint32_t id, const char *target, const char *source, solar_os_robot_transform_t *out)
{ MODEL_CALL(solar_os_robot_model_lookup(m, target, source, out)); }
static int model_extension(uint32_t id, char *out, size_t bytes)
{ MODEL_CALL(solar_os_robot_model_extension(m, out, bytes)); }
static int model_binding(uint32_t id, uint32_t index, solar_os_robot_joint_config_v3_t *out)
{ MODEL_CALL(solar_os_robot_model_binding(m, index, out)); }
#undef MODEL_CALL

static int model_create_joint(uint32_t id, uint32_t index, uint32_t *out)
{
    if(!out) return ARG;
    *out=0;
    solar_os_robot_joint_config_v3_t c;
    int e=model_binding(id,index,&c);
    if(e) return e;
    uint32_t joint=0;
    e=create(c.name,c.kind,&joint);
    if(e) return e;
    lock();
    joint_t *a=find(joint);
    a->info.config=c; a->ppr_configured=a->ratio_configured=true;
    unlock(); *out=joint;
    return OK;
}

static const solar_os_robot_api_v3_t api = {.abi_version = SOLAR_OS_ROBOT_ABI, .struct_size = sizeof(api),
    .create = create, .configure = configure, .bind_motor = bind_motor, .bind_servo = bind_servo,
    .command = command, .enable = enable, .disable = disable, .status = status, .close = close_joint,
    .model_load = model_load, .model_close = model_close, .model_info = model_info,
    .model_link = model_link, .model_joint = model_joint, .model_set_positions = model_set_positions,
    .model_lookup = model_lookup, .model_extension = model_extension,
    .transform_from_rpy = solar_os_robot_transform_from_rpy, .transform_compose = solar_os_robot_transform_compose,
    .transform_inverse = solar_os_robot_transform_inverse, .transform_apply = solar_os_robot_transform_apply,
    .model_binding=model_binding, .model_create_joint=model_create_joint};

const solar_os_robot_api_v3_t *solar_os_robot_engine_init(
    const solar_os_native_memory_api_v1_t *m, const solar_os_native_time_api_v1_t *t,
    const solar_os_native_thread_api_v1_t *th, const solar_os_native_motion_api_v1_t *hw,
    const solar_os_native_controllers_api_v1_t *controllers)
{
    if (!m || !m->alloc || !m->free || !t || !t->now_us || !t->sleep_ms || !th ||
        !th->mutex_create || !th->mutex_lock || !th->mutex_unlock || !th->mutex_destroy ||
        !th->worker_start || !th->worker_join || !hw) return NULL;
    if(controllers && (controllers->abi_version!=SOLAR_OS_NATIVE_CONTROLLERS_ABI ||
        controllers->struct_size<sizeof(*controllers) || !controllers->open || !controllers->evaluate ||
        !controllers->pause || !controllers->fault || !controllers->close ||
        !controllers->open_bound || !controllers->command || !controllers->step_bound || !controllers->status_bound)) return NULL;
    memory = m; clock_api = t; threads = th; motion = hw; controllers_api=controllers;
    if (threads->mutex_create(&mutex, 0) != 0) return NULL;
    return &api;
}
int solar_os_robot_engine_shutdown(void)
{
    lock();
    for (unsigned i = 0; i < SOLAR_OS_ROBOT_JOINT_MAX; i++) if (joints[i]) { unlock(); return STATE; }
    for (unsigned i = 0; i < SOLAR_OS_ROBOT_MODEL_MAX; i++) if (models[i].object) { unlock(); return STATE; }
    unlock();
    if (threads->mutex_destroy(mutex) != 0) return STATE;
    mutex = NULL; return OK;
}
