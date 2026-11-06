#include "model.h"
#include "joint_config.h"
#include <math.h>
#include <string.h>

#define OK SOLAR_OS_NATIVE_OK
#define ARG SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT
#define STATE SOLAR_OS_NATIVE_ERROR_INVALID_STATE
#define MISSING SOLAR_OS_NATIVE_ERROR_NOT_FOUND
#define UNSUPPORTED SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED
#define OOM SOLAR_OS_NATIVE_ERROR_NO_MEMORY
#define NAME SOLAR_OS_ROBOT_MODEL_NAME_MAX
#define DEPTH 32U
#define ATTRS 24U
#define VALUE 256U

typedef struct {
    char name[NAME];
    int incoming;
} link_t;

typedef struct {
    solar_os_robot_model_joint_info_t info;
    int parent, child;
    unsigned fields;
} model_joint_t;

struct solar_os_robot_model {
    const solar_os_native_memory_api_v1_t *memory;
    solar_os_robot_model_info_t info;
    link_t links[SOLAR_OS_ROBOT_MODEL_LINK_MAX];
    model_joint_t joints[SOLAR_OS_ROBOT_MODEL_JOINT_MAX];
    char *extension;
    unsigned binding_count;
    bool drive_seen, drive_geometry, drive_timeout;
    solar_os_robot_diff_drive_config_t drive;
    struct {
        solar_os_robot_joint_config_v3_t config;
        uint32_t parameters, elements;
    } bindings[SOLAR_OS_ROBOT_JOINT_MAX];
};

typedef enum { ROBOT, LINK, JOINT, LEAF, SKIP, EXTENSION, EXTENSION_CHILD, BINDING } context_t;
typedef struct { char name[NAME]; context_t context; int index; } frame_t;
typedef struct { char name[NAME], value[VALUE]; } attribute_t;
typedef struct {
    solar_os_robot_model_t *model;
    const char *xml, *cursor, *end, *extension_start, *at, *bad_attribute;
    char *error;
    size_t error_bytes;
    frame_t stack[DEPTH];
    attribute_t attributes[ATTRS];
    unsigned depth, attrs, roots;
    bool extension_seen;
    char references[3 * SOLAR_OS_ROBOT_MODEL_JOINT_MAX][NAME];
    unsigned reference_kind[3 * SOLAR_OS_ROBOT_MODEL_JOINT_MAX];
    unsigned reference_count;
} parser_t;

static bool eq(const char *a, const char *b)
{
    return a && b && strlen(a) == strlen(b) && !memcmp(a, b, strlen(a));
}

static bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static bool digit(char c) { return c >= '0' && c <= '9'; }
static bool name_char(char c, bool first)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == ':' ||
        (!first && (digit(c) || c == '.' || c == '-'));
}

static bool copy_name(char *out, const char *in)
{
    if (!in || !*in || strlen(in) >= NAME) return false;
    memcpy(out, in, strlen(in) + 1);
    return true;
}

/* Keep diagnostics inside the caller's buffer without adding libc formatting
 * imports to the native library. Location is computed only on failure. */
static void error_append(parser_t *p, const char *text)
{
    if (!p->error || !p->error_bytes || !text) return;
    size_t used = strlen(p->error), n = strlen(text);
    if (n >= p->error_bytes - used) n = p->error_bytes - used - 1;
    memcpy(p->error + used, text, n); p->error[used + n] = 0;
}

static void error_number(parser_t *p, unsigned value)
{
    char digits[11]; unsigned n = 10;
    digits[n] = 0;
    do { digits[--n] = (char)('0' + value % 10); value /= 10; } while (value);
    error_append(p, digits + n);
}

static int fail(parser_t *p, int code, const char *message)
{
    if (!p->error || !p->error_bytes) return code;
    *p->error = 0;
    if (p->at) {
        unsigned line = 1, column = 1;
        for (const char *s = p->xml; s < p->at; ++s) {
            if (*s == '\r') { ++line; column = 1; }
            else if (*s == '\n') { if (s == p->xml || s[-1] != '\r') ++line; column = 1; }
            else ++column;
        }
        error_append(p, "line "); error_number(p, line);
        error_append(p, ", column "); error_number(p, column);
        error_append(p, ": ");
    }
    error_append(p, message);
    if (p->bad_attribute && p->depth < DEPTH) {
        error_append(p, " '"); error_append(p, p->bad_attribute);
        error_append(p, "' on <"); error_append(p, p->stack[p->depth].name);
        for (unsigned i = 0; i < p->attrs; ++i) if (eq(p->attributes[i].name, "name")) {
            error_append(p, " name='"); error_append(p, p->attributes[i].value);
            error_append(p, "'"); break;
        }
        error_append(p, ">");
    }
    return code;
}

static void whitespace(parser_t *p) { while (p->cursor < p->end && space(*p->cursor)) ++p->cursor; }
static bool starts(const parser_t *p, const char *s)
{
    size_t n = strlen(s);
    return (size_t)(p->end - p->cursor) >= n && !memcmp(p->cursor, s, n);
}

static bool xml_name(parser_t *p, char out[NAME])
{
    unsigned n = 0;
    while (p->cursor < p->end && name_char(*p->cursor, n == 0)) {
        if (n + 1 >= NAME) return false;
        out[n++] = *p->cursor++;
    }
    out[n] = 0;
    return n != 0;
}

/* Decode XML's five named entities and numeric Unicode references. No DTD or
 * external entity expansion. Literal UTF-8 is preserved without truncation. */
static bool entity(parser_t *p, char out[4], unsigned *length)
{
    const char *begin = ++p->cursor;
    while (p->cursor < p->end && *p->cursor != ';' && p->cursor - begin < 16) ++p->cursor;
    if (p->cursor == p->end || *p->cursor != ';') return false;
    size_t n = p->cursor++ - begin;
    static const char *names[] = {"amp", "lt", "gt", "quot", "apos"};
    static const char chars[] = {'&', '<', '>', '"', '\''};
    for (unsigned i = 0; i < 5; ++i) if (n == strlen(names[i]) && !memcmp(begin, names[i], n)) {
        out[0] = chars[i]; *length = 1; return true;
    }
    if (n < 2 || *begin++ != '#') return false;
    const char *finish = p->cursor - 1;
    unsigned base = 10, value = 0, count = 0;
    if (*begin == 'x') { base = 16; ++begin; }
    for (; begin < finish; ++begin) {
        unsigned d = digit(*begin) ? (unsigned)(*begin-'0') :
            *begin >= 'a' && *begin <= 'f' ? (unsigned)(*begin-'a'+10) :
            *begin >= 'A' && *begin <= 'F' ? (unsigned)(*begin-'A'+10) : 99;
        if (d >= base || value > (0x10ffffU - d)/base) return false;
        value = value*base + d; ++count;
    }
    if (!count || !(value == 9 || value == 10 || value == 13 ||
        (value >= 0x20 && value <= 0xd7ff) || (value >= 0xe000 && value <= 0xfffd) ||
        (value >= 0x10000 && value <= 0x10ffff))) return false;
    if (value < 0x80) { out[0] = (char)value; *length = 1; }
    else if (value < 0x800) { out[0] = (char)(0xc0 | (value>>6)); out[1] = (char)(0x80 | (value&63)); *length = 2; }
    else if (value < 0x10000) {
        out[0] = (char)(0xe0 | (value>>12)); out[1] = (char)(0x80 | ((value>>6)&63));
        out[2] = (char)(0x80 | (value&63)); *length = 3;
    } else {
        out[0] = (char)(0xf0 | (value>>18)); out[1] = (char)(0x80 | ((value>>12)&63));
        out[2] = (char)(0x80 | ((value>>6)&63)); out[3] = (char)(0x80 | (value&63)); *length = 4;
    }
    return true;
}

static bool attribute_value(parser_t *p, char out[VALUE])
{
    if (p->cursor == p->end || (*p->cursor != '\'' && *p->cursor != '"')) return false;
    char quote = *p->cursor++;
    unsigned used = 0;
    while (p->cursor < p->end && *p->cursor != quote) {
        char bytes[4]; unsigned n = 1;
        if (*p->cursor == '&') { if (!entity(p, bytes, &n)) return false; }
        else {
            char c = *p->cursor++;
            if (c == '<' || ((unsigned char)c < 0x20 && !space(c))) return false;
            bytes[0] = space(c) ? ' ' : c;
        }
        if (used + n >= VALUE) return false;
        memcpy(out + used, bytes, n); used += n;
    }
    if (p->cursor == p->end) return false;
    ++p->cursor; out[used] = 0;
    return true;
}

static const char *attr(const parser_t *p, const char *name)
{
    for (unsigned i = 0; i < p->attrs; ++i) if (eq(p->attributes[i].name, name)) return p->attributes[i].value;
    return NULL;
}

/* A bounded decimal parser keeps the ELF independent of libc strtof/locale.
 * Reject NaN, infinity, hex floats, overflow, trailing tokens and Xacro. */
static bool numbers(const char *s, float *out, unsigned count)
{
    if (!s) return false;
    for (unsigned i = 0; i < count; ++i) {
        while (space(*s)) ++s;
        bool negative = *s == '-';
        if (*s == '+' || *s == '-') ++s;
        double v = 0, fraction = .1;
        unsigned digits = 0;
        while (digit(*s)) { v = v*10 + (*s++-'0'); ++digits; }
        if (*s == '.') {
            ++s;
            while (digit(*s)) { v += (*s++-'0')*fraction; fraction *= .1; ++digits; }
        }
        if (!digits) return false;
        if (*s == 'e' || *s == 'E') {
            ++s; bool inverse = *s == '-';
            if (*s == '+' || *s == '-') ++s;
            unsigned exponent = 0, n = 0;
            while (digit(*s)) {
                if (exponent > 300) return false;
                exponent = exponent*10 + (*s++-'0'); ++n;
            }
            if (!n || exponent > 300) return false;
            while (exponent--) v = inverse ? v/10 : v*10;
        }
        out[i] = (float)(negative ? -v : v);
        if (!isfinite(out[i]) || (v != 0 && out[i] == 0)) return false;
        if (i + 1 < count && !space(*s)) return false;
    }
    while (space(*s)) ++s;
    return !*s;
}

static int link_index(const solar_os_robot_model_t *m, const char *name)
{
    for (unsigned i = 0; i < m->info.links; ++i) if (eq(m->links[i].name, name)) return (int)i;
    return -1;
}
static int joint_index(const solar_os_robot_model_t *m, const char *name)
{
    for (unsigned i = 0; i < m->info.joints; ++i) if (eq(m->joints[i].info.name, name)) return (int)i;
    return -1;
}

static int reference(parser_t *p, const char *name, unsigned kind)
{
    if (p->reference_count >= 3*SOLAR_OS_ROBOT_MODEL_JOINT_MAX ||
        !copy_name(p->references[p->reference_count], name)) return fail(p, ARG, "Invalid SolarOS joint reference");
    if (kind == 1) for (unsigned i = 0; i < p->reference_count; ++i)
        if (p->reference_kind[i] == 1 && eq(p->references[i], name)) return fail(p, ARG, "Duplicate SolarOS joint binding");
    p->reference_kind[p->reference_count] = kind;
    ++p->reference_count;
    return OK;
}

static bool allowed_attributes(parser_t *p, const char *const *names, unsigned count)
{
    p->bad_attribute = NULL;
    for (unsigned i=0; i<p->attrs; ++i) {
        unsigned j=0;
        while (j<count && !eq(p->attributes[i].name,names[j])) ++j;
        if (j==count) { p->bad_attribute = p->attributes[i].name; return false; }
    }
    return true;
}

static int binding_parameter(parser_t *p, unsigned index, const char *key, const char *value)
{
    static const char *const keys[] = {"ppr", "gear_ratio", "motor_polarity", "encoder_polarity",
        "position_min", "position_max", "position_zero", "velocity_limit", "acceleration_limit",
        "max_drive", "velocity_tau", "friction_bias_forward", "friction_bias_reverse", "velocity_deadband",
        "kp", "ki", "kd", "tau", "encoder_on_output", "brake", "period_ms", "command_timeout_ms",
        "feedback_timeout_ms", "max_gap_ms"};
    unsigned k=0;
    while (k<sizeof(keys)/sizeof(keys[0]) && !eq(key,keys[k])) ++k;
    if (k==sizeof(keys)/sizeof(keys[0])) return fail(p,ARG,"Unknown joint parameter");
    uint32_t bit=1U<<k;
    if (p->model->bindings[index].parameters & bit) return fail(p,ARG,"Duplicate joint parameter");
    float v;
    if (!numbers(value,&v,1) || solar_os_robot_config_set(&p->model->bindings[index].config,key,v))
        return fail(p,ARG,"Invalid joint parameter");
    p->model->bindings[index].parameters |= bit;
    return OK;
}

static int binding_element(parser_t *p, const char *name, unsigned index)
{
    solar_os_robot_joint_config_v3_t *c=&p->model->bindings[index].config;
    const char *const motor_attrs[]={"device","polarity"};
    const char *const encoder_attrs[]={"device","ppr","placement","polarity"};
    const char *const transmission_attrs[]={"gear_ratio"};
    const char *const param_attrs[]={"name","value"};
    unsigned bit=eq(name,"motor")?1:eq(name,"encoder")?2:eq(name,"transmission")?4:0;
    if (bit && (p->model->bindings[index].elements & bit)) return fail(p,ARG,"Duplicate binding element");
    p->model->bindings[index].elements |= bit;
    if (eq(name,"param")) {
        if (!allowed_attributes(p,param_attrs,2)) return fail(p,ARG,"Unknown param attribute");
        return binding_parameter(p,index,attr(p,"name"),attr(p,"value"));
    }
    if (bit==1 || bit==2) {
        if (!allowed_attributes(p,bit==1?motor_attrs:encoder_attrs,bit==1?2:4))
            return fail(p,ARG,"Unknown device binding attribute");
        const char *device=attr(p,"device");
        if (!device || !*device || strlen(device)>=sizeof(c->motor)) return fail(p,ARG,"Invalid device name");
        memcpy(bit==1?c->motor:c->encoder,device,strlen(device)+1);
        const char *polarity=attr(p,"polarity");
        if (polarity) { int e=binding_parameter(p,index,bit==1?"motor_polarity":"encoder_polarity",polarity); if(e)return e; }
        if (bit==2) {
            const char *ppr=attr(p,"ppr"), *placement=attr(p,"placement");
            if(ppr) { int e=binding_parameter(p,index,"ppr",ppr); if(e)return e; }
            if(placement) {
                if(!eq(placement,"motor") && !eq(placement,"output")) return fail(p,ARG,"Invalid encoder placement");
                return binding_parameter(p,index,"encoder_on_output",eq(placement,"output")?"1":"0");
            }
        }
        return OK;
    }
    if (bit==4) {
        if (!allowed_attributes(p,transmission_attrs,1)) return fail(p,ARG,"Unknown transmission attribute");
        return binding_parameter(p,index,"gear_ratio",attr(p,"gear_ratio"));
    }
    return fail(p,UNSUPPORTED,"Unsupported joint binding element");
}

static int start_element(parser_t *p, const char *name, frame_t *f)
{
    solar_os_robot_model_t *m = p->model;
    f->index = -1;
    if (!p->depth) {
        if (p->roots++ || !eq(name, "robot") || !copy_name(m->info.name, attr(p, "name")))
            return fail(p, ARG, "Expected one named robot root");
        const char *version = attr(p, "version");
        if (version && !eq(version, "1.0")) return fail(p, UNSUPPORTED, "Unsupported URDF version");
        f->context = ROBOT; return OK;
    }
    const frame_t *parent = &p->stack[p->depth-1];
    if (parent->context == LEAF) return fail(p, ARG, "Unexpected nested joint field");
    if (parent->context == SKIP) { f->context = SKIP; return OK; }
    if (parent->context == BINDING) {
        f->context=LEAF;
        return binding_element(p,name,(unsigned)parent->index);
    }
    if (parent->context == EXTENSION || parent->context == EXTENSION_CHILD) {
        f->context = EXTENSION_CHILD;
        if (parent->context == EXTENSION && eq(name, "joint")) {
            const char *const attrs[]={"name","command"};
            if (!allowed_attributes(p,attrs,2)) return fail(p,ARG,"Unknown joint binding attribute");
            if (!eq(attr(p,"command"),"velocity")) return fail(p,UNSUPPORTED,"URDF bindings currently require command=velocity");
            if (m->binding_count>=SOLAR_OS_ROBOT_JOINT_MAX) return fail(p,ARG,"Too many joint bindings");
            const char *joint_name=attr(p,"name");
            if(!joint_name || !*joint_name || strlen(joint_name)>=SOLAR_OS_ROBOT_JOINT_NAME_MAX)
                return fail(p,ARG,"Invalid controller joint name");
            int e=reference(p,joint_name,1); if(e)return e;
            f->index=(int)m->binding_count++; f->context=BINDING;
            solar_os_robot_joint_config_v3_t *c=&m->bindings[f->index].config;
            solar_os_robot_config_defaults(c,SOLAR_OS_ROBOT_MOTOR_VELOCITY);
            memcpy(c->name,joint_name,strlen(joint_name)+1);
            return OK;
        }
        if (parent->context == EXTENSION && eq(name, "diff_drive")) {
            const char *const names[]={"name","left_joint","right_joint"};
            if(!allowed_attributes(p,names,3)) return fail(p,ARG,"Unknown diff_drive attribute");
            if(m->drive_seen) return fail(p,ARG,"Only one diff_drive per model is supported");
            m->drive_seen=true;
            solar_os_robot_diff_drive_config_t *d=&m->drive;
            const char *drive_name=attr(p,"name"),*left=attr(p,"left_joint"),*right=attr(p,"right_joint");
            if(!drive_name || !*drive_name || strlen(drive_name)>=sizeof(d->name) ||
               !left || strlen(left)>=sizeof(d->left_joint) || !right || strlen(right)>=sizeof(d->right_joint))
                return fail(p,ARG,"Invalid diff_drive or wheel name");
            /* Component names also form parameter/stream path components. */
            for(const char *s=drive_name;*s;++s) if(!name_char(*s,s==drive_name) || *s==':' || *s=='.')
                return fail(p,ARG,"Invalid diff_drive name");
            memcpy(d->name,drive_name,strlen(drive_name)+1);
            memcpy(d->left_joint,left,strlen(left)+1);memcpy(d->right_joint,right,strlen(right)+1);
            memcpy(d->odom_frame,"odom",5);d->command_timeout_ms=300;
            int e = reference(p, attr(p, "left_joint"), 2);
            if (e) return e;
            if (eq(attr(p, "left_joint"), attr(p, "right_joint"))) return fail(p, ARG, "Drive wheels must be distinct");
            return reference(p, attr(p, "right_joint"), 2);
        }
        if (eq(parent->name,"diff_drive")) {
            solar_os_robot_diff_drive_config_t *d=&m->drive;
            f->context=LEAF;
            if(eq(name,"geometry")) {
                const char *const names[]={"wheel_radius","wheel_separation"};
                if(!allowed_attributes(p,names,2)) return fail(p,ARG,"Unknown drive geometry attribute");
                if(m->drive_geometry || !numbers(attr(p,"wheel_radius"),&d->wheel_radius,1) ||
                   !numbers(attr(p,"wheel_separation"),&d->wheel_separation,1) ||
                   d->wheel_radius<=0 || d->wheel_separation<=0) return fail(p,ARG,"Invalid or duplicate drive geometry");
                m->drive_geometry=true;return OK;
            }
            if(eq(name,"odometry")) {
                const char *const names[]={"frame","base_frame"};
                if(!allowed_attributes(p,names,2)) return fail(p,ARG,"Unknown odometry attribute");
                if(d->base_frame[0] || !copy_name(d->odom_frame,attr(p,"frame")) ||
                   !copy_name(d->base_frame,attr(p,"base_frame")) || eq(d->odom_frame,d->base_frame))
                    return fail(p,ARG,"Invalid or duplicate odometry frames");
                return reference(p,d->base_frame,3);
            }
            if(eq(name,"param")) {
                const char *const names[]={"name","value"};float value;
                if(!allowed_attributes(p,names,2)) return fail(p,ARG,"Unknown param attribute");
                const char *key=attr(p,"name");
                if(!numbers(attr(p,"value"),&value,1) || value<=0) return fail(p,ARG,"Invalid diff_drive parameter");
                if(eq(key,"linear_velocity_limit") && !d->linear_limit) d->linear_limit=value;
                else if(eq(key,"angular_velocity_limit") && !d->angular_limit) d->angular_limit=value;
                else if(eq(key,"command_timeout_ms") && !m->drive_timeout && value>=10 && value<=60000 && value==(float)(uint32_t)value) {
                    d->command_timeout_ms=(uint32_t)value;m->drive_timeout=true;
                }
                else return fail(p,ARG,"Unknown or duplicate diff_drive parameter");
                return OK;
            }
            return fail(p,UNSUPPORTED,"Unsupported diff_drive element");
        }
        return OK;
    }
    if (eq(name, "solar_os") && parent->context != ROBOT) return fail(p, ARG, "solar_os must be a robot child");
    if (parent->context == ROBOT) {
        if (eq(name, "link")) {
            if (m->info.links >= SOLAR_OS_ROBOT_MODEL_LINK_MAX) return fail(p, ARG, "Too many links");
            link_t *l = &m->links[m->info.links];
            if (!copy_name(l->name, attr(p, "name")) || link_index(m, l->name) >= 0) return fail(p, ARG, "Invalid or duplicate link name");
            l->incoming = -1; f->index = (int)m->info.links++; f->context = LINK; return OK;
        }
        if (eq(name, "joint")) {
            if (m->info.joints >= SOLAR_OS_ROBOT_MODEL_JOINT_MAX) return fail(p, ARG, "Too many joints");
            model_joint_t *j = &m->joints[m->info.joints];
            if (!copy_name(j->info.name, attr(p, "name")) || joint_index(m, j->info.name) >= 0) return fail(p, ARG, "Invalid or duplicate joint name");
            const char *type = attr(p, "type");
            if (eq(type, "fixed")) j->info.kind = SOLAR_OS_ROBOT_MODEL_FIXED;
            else if (eq(type, "revolute")) j->info.kind = SOLAR_OS_ROBOT_MODEL_REVOLUTE;
            else if (eq(type, "continuous")) j->info.kind = SOLAR_OS_ROBOT_MODEL_CONTINUOUS;
            else if (eq(type, "prismatic")) j->info.kind = SOLAR_OS_ROBOT_MODEL_PRISMATIC;
            else return fail(p, UNSUPPORTED, "Unsupported joint type");
            j->info.origin.rotation[3] = 1; j->info.axis[0] = 1;
            j->info.position_known = j->info.kind == SOLAR_OS_ROBOT_MODEL_FIXED;
            f->context = JOINT; f->index = (int)m->info.joints++; return OK;
        }
        if (eq(name, "solar_os")) {
            if (p->extension_seen) return fail(p, ARG, "Duplicate solar_os block");
            if (!eq(attr(p, "version"), "1")) return fail(p, UNSUPPORTED, "Expected solar_os version 1");
            p->extension_seen = true; f->context = EXTENSION; return OK;
        }
        if (eq(name, "material") || eq(name, "gazebo") || eq(name, "ros2_control") || eq(name, "transmission")) {
            f->context = SKIP; return OK;
        }
        return fail(p, UNSUPPORTED, "Unsupported robot element; export expanded URDF");
    }
    if (parent->context == LINK) {
        if (!eq(name, "visual") && !eq(name, "collision") && !eq(name, "inertial"))
            return fail(p, UNSUPPORTED, "Unsupported link element");
        f->context = SKIP; return OK;
    }
    model_joint_t *j = &m->joints[parent->index];
    unsigned field;
    if (eq(name, "parent")) field = 1;
    else if (eq(name, "child")) field = 2;
    else if (eq(name, "origin")) field = 4;
    else if (eq(name, "axis")) field = 8;
    else if (eq(name, "limit")) field = 16;
    else if (eq(name, "dynamics") || eq(name, "calibration") || eq(name, "safety_controller")) {
        f->context = SKIP; return OK;
    } else return fail(p, UNSUPPORTED, "Unsupported joint element (including mimic)");
    if (j->fields & field) return fail(p, ARG, "Duplicate joint field");
    j->fields |= field; f->context = LEAF;
    if (field == 1 || field == 2) {
        if (!copy_name(field == 1 ? j->info.parent : j->info.child, attr(p, "link")))
            return fail(p, ARG, "Missing parent/child link");
    } else if (field == 4) {
        float xyz[3] = {0}, rpy[3] = {0};
        const char *a = attr(p, "xyz"), *b = attr(p, "rpy");
        if ((a && !numbers(a, xyz, 3)) || (b && !numbers(b, rpy, 3)) ||
            solar_os_robot_transform_from_rpy(xyz, rpy, &j->info.origin)) return fail(p, ARG, "Invalid joint origin");
    } else if (field == 8) {
        if (!numbers(attr(p, "xyz"), j->info.axis, 3)) return fail(p, ARG, "Invalid joint axis");
        if (!solar_os_robot_normalize_vector(j->info.axis, 3)) return fail(p, ARG, "Zero or invalid joint axis");
    } else {
        const char *lower = attr(p, "lower"), *upper = attr(p, "upper");
        if (!numbers(attr(p, "effort"), &j->info.effort, 1) || !numbers(attr(p, "velocity"), &j->info.velocity, 1) ||
            (lower && !numbers(lower, &j->info.lower, 1)) || (upper && !numbers(upper, &j->info.upper, 1)) ||
            j->info.effort < 0 || j->info.velocity < 0 || j->info.lower > j->info.upper)
            return fail(p, ARG, "Invalid joint limits");
        j->info.has_limits = 1;
    }
    return OK;
}

static int end_element(parser_t *p)
{
    frame_t *f = &p->stack[p->depth-1];
    if (f->context == BINDING) {
        const solar_os_robot_joint_config_v3_t *c=&p->model->bindings[f->index].config;
        if (!*c->motor || !*c->encoder || c->ppr<=0 || c->gear_ratio<=0 ||
            !solar_os_robot_config_valid(c,true,true)) return fail(p,ARG,"Incomplete or invalid joint controller configuration");
    }
    if (f->context == JOINT) {
        const model_joint_t *j = &p->model->joints[f->index];
        if ((j->fields & 3) != 3) return fail(p, ARG, "Joint needs parent and child");
        if ((j->info.kind == SOLAR_OS_ROBOT_MODEL_REVOLUTE || j->info.kind == SOLAR_OS_ROBOT_MODEL_PRISMATIC) &&
            !j->info.has_limits) return fail(p, ARG, "Bounded joint needs limits");
    }
    if (f->context == EXTENSION) {
        size_t n = p->cursor - p->extension_start;
        if (n > SOLAR_OS_ROBOT_MODEL_EXTENSION_MAX) return fail(p, ARG, "solar_os block too large");
        char *s = p->model->memory->alloc(n + 1);
        if (!s) return fail(p, OOM, "No memory for solar_os metadata");
        memcpy(s, p->extension_start, n); s[n] = 0;
        p->model->extension = s; p->model->info.extension_bytes = (uint32_t)n;
    }
    --p->depth;
    return OK;
}

static int parse_xml(parser_t *p)
{
    if (starts(p, "\xef\xbb\xbf")) p->cursor += 3;
    while (p->cursor < p->end) {
        p->at = p->cursor;
        if (*p->cursor != '<') {
            bool text_allowed = p->depth && (p->stack[p->depth-1].context == SKIP ||
                p->stack[p->depth-1].context == EXTENSION_CHILD);
            while (p->cursor < p->end && *p->cursor != '<') {
                if (starts(p, "]]>")) return fail(p, ARG, "Invalid XML text");
                if (*p->cursor == '&') {
                    char decoded[4]; unsigned n;
                    if (!text_allowed || !entity(p, decoded, &n)) return fail(p, ARG, "Invalid XML entity/text");
                } else {
                    char c = *p->cursor++;
                    if ((!text_allowed && !space(c)) || ((unsigned char)c < 0x20 && !space(c)))
                        return fail(p, ARG, "Unexpected XML text");
                }
            }
            continue;
        }
        if (starts(p, "<!--")) {
            p->cursor += 4;
            while (p->cursor < p->end && !starts(p, "--")) ++p->cursor;
            if (!starts(p, "-->")) return fail(p, ARG, "Invalid XML comment");
            p->cursor += 3; continue;
        }
        if (starts(p, "<?")) {
            p->cursor += 2;
            char name[NAME];
            if (!xml_name(p, name)) return fail(p, ARG, "Invalid processing instruction");
            while (p->cursor < p->end && !starts(p, "?>")) ++p->cursor;
            if (!starts(p, "?>")) return fail(p, ARG, "Unterminated processing instruction");
            p->cursor += 2; continue;
        }
        if (starts(p, "<![CDATA[")) {
            if (!p->depth || (p->stack[p->depth-1].context != SKIP && p->stack[p->depth-1].context != EXTENSION_CHILD))
                return fail(p, ARG, "Unexpected CDATA");
            p->cursor += 9;
            while (p->cursor < p->end && !starts(p, "]]>")) ++p->cursor;
            if (!starts(p, "]]>")) return fail(p, ARG, "Unterminated CDATA");
            p->cursor += 3; continue;
        }
        if (starts(p, "<!")) return fail(p, UNSUPPORTED, "DTD/declarations are unsupported");
        const char *begin = p->cursor++;
        bool closing = p->cursor < p->end && *p->cursor == '/';
        if (closing) ++p->cursor;
        char name[NAME];
        if (!xml_name(p, name)) return fail(p, ARG, "Invalid or oversized XML element name");
        if (closing) {
            whitespace(p);
            if (p->cursor == p->end || *p->cursor++ != '>' || !p->depth || !eq(p->stack[p->depth-1].name, name))
                return fail(p, ARG, "Mismatched XML closing tag");
            int e = end_element(p); if (e) return e;
            continue;
        }
        p->attrs = 0;
        for (;;) {
            const char *before = p->cursor;
            whitespace(p);
            if (starts(p, ">") || starts(p, "/>")) break;
            if (before == p->cursor || p->attrs >= ATTRS) return fail(p, ARG, "Invalid or excessive XML attributes");
            attribute_t *a = &p->attributes[p->attrs];
            if (!xml_name(p, a->name)) return fail(p, ARG, "Invalid XML attribute name");
            for (unsigned i = 0; i < p->attrs; ++i) if (eq(a->name, p->attributes[i].name)) return fail(p, ARG, "Duplicate XML attribute");
            whitespace(p);
            if (p->cursor == p->end || *p->cursor++ != '=') return fail(p, ARG, "Expected attribute assignment");
            whitespace(p);
            if (!attribute_value(p, a->value)) return fail(p, ARG, "Invalid or oversized XML attribute");
            ++p->attrs;
        }
        bool empty = starts(p, "/>"); p->cursor += empty ? 2 : 1;
        if (p->depth >= DEPTH) return fail(p, ARG, "XML nesting limit exceeded");
        frame_t *f = &p->stack[p->depth];
        memcpy(f->name, name, strlen(name)+1);
        int e = start_element(p, name, f); if (e) return e;
        if (f->context == EXTENSION) p->extension_start = begin;
        ++p->depth;
        if (empty) { e = end_element(p); if (e) return e; }
    }
    if (p->depth || p->roots != 1) return fail(p, ARG, "Incomplete XML document");
    return OK;
}

static int validate_tree(parser_t *p)
{
    solar_os_robot_model_t *m = p->model;
    if (!m->info.links || m->info.joints + 1 != m->info.links) return fail(p, ARG, "Expected one connected link tree");
    for (unsigned i = 0; i < m->info.joints; ++i) {
        model_joint_t *j = &m->joints[i];
        j->parent = link_index(m, j->info.parent); j->child = link_index(m, j->info.child);
        if (j->parent < 0 || j->child < 0 || j->parent == j->child) return fail(p, ARG, "Invalid joint link reference");
        if (m->links[j->child].incoming >= 0) return fail(p, ARG, "Link has multiple parents");
        m->links[j->child].incoming = (int)i;
    }
    int root = -1;
    for (unsigned i = 0; i < m->info.links; ++i) if (m->links[i].incoming < 0) {
        if (root >= 0) return fail(p, ARG, "Multiple root links");
        root = (int)i;
    }
    if (root < 0) return fail(p, ARG, "No root link");
    for (unsigned i = 0; i < m->info.links; ++i) {
        int current = (int)i;
        unsigned steps = 0;
        while (m->links[current].incoming >= 0 && steps++ < m->info.links)
            current = m->joints[m->links[current].incoming].parent;
        if (current != root || steps >= m->info.links) return fail(p, ARG, "Disconnected tree or joint cycle");
    }
    copy_name(m->info.root, m->links[root].name);
    if(m->drive_seen) {
        if(!m->drive_geometry) return fail(p,ARG,"diff_drive requires wheel geometry");
        if(!m->drive.base_frame[0]) copy_name(m->drive.base_frame,m->info.root);
        bool left=false,right=false;
        for(unsigned i=0;i<m->binding_count;++i) {
            left|=eq(m->bindings[i].config.name,m->drive.left_joint);
            right|=eq(m->bindings[i].config.name,m->drive.right_joint);
            if(eq(m->bindings[i].config.name,m->drive.left_joint) &&
               m->drive.command_timeout_ms<m->bindings[i].config.period_ms)
                return fail(p,ARG,"Drive command timeout is shorter than its update period");
        }
        if(!left || !right) return fail(p,ARG,"diff_drive wheels require velocity controller bindings");
        if(eq(m->drive.odom_frame,m->drive.base_frame)) return fail(p,ARG,"Odometry and base frames must differ");
    }
    for(unsigned i=0;i<m->binding_count;++i) {
        const solar_os_robot_joint_config_v3_t *c=&m->bindings[i].config;
        int index=joint_index(m,c->name);
        if(index>=0 && m->joints[index].info.kind==SOLAR_OS_ROBOT_MODEL_PRISMATIC)
            return fail(p,UNSUPPORTED,"Motor velocity bindings require rotary joints");
        for(unsigned k=0;k<i;++k) if(eq(c->motor,m->bindings[k].config.motor) || eq(c->encoder,m->bindings[k].config.encoder))
            return fail(p,ARG,"Joint bindings must use distinct motors and encoders");
    }
    for (unsigned i = 0; i < p->reference_count; ++i) {
        if (p->reference_kind[i] == 3) {
            if (link_index(m, p->references[i]) < 0) return fail(p, ARG, "SolarOS references an unknown base frame");
            continue;
        }
        int index = joint_index(m, p->references[i]);
        if (index < 0) return fail(p, ARG, "SolarOS references an unknown joint");
        uint32_t kind = m->joints[index].info.kind;
        if (kind == SOLAR_OS_ROBOT_MODEL_FIXED) return fail(p, ARG, "Cannot bind a fixed joint");
        if (p->reference_kind[i] == 2 && kind == SOLAR_OS_ROBOT_MODEL_PRISMATIC)
            return fail(p, ARG, "Drive wheels need rotary joints");
    }
    return OK;
}

int solar_os_robot_model_binding(const solar_os_robot_model_t *m, uint32_t index, solar_os_robot_joint_config_v3_t *out)
{
    if(!m || !out) return ARG;
    if(index>=m->binding_count) return MISSING;
    *out=m->bindings[index].config;
    return OK;
}

int solar_os_robot_model_diff_drive(const solar_os_robot_model_t *m,uint32_t index,solar_os_robot_diff_drive_config_t *out)
{
    if(!m || !out) return ARG;
    if(index || !m->drive_seen) return MISSING;
    *out=m->drive;return OK;
}

void solar_os_robot_model_destroy(solar_os_robot_model_t *m)
{
    if (!m) return;
    const solar_os_native_memory_api_v1_t *memory = m->memory;
    memory->free(m->extension); memory->free(m);
}

int solar_os_robot_model_parse(const solar_os_native_memory_api_v1_t *memory, const char *xml, size_t bytes,
    solar_os_robot_model_t **out, char *error, size_t error_bytes)
{
    if (out) *out = NULL;
    if (error && error_bytes) *error = 0;
    if (!memory || !memory->alloc || !memory->free || !out || !xml || !bytes || bytes > SOLAR_OS_ROBOT_MODEL_XML_MAX) return ARG;
    /* Validate UTF-8 and XML 1.0 characters before scanning syntax. This also
     * rejects embedded NULs, including inside comments and skipped geometry. */
    for (size_t i = 0; i < bytes;) {
        unsigned value = (unsigned char)xml[i++], continuation = 0, minimum = 0;
        if (value >= 0xc2 && value <= 0xdf) { value &= 31; continuation = 1; minimum = 0x80; }
        else if (value >= 0xe0 && value <= 0xef) { value &= 15; continuation = 2; minimum = 0x800; }
        else if (value >= 0xf0 && value <= 0xf4) { value &= 7; continuation = 3; minimum = 0x10000; }
        else if (value >= 0x80) return ARG;
        if (bytes - i < continuation) return ARG;
        for (unsigned k = 0; k < continuation; ++k) {
            unsigned ch = (unsigned char)xml[i++];
            if ((ch & 0xc0) != 0x80) return ARG;
            value = (value << 6) | (ch & 63);
        }
        if (value < minimum || !(value == 9 || value == 10 || value == 13 ||
            (value >= 0x20 && value <= 0xd7ff) || (value >= 0xe000 && value <= 0xfffd) ||
            (value >= 0x10000 && value <= 0x10ffff))) return ARG;
    }
    solar_os_robot_model_t *m = memory->alloc(sizeof(*m));
    if (!m) return OOM;
    memset(m, 0, sizeof(*m)); m->memory = memory;
    parser_t *p = memory->alloc(sizeof(*p));
    if (!p) { solar_os_robot_model_destroy(m); return OOM; }
    memset(p, 0, sizeof(*p));
    p->model = m; p->xml = p->cursor = xml; p->end = xml + bytes;
    p->error = error; p->error_bytes = error_bytes;
    int e = parse_xml(p);
    /* Cross-reference/tree failures have no single offending XML token. */
    p->at = NULL;
    if (!e) e = validate_tree(p);
    memory->free(p);
    if (e) solar_os_robot_model_destroy(m);
    else *out = m;
    return e;
}

int solar_os_robot_model_info(const solar_os_robot_model_t *m, solar_os_robot_model_info_t *out)
{
    if (!m || !out) return ARG;
    *out = m->info; out->known_joints = 0;
    for (unsigned i = 0; i < m->info.joints; ++i) out->known_joints += m->joints[i].info.position_known;
    return OK;
}
int solar_os_robot_model_link(const solar_os_robot_model_t *m, uint32_t index, char *out, size_t bytes)
{
    if (!m || !out || !bytes) return ARG;
    if (index >= m->info.links) return MISSING;
    size_t n = strlen(m->links[index].name)+1;
    if (bytes < n) return ARG;
    memcpy(out, m->links[index].name, n); return OK;
}
int solar_os_robot_model_joint(const solar_os_robot_model_t *m, uint32_t index, solar_os_robot_model_joint_info_t *out)
{
    if (!m || !out) return ARG;
    if (index >= m->info.joints) return MISSING;
    *out = m->joints[index].info; return OK;
}
int solar_os_robot_model_extension(const solar_os_robot_model_t *m, char *out, size_t bytes)
{
    if (!m || !out || bytes <= m->info.extension_bytes) return ARG;
    if (m->extension) memcpy(out, m->extension, m->info.extension_bytes+1);
    else *out = 0;
    return OK;
}

int solar_os_robot_model_set_positions(solar_os_robot_model_t *m, const char *const *names, const float *positions, uint32_t count)
{
    if (!m || count > m->info.joints || (count && (!names || !positions))) return ARG;
    int indices[SOLAR_OS_ROBOT_MODEL_JOINT_MAX];
    for (unsigned i = 0; i < count; ++i) {
        int index = joint_index(m, names[i]);
        if (index < 0) return MISSING;
        const solar_os_robot_model_joint_info_t *j = &m->joints[index].info;
        /* This is observed/supplied state, not a command. Preserve the actual
         * pose even outside declared travel limits; controllers own limits. */
        if (!isfinite(positions[i]) || j->kind == SOLAR_OS_ROBOT_MODEL_FIXED) return ARG;
        for (unsigned k = 0; k < i; ++k) if (indices[k] == index) return ARG;
        indices[i] = index;
    }
    /* Apply only after every element passes: callers see coherent batches. */
    for (unsigned i = 0; i < count; ++i) {
        m->joints[indices[i]].info.position = positions[i];
        m->joints[indices[i]].info.position_known = 1;
    }
    return OK;
}

int solar_os_robot_model_feedback(solar_os_robot_model_t *m, const char *name,
    float position, bool valid, bool referenced)
{
    if (!m || !name) return ARG;
    int index = joint_index(m, name);
    if (index < 0) return MISSING;
    solar_os_robot_model_joint_info_t *j = &m->joints[index].info;
    if (j->kind == SOLAR_OS_ROBOT_MODEL_FIXED) return ARG;
    j->position_known = valid && isfinite(position) &&
        (referenced || j->kind == SOLAR_OS_ROBOT_MODEL_CONTINUOUS);
    if (j->position_known) j->position = position;
    return OK;
}

static int edge_transform(const model_joint_t *j, solar_os_robot_transform_t *out)
{
    const solar_os_robot_model_joint_info_t *s = &j->info;
    if (!s->position_known) return STATE;
    solar_os_robot_transform_t motion = {.rotation = {0, 0, 0, 1}};
    if (s->kind == SOLAR_OS_ROBOT_MODEL_PRISMATIC)
        for (unsigned i = 0; i < 3; ++i) motion.translation[i] = s->axis[i]*s->position;
    else if (s->kind != SOLAR_OS_ROBOT_MODEL_FIXED) {
        float sine = sinf(s->position/2);
        for (unsigned i = 0; i < 3; ++i) motion.rotation[i] = s->axis[i]*sine;
        motion.rotation[3] = cosf(s->position/2);
    }
    return solar_os_robot_transform_compose(&s->origin, &motion, out);
}

int solar_os_robot_model_lookup(const solar_os_robot_model_t *m, const char *target, const char *source, solar_os_robot_transform_t *out)
{
    if (!m || !target || !source || !out) return ARG;
    int a = link_index(m, target), b = link_index(m, source);
    if (a < 0 || b < 0) return MISSING;
    /* Stop at the lowest common ancestor. Unknown joints above that ancestor
     * do not make a local transform unavailable. No recursive stack growth. */
    int path[SOLAR_OS_ROBOT_MODEL_LINK_MAX]; unsigned n = 0;
    for (int k = a; k >= 0;) {
        path[n++] = k;
        int incoming = m->links[k].incoming;
        k = incoming < 0 ? -1 : m->joints[incoming].parent;
    }
    int ancestor = b;
    for (;;) {
        bool found = false;
        for (unsigned i = 0; i < n; ++i) if (path[i] == ancestor) { found = true; break; }
        if (found) break;
        ancestor = m->joints[m->links[ancestor].incoming].parent;
    }
    solar_os_robot_transform_t ta = {.rotation = {0,0,0,1}}, tb = ta;
    int nodes[2] = {a,b}; solar_os_robot_transform_t *transforms[2] = {&ta,&tb};
    for (unsigned side = 0; side < 2; ++side) {
        while (nodes[side] != ancestor) {
            const model_joint_t *j = &m->joints[m->links[nodes[side]].incoming];
            solar_os_robot_transform_t edge;
            int e = edge_transform(j, &edge);
            if (e) return e;
            e = solar_os_robot_transform_compose(&edge, transforms[side], transforms[side]);
            if (e) return e;
            nodes[side] = j->parent;
        }
    }
    int e = solar_os_robot_transform_inverse(&ta, &ta);
    return e ? e : solar_os_robot_transform_compose(&ta, &tb, out);
}
