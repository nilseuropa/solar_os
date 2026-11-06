#include "model.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OK SOLAR_OS_NATIVE_OK
#define ARG SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT
#define STATE SOLAR_OS_NATIVE_ERROR_INVALID_STATE
#define MISSING SOLAR_OS_NATIVE_ERROR_NOT_FOUND
#define UNSUPPORTED SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED
#define OOM SOLAR_OS_NATIVE_ERROR_NO_MEMORY
static unsigned allocations, attempts, fail_at;
static void *allocate(size_t bytes)
{
    if (++attempts == fail_at) return NULL;
    void *p = malloc(bytes); if (p) ++allocations; return p;
}
static void deallocate(void *p) { if (p) { assert(allocations); --allocations; free(p); } }
static const solar_os_native_memory_api_v1_t memory = {.alloc = allocate, .free = deallocate};
static void near(float a, float b) { assert(fabsf(a-b) < 2e-5f); }
static solar_os_robot_model_t *parse(const char *xml)
{
    solar_os_robot_model_t *m = NULL; char error[128];
    int e = solar_os_robot_model_parse(&memory, xml, strlen(xml), &m, error, sizeof(error));
    if (e) fprintf(stderr, "parse: %s\n", error);
    assert(e == OK && m); return m;
}
static void reject(const char *xml)
{
    unsigned before = allocations;
    solar_os_robot_model_t *m = (void *)1; char error[128];
    assert(solar_os_robot_model_parse(&memory, xml, strlen(xml), &m, error, sizeof(error)) != OK);
    assert(!m && allocations == before);
}

static void transform_math(void)
{
    float xyz[3] = {1,2,3}, rpy[3] = {0,0,1.5707963267948966f};
    solar_os_robot_transform_t t, inv, identity, saved;
    assert(solar_os_robot_transform_from_rpy(xyz,rpy,&t) == OK);
    float point[3] = {1,0,0};
    assert(solar_os_robot_transform_apply(&t,point,0,point) == OK);
    near(point[0],1);near(point[1],3);near(point[2],3);
    assert(solar_os_robot_transform_inverse(&t,&inv) == OK);
    assert(solar_os_robot_transform_apply(&inv,point,0,point) == OK);
    near(point[0],1);near(point[1],0);near(point[2],0);
    assert(solar_os_robot_transform_compose(&t,&inv,&identity) == OK);
    for (unsigned i=0;i<3;++i) { near(identity.translation[i],0);near(identity.rotation[i],0); }
    near(identity.rotation[3],1);
    point[0]=1;
    assert(solar_os_robot_transform_apply(&t,point,1,point) == OK);
    near(point[0],0);near(point[1],1);near(point[2],0);
    saved = t;
    assert(solar_os_robot_transform_compose(&t,&inv,&t) == OK); /* aliased output */
    near(t.translation[0],0);
    t = saved; assert(solar_os_robot_transform_inverse(&t,&t) == OK);
    near(t.translation[0],inv.translation[0]);
    t.rotation[0]=t.rotation[1]=t.rotation[2]=t.rotation[3]=0;
    assert(solar_os_robot_transform_inverse(&t,&identity) == ARG);
    t=saved;t.translation[0]=NAN;
    assert(solar_os_robot_transform_apply(&t,point,0,point) == ARG);
    t=saved;t.rotation[3]*=2;t.rotation[2]*=2; /* caller quaternions normalized */
    point[0]=1;point[1]=point[2]=0;
    assert(solar_os_robot_transform_apply(&t,point,1,point) == OK);near(point[1],1);
    xyz[0]=INFINITY;
    assert(solar_os_robot_transform_from_rpy(xyz,rpy,&t) == ARG);
    /* Independently known Rz(yaw)*Ry(pitch)*Rx(roll) result. */
    xyz[0]=xyz[1]=xyz[2]=0;rpy[0]=rpy[1]=rpy[2]=1.5707963267948966f;
    assert(solar_os_robot_transform_from_rpy(xyz,rpy,&t) == OK);
    point[0]=1;point[1]=point[2]=0;
    assert(solar_os_robot_transform_apply(&t,point,1,point) == OK);
    near(point[0],0);near(point[1],0);near(point[2],-1);
}

static const char chain[] =
    "<robot name='chain'><link name='root'/><link name='arm'/><link name='tip'/><link name='sensor'/>"
    "<joint name='rotation' type='revolute'><parent link='root'/><child link='arm'/>"
    "<origin xyz='1 0 0' rpy='0 0 1.5707963267948966'/><axis xyz='0 0 2'/>"
    "<limit lower='-3.2' upper='3.2' velocity='2' effort='5'/></joint>"
    "<joint name='slide' type='prismatic'><parent link='arm'/><child link='tip'/>"
    "<origin xyz='1 0 0'/><axis xyz='1 0 0'/><limit lower='0' upper='2' velocity='2' effort='5'/></joint>"
    "<joint name='mount' type='fixed'><parent link='tip'/><child link='sensor'/><origin xyz='0 0 1'/></joint>"
    "</robot>";

static void kinematics(void)
{
    solar_os_robot_model_t *m=parse(chain);
    solar_os_robot_transform_t t;
    solar_os_robot_model_info_t info;
    assert(solar_os_robot_model_info(m,&info)==OK && info.known_joints==1);
    assert(solar_os_robot_model_lookup(m,"root","sensor",&t)==STATE);
    assert(solar_os_robot_model_lookup(m,"tip","sensor",&t)==OK);near(t.translation[2],1);
    assert(solar_os_robot_model_lookup(m,"arm","arm",&t)==OK);near(t.rotation[3],1);
    /* Relative counts cannot establish a bounded joint's absolute pose. */
    assert(solar_os_robot_model_feedback(m,"rotation",.5f,true,false)==OK);
    assert(solar_os_robot_model_lookup(m,"root","sensor",&t)==STATE);
    assert(solar_os_robot_model_feedback(m,"rotation",.5f,true,true)==OK);
    solar_os_robot_model_joint_info_t feedback_joint;
    assert(solar_os_robot_model_joint(m,0,&feedback_joint)==OK && feedback_joint.position_known);
    assert(solar_os_robot_model_feedback(m,"rotation",NAN,true,true)==OK);
    assert(solar_os_robot_model_joint(m,0,&feedback_joint)==OK && !feedback_joint.position_known);
    const char *names[]={"rotation","slide"};float q[]={1.5707963267948966f,.5f};
    assert(solar_os_robot_model_set_positions(m,names,q,2)==OK);
    assert(solar_os_robot_model_lookup(m,"root","sensor",&t)==OK);
    /* Origin yaw + joint rotation = pi; prismatic axis is joint-local. */
    near(t.translation[0],-.5f);near(t.translation[1],0);near(t.translation[2],1);
    assert(solar_os_robot_model_lookup(m,"sensor","root",&t)==OK);
    near(t.translation[0],-.5f);near(t.translation[2],-1);
    assert(solar_os_robot_model_lookup(m,"missing","root",&t)==MISSING);
    q[0]=0;q[1]=NAN;
    assert(solar_os_robot_model_set_positions(m,names,q,2)==ARG);
    solar_os_robot_model_joint_info_t j;
    assert(solar_os_robot_model_joint(m,0,&j)==OK);near(j.position,1.5707963267948966f);
    names[1]="rotation";q[1]=0;
    assert(solar_os_robot_model_set_positions(m,names,q,2)==ARG);
    names[1]="mount";assert(solar_os_robot_model_set_positions(m,names,q,2)==ARG);
    names[1]="missing";assert(solar_os_robot_model_set_positions(m,names,q,2)==MISSING);
    names[1]="slide";q[1]=NAN;assert(solar_os_robot_model_set_positions(m,names,q,2)==ARG);
    q[0]=0;q[1]=3;
    assert(solar_os_robot_model_set_positions(m,names,q,2)==OK); /* Actual state may exceed a declared limit. */
    assert(solar_os_robot_model_lookup(m,"root","sensor",&t)==OK);
    near(t.translation[0],1);near(t.translation[1],4);near(t.translation[2],1);
    solar_os_robot_model_destroy(m);assert(!allocations);
}

static void parser_validation(void)
{
    const char *bad[]={
        "", "<robot name='x'>", "<robot name='x'/><robot name='y'/>",
        "<robot name='x'><link name='a'></robot>",
        "<robot name='x'><link name='a'/><link name='a'/></robot>",
        "<robot name='x'><link name='a'/><link name='b'/></robot>",
        "<robot name='x'><link name='a'/><joint name='j' type='fixed'><parent link='a'/><child link='a'/></joint></robot>",
        "<robot name='x'><link name='a'/><link name='b'/><joint name='j' type='fixed'><parent link='z'/><child link='b'/></joint></robot>",
        "<robot name='x'><link name='a'/><link name='b'/><joint name='j' type='floating'><parent link='a'/><child link='b'/></joint></robot>",
        "<robot name='x'><link name='a'/><link name='b'/><joint name='j' type='revolute'><parent link='a'/><child link='b'/></joint></robot>",
        "<robot name='x'><link name='a'/><link name='b'/><joint name='j' type='continuous'><parent link='a'/><child link='b'/><axis xyz='0 0 0'/></joint></robot>",
        "<robot name='x'><link name='a'/><link name='b'/><joint name='j' type='fixed'><parent link='a'/><child link='b'/><origin xyz='NaN 0 0'/></joint></robot>",
        "<robot name='x'><link name='a'/><link name='b'/><joint name='j' type='fixed'><parent link='a'/><child link='b'/><origin xyz='1e99 0 0'/></joint></robot>",
        "<robot name='x'><link name='a'/><link name='b'/><joint name='j' type='fixed'><parent link='a'/><child link='b'/><origin xyz='1 2 3 4'/></joint></robot>",
        "<robot name='x'><link name='a'/><link name='b'/><joint name='j' type='fixed'><parent link='a'/><child link='b'/><mimic joint='other'/></joint></robot>",
        "<robot name='x'><link name='a'/><link name='b'/><joint name='j' type='fixed'><parent link='a'/><parent link='b'/><child link='b'/></joint></robot>",
        "<robot name='x'><link name='a'/><solar_os version='2'/></robot>",
        "<robot name='x'><link name='a'/><solar_os version='1'/><solar_os version='1'/></robot>",
        "<robot name='x'><link name='a'/><solar_os version='1'><joint name='missing'/></solar_os></robot>",
        "<!DOCTYPE robot><robot name='x'><link name='a'/></robot>",
        "<robot name='x' name='y'><link name='a'/></robot>",
        "<robot name='&unknown;'><link name='a'/></robot>",
        "<robot name='&#0;'><link name='a'/></robot>",
        "<robot name='x'><xacro:wheel/></robot>",
        "<robot name='x'><link name='a'/><!-- invalid -- comment --></robot>",
        "<robot name='x'><link name='a'/>text</robot>",
        /* One root plus a disconnected cycle still has N-1 joints. */
        "<robot name='x'><link name='root'/><link name='a'/><link name='b'/>"
        "<joint name='a' type='fixed'><parent link='a'/><child link='b'/></joint>"
        "<joint name='b' type='fixed'><parent link='b'/><child link='a'/></joint></robot>",
        /* A duplicate child with the expected edge count. */
        "<robot name='x'><link name='root'/><link name='a'/><link name='b'/>"
        "<joint name='a' type='fixed'><parent link='root'/><child link='a'/></joint>"
        "<joint name='b' type='fixed'><parent link='b'/><child link='a'/></joint></robot>",
    };
    for (unsigned i=0;i<sizeof(bad)/sizeof(*bad);++i) reject(bad[i]);
    solar_os_robot_model_t *m=parse("<?xml version='1.0'?><!-- hi --><robot name='A&amp;B'><link name='r&#111;ot'/><solar_os version='1'/></robot>");
    solar_os_robot_model_info_t info;assert(solar_os_robot_model_info(m,&info)==OK);
    assert(!strcmp(info.name,"A&B") && !strcmp(info.root,"root"));
    char extension[64];assert(solar_os_robot_model_extension(m,extension,sizeof(extension))==OK);
    assert(!strcmp(extension,"<solar_os version='1'/>"));
    assert(solar_os_robot_model_extension(m,extension,1)==ARG);
    solar_os_robot_model_destroy(m);
    /* Every truncated prefix must fail and release its allocations. */
    for (size_t n=1;n<strlen(chain);++n) {
        m=NULL;assert(solar_os_robot_model_parse(&memory,chain,n,&m,NULL,0)!=OK && !m && !allocations);
    }
    for (unsigned i=1;i<=3;++i) {
        attempts=0;fail_at=i;
        const char *xml="<robot name='x'><link name='r'/><solar_os version='1'/></robot>";
        assert(solar_os_robot_model_parse(&memory,xml,strlen(xml),&m,NULL,0)==OOM && !m && !allocations);
    }
    fail_at=0;
    reject("<robot name='\xff'><link name='r'/></robot>");
    reject("<robot name='x'><link name='r'/><!--\x01--></robot>");
    reject("<robot name='x'><link name='r'/><solar_os version='1'><joint name='a'/><joint name='a'/></solar_os></robot>");
    /* Model joint names inside the extension are references, not URDF edges. */
    const char *metadata =
        "<robot name='meta'><solar_os version='1'><joint name='left' command='velocity'>"
        "<motor device='m'/><encoder device='e' ppr='70'/><transmission gear_ratio='30'/></joint>"
        "<joint name='right' command='velocity'><motor device='mr'/><encoder device='er' ppr='70'/>"
        "<transmission gear_ratio='30'/></joint>"
        "<diff_drive name='base' left_joint='left' right_joint='right'>"
        "<geometry wheel_radius='0.0145' wheel_separation='0.0701282'/>"
        "<odometry frame='odom' base_frame='root'/></diff_drive></solar_os>"
        "<link name='root'/><link name='l'/><link name='r'/>"
        "<joint name='left' type='continuous'><parent link='root'/><child link='l'/></joint>"
        "<joint name='right' type='continuous'><parent link='root'/><child link='r'/></joint></robot>";
    m=parse(metadata);assert(solar_os_robot_model_info(m,&info)==OK && info.joints==2 && info.known_joints==0);
    char *stored=malloc(info.extension_bytes+1);assert(stored);
    assert(solar_os_robot_model_extension(m,stored,info.extension_bytes+1)==OK);
    assert(strstr(stored,"<motor device='m'/>") && strstr(stored,"<diff_drive"));
    assert(!memcmp(stored,strstr(metadata,"<solar_os"),info.extension_bytes));
    solar_os_robot_diff_drive_config_t drive;
    assert(solar_os_robot_model_diff_drive(m,0,&drive)==OK);
    assert(!strcmp(drive.name,"base") && !strcmp(drive.left_joint,"left") && !strcmp(drive.right_joint,"right"));
    near(drive.wheel_radius,.0145f);near(drive.wheel_separation,.0701282f);
    assert(!strcmp(drive.odom_frame,"odom") && !strcmp(drive.base_frame,"root") && drive.command_timeout_ms==300);
    assert(solar_os_robot_model_diff_drive(m,1,&drive)==MISSING);
    free(stored);solar_os_robot_model_destroy(m);
    const struct {const char *from,*to;} invalid_drive[]={
        {"wheel_radius='0.0145'","wheel_radius='0'"},
        {"wheel_separation='0.0701282'","wheel_separation='nan'"},
        {"left_joint='left'","left_joint='right'"},
        {"right_joint='right'","right_joint='missing'"},
        {"<geometry wheel_radius='0.0145' wheel_separation='0.0701282'/>",""},
        {"diff_drive name='base'","diff_drive name='base' bogus='1'"},
        {"frame='odom' base_frame='root'","frame='root' base_frame='root'"},
        {"</diff_drive>","<param name='typo' value='1'/></diff_drive>"},
        {"</diff_drive>","<param name='command_timeout_ms' value='10'/></diff_drive>"},
        {"</diff_drive>","<param name='command_timeout_ms' value='100'/><param name='command_timeout_ms' value='200'/></diff_drive>"},
    };
    for(unsigned i=0;i<sizeof(invalid_drive)/sizeof(invalid_drive[0]);++i) {
        const char *at=strstr(metadata,invalid_drive[i].from);assert(at);char invalid[2048];
        snprintf(invalid,sizeof(invalid),"%.*s%s%s",(int)(at-metadata),metadata,invalid_drive[i].to,at+strlen(invalid_drive[i].from));
        reject(invalid);
    }
    char deep[4096]="<robot name='x'><link name='r'/><gazebo>";
    for(unsigned i=0;i<40;++i)strcat(deep,"<a>");
    for(unsigned i=0;i<40;++i)strcat(deep,"</a>");
    strcat(deep,"</gazebo></robot>");reject(deep);
    char oversized[512]="<robot name='";
    for(unsigned i=0;i<SOLAR_OS_ROBOT_MODEL_NAME_MAX;++i)strcat(oversized,"n");
    strcat(oversized,"'><link name='r'/></robot>");reject(oversized);
}

static void solbot(const char *path)
{
    FILE *file=fopen(path,"rb");assert(file);assert(!fseek(file,0,SEEK_END));long bytes=ftell(file);assert(bytes>0);
    rewind(file);char *xml=malloc((size_t)bytes+1);assert(xml);
    assert(fread(xml,1,(size_t)bytes,file)==(size_t)bytes);xml[bytes]=0;fclose(file);
    solar_os_robot_model_t *m=parse(xml);free(xml);
    solar_os_robot_model_info_t info;assert(solar_os_robot_model_info(m,&info)==OK);
    assert(!strcmp(info.name,"solbot") && !strcmp(info.root,"base_footprint"));
    assert(info.links==6 && info.joints==5 && info.known_joints==3);
    solar_os_robot_transform_t t;
    assert(solar_os_robot_model_lookup(m,"base_footprint","caster_ball_link",&t)==OK);
    near(t.translation[0],-.0747f);near(t.translation[2],.006f);
    assert(solar_os_robot_model_lookup(m,"base_link","left_wheel_link",&t)==STATE);
    const char *names[]={"left_wheel_joint","right_wheel_joint"};float q[]={0,0};
    assert(solar_os_robot_model_set_positions(m,names,q,2)==OK);
    assert(solar_os_robot_model_lookup(m,"right_wheel_link","left_wheel_link",&t)==OK);
    near(t.translation[1],.0701282f);
    q[0]=1.5707963267948966f;
    assert(solar_os_robot_model_set_positions(m,names,q,2)==OK);
    assert(solar_os_robot_model_lookup(m,"base_footprint","left_wheel_link",&t)==OK);
    float v[]={1,0,0};assert(solar_os_robot_transform_apply(&t,v,1,v)==OK);
    near(v[0],0);near(v[1],0);near(v[2],-1);
    solar_os_robot_model_destroy(m);assert(!allocations);
}

static void binding_validation(void)
{
    const char *prefix="<robot name='bound'><link name='r'/><link name='w'/>"
        "<joint name='wheel' type='continuous'><parent link='r'/><child link='w'/></joint>"
        "<solar_os version='1'><joint name='wheel' command='velocity'>";
    const char *suffix="</joint></solar_os></robot>";
    const char *valid="<motor device='left'/><encoder device='enc' ppr='70' placement='output'/>"
        "<transmission gear_ratio='30'/>";
    const char *bad[]={
        "<motor device='left'/><encoder device='enc'/><transmission gear_ratio='30'/>",
        "<motor device='left'/><encoder device='enc' ppr='70'/>",
        "<motor device='left' typo='1'/><encoder device='enc' ppr='70'/><transmission gear_ratio='30'/>",
        "<motor device='left'/><motor device='other'/><encoder device='enc' ppr='70'/><transmission gear_ratio='30'/>",
        "<motor device='left'/><encoder device='enc' ppr='70' placement='wheel'/><transmission gear_ratio='30'/>"};
    char xml[2048];
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) { snprintf(xml,sizeof(xml),"%s%s%s",prefix,bad[i],suffix);reject(xml); }
    const char *params[]={"<param name='kp' value='nan'/>","<param name='typo' value='1'/>",
        "<param name='period_ms' value='2'/>","<param name='ppr' value='2'/>",
        "<param name='kp' value='1'/><param name='kp' value='2'/>",
        "<param name='kd' value='1'/><param name='tau' value='0'/>",
        "<param name='max_drive' value='0.1'/><param name='friction_bias_forward' value='0.2'/>",
        "<param name='position_min' value='10'/>"};
    for(unsigned i=0;i<sizeof(params)/sizeof(params[0]);++i) {
        snprintf(xml,sizeof(xml),"%s%s%s%s",prefix,valid,params[i],suffix);reject(xml);
    }
    /* Final validation permits parameters in any order. */
    snprintf(xml,sizeof(xml),"%s%s<param name='friction_bias_forward' value='0.3'/>"
        "<param name='max_drive' value='0.4'/>%s",prefix,valid,suffix);
    solar_os_robot_model_t *m=parse(xml);
    solar_os_robot_joint_config_v3_t c;
    assert(solar_os_robot_model_binding(m,0,&c)==OK && c.encoder_on_output && c.friction_bias_forward==0.3f);
    assert(solar_os_robot_model_binding(m,1,&c)==MISSING);
    solar_os_robot_model_destroy(m);
    snprintf(xml,sizeof(xml),"%s%s<param name='kd' value='0.005'/>"
        "<param name='tau' value='0.02'/>%s",prefix,valid,suffix);
    m=parse(xml);
    assert(solar_os_robot_model_binding(m,0,&c)==OK);
    near(c.kd,.005f);near(c.derivative_tau,.02f);
    solar_os_robot_model_destroy(m);
}

static void device_frames(void)
{
    const char *prefix="<robot name='s'><solar_os version='1'>";
    const char *suffix="</solar_os><link name='r'/><link name='imu'/><joint name='mount' type='fixed'>"
        "<parent link='r'/><child link='imu'/><origin xyz='0 0 0.02'/></joint></robot>";
    char xml[4096];
    snprintf(xml,sizeof(xml),"%s<device name='imu0' frame='imu'/>%s",prefix,suffix);
    solar_os_robot_model_t *m=parse(xml); solar_os_robot_model_device_t device;
    char frame[64]; solar_os_robot_transform_t t;
    assert(solar_os_robot_model_device(m,0,&device)==OK && !strcmp(device.name,"imu0") && !strcmp(device.frame,"imu"));
    assert(solar_os_robot_model_device(m,1,&device)==MISSING);
    assert(solar_os_robot_model_device_frame(m,"imu0",frame,sizeof(frame))==OK && !strcmp(frame,"imu"));
    assert(solar_os_robot_model_lookup(m,"r",frame,&t)==OK);near(t.translation[2],.02f);near(t.rotation[3],1);
    assert(solar_os_robot_model_device_frame(m,"missing",frame,sizeof(frame))==MISSING);
    assert(solar_os_robot_model_device_frame(m,"imu0",frame,1)==ARG);
    solar_os_robot_model_destroy(m);
    const char *bad[]={"<device name='imu0'/>","<device name='imu0' frame='absent'/>",
        "<device name='imu0' frame='imu' typo='1'/>","<device name='' frame='imu'/>",
        "<device name='imu0' frame='imu'/><device name='imu0' frame='r'/>",
        "<device name='imu0' frame='imu'><child/></device>"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        snprintf(xml,sizeof(xml),"%s%s%s",prefix,bad[i],suffix);reject(xml);
    }
    snprintf(xml,sizeof(xml),"%s<device name='imu0' frame='imu'/><device name='camera0' frame='imu'/>%s",prefix,suffix);
    m=parse(xml);assert(solar_os_robot_model_device(m,1,&device)==OK);solar_os_robot_model_destroy(m);
}

static void parser_diagnostics(void)
{
    const char *xml="<robot name='bound'>\r\n"
        "  <link name='r'/><link name='w'/>\r\n"
        "  <joint name='wheel' type='continuous'><parent link='r'/><child link='w'/></joint>\r\n"
        "  <solar_os version='1'><joint name='wheel' command='velocity'>\r\n"
        "    <motor device='left'/><encoder device='enc' ppr='70'/><transmission gear_ratio='30'/>\r\n"
        "    <param name='kd' valule='0.005'/>\r\n"
        "  </joint></solar_os></robot>";
    char error[128];solar_os_robot_model_t *m=NULL;
    assert(solar_os_robot_model_parse(&memory,xml,strlen(xml),&m,error,sizeof(error))==ARG);
    assert(!m && !allocations);
    assert(!strcmp(error,"line 6, column 5: Unknown param attribute 'valule' on <param name='kd'>"));
    puts(error);
    /* Diagnostics are optional and always bounded, including a one-byte buffer. */
    for(size_t n=0;n<=strlen(error);++n) {
        char small[128];memset(small,'X',sizeof(small));
        assert(solar_os_robot_model_parse(&memory,xml,strlen(xml),&m,small,n)==ARG);
        assert(!m && !allocations && small[n]=='X');
        if(n) assert(small[n-1]==0);
    }
    assert(solar_os_robot_model_parse(&memory,xml,strlen(xml),&m,NULL,0)==ARG);
    /* General syntax failures identify their opening token, too. */
    const char *bad="<robot name='x'>\n  <link name='r' name='s'/></robot>";
    assert(solar_os_robot_model_parse(&memory,bad,strlen(bad),&m,error,sizeof(error))==ARG);
    assert(!strcmp(error,"line 2, column 3: Duplicate XML attribute"));
}

int main(int argc,char **argv)
{
    transform_math();kinematics();parser_validation();binding_validation();device_frames();parser_diagnostics();
    solbot(argc>1?argv[1]:"../fixtures/solbot.urdf");
    assert(!allocations);puts("robot model: transform math, URDF validation, unknown state, atomic batches, OOM and Solbot passed");
    return 0;
}
