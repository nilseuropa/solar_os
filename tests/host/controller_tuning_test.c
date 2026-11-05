#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "solar_os_controller_tuning.h"
#include "solar_os_controller_math.h"

static void plant_test(bool reverse, bool noise)
{
    solar_os_controller_tuner_t t;
    solar_os_controller_tune_config_t c={.low=-1,.high=1,.band=.01f,.max_dt=.05f,.timeout=30,.cycles=5,.reverse=reverse};
    assert(solar_os_controller_tuner_init(&t,&c));
    float delay[35]={0}, y=0, output=0;
    unsigned cursor=0;
    for(unsigned i=0;i<4000 && t.result.state==SOLAR_OS_CONTROLLER_TUNE_RUNNING;++i) {
        float measurement=y+(noise?.002f*sinf((float)i*1.7f):0);
        assert(solar_os_controller_tuner_step(&t,measurement,.01f,&output));
        if(t.result.state==SOLAR_OS_CONTROLLER_TUNE_COMPLETE) break;
        assert(output>=-1 && output<=1);
        y+=((reverse?-1:1)*delay[cursor]-y)*(.01f/.25f);
        delay[cursor]=output; cursor=(cursor+1)%35;
    }
    assert(t.result.state==SOLAR_OS_CONTROLLER_TUNE_COMPLETE);
    assert(t.result.kp>0 && t.result.ki>0 && t.result.kd>0 && t.result.derivative_tau>0);
    assert(t.result.period>.5f && t.result.period<3 && t.result.amplitude>.1f);
    /* Exercise the returned parallel gains against the same delayed plant. */
    solar_os_controller_math_config_t pid={.kind=SOLAR_OS_CONTROLLER_PID,
        .kp=t.result.kp,.ki=t.result.ki,.kd=t.result.kd,.derivative_tau=t.result.derivative_tau,
        .output_min=-1,.output_max=1,.max_dt=.05f,.hysteresis=1,.reverse=reverse};
    solar_os_controller_math_state_t state;
    assert(solar_os_controller_math_reset(&pid,&state,0));
    for(unsigned i=0;i<2000;++i) {
        assert(solar_os_controller_math_step(&pid,&state,.5f,y,.01f));
        y+=((reverse?-1:1)*delay[cursor]-y)*(.01f/.25f);
        delay[cursor]=state.output; cursor=(cursor+1)%35;
        assert(isfinite(y) && fabsf(y)<2);
    }
    assert(fabsf(y-.5f)<.03f);
}

/* Motor-like dead zone, timestamp jitter, quantized/noisy feedback and an
 * actuator delay. No user setpoint or useful initial PID gains are supplied. */
static void automatic_plant_test(bool reverse,bool negative,bool noisy,float period)
{
    solar_os_controller_tuner_t t;
    solar_os_controller_tune_config_t c={.low=negative?-1:0,.high=negative?0:1,.safe=0,
        .band=0,.max_dt=.1f,.timeout=120,.cycles=4,.reverse=reverse,.automatic=true,
        .measurement_min=-10,.measurement_max=10};
    assert(solar_os_controller_tuner_init(&t,&c));
    float delay[13]={0},y=0,output=0;
    unsigned cursor=0,points_seen=0;bool validation_seen=false;
    for(unsigned i=0;i<30000 && t.result.state==SOLAR_OS_CONTROLLER_TUNE_RUNNING;++i) {
        float dt=noisy?period*(.9f+.1f*(i%3)):period;
        float sample=noisy?roundf(y*1000)/1000+.002f*sinf(i*1.7f):y;
        bool ok=solar_os_controller_tuner_step(&t,sample,dt,&output);
        if(!ok) fprintf(stderr,"automatic failure: %s phase=%s t=%g y=%g drive=%g point=%u\n",
            solar_os_controller_tune_reason_name(t.result.reason),solar_os_controller_tune_phase_name(t.result.phase),
            t.result.elapsed,y,output,t.result.point);
        assert(ok);
        assert(output>=c.low && output<=c.high);
        if(t.result.phase==SOLAR_OS_CONTROLLER_TUNE_IDENTIFY) points_seen|=1u<<t.result.point;
        if(t.result.phase==SOLAR_OS_CONTROLLER_TUNE_VALIDATE) validation_seen=true;
        float u=delay[cursor],speed=fabsf(u)>.2f?copysignf((fabsf(u)-.2f)*4,u):0;
        y+=((reverse?-speed:speed)-y)*(dt/.25f);
        delay[cursor]=output;cursor=(cursor+1)%13;
    }
    assert(t.result.state==SOLAR_OS_CONTROLLER_TUNE_COMPLETE);
    assert(points_seen==15 && validation_seen && t.result.point==4);
    assert(t.result.kp>0 && t.result.ki>0 && t.result.kd>=0 && t.result.derivative_tau>0);
    assert(fabsf(t.result.breakaway)>.2f && fabsf(t.result.breakaway)<.3f);
    assert(output==0 && !c.setpoint);
}

static void tortuga_fit_test(void)
{
    solar_os_controller_tuner_t t;
    solar_os_controller_tune_config_t c={.low=0,.high=1,.max_dt=.1f,.timeout=120,.cycles=4,
        .automatic=true,.measurement_min=-10,.measurement_max=10};
    assert(solar_os_controller_tuner_init(&t,&c));
    t.result.phase=SOLAR_OS_CONTROLLER_TUNE_IDENTIFY;t.threshold=.02f;t.step_drive=.5f;
    float output;
    for(unsigned i=1;i<=251 && !t.valid_steps;++i) {
        float time=i*.02f,y=time<.2f?0:1-expf(-(time-.2f)/.3f);
        assert(solar_os_controller_tuner_step(&t,y,.02f,&output));
    }
    /* Tortuga's 10/63.2 fit for a gain-2, delay-.2, lag-.3 step response.
     * Its delay/lag are intentionally the original heuristic estimates. */
    assert(t.valid_steps==1 && fabsf(t.result.process_gain-2)<.002f);
    assert(fabsf(t.result.dead_time-.2316f)<.001f);
    assert(fabsf(t.result.time_constant-.2682f)<.002f);
    assert(fabsf(t.kp_sum-.5f)<.001f && fabsf(t.ki_sum-1.302f)<.01f);
    assert(fabsf(t.kd_sum-.0404f)<.001f);
}

static void automatic_failure_test(void)
{
    solar_os_controller_tuner_t t;
    solar_os_controller_tune_config_t c={.low=0,.high=.2f,.max_dt=.1f,.timeout=120,.cycles=4,
        .automatic=true,.measurement_min=-4,.measurement_max=4};
    assert(solar_os_controller_tuner_init(&t,&c));float output=0;
    for(unsigned i=0;i<6500 && t.result.state==SOLAR_OS_CONTROLLER_TUNE_RUNNING;++i)
        (void)solar_os_controller_tuner_step(&t,0,.02f,&output);
    assert(t.result.reason==SOLAR_OS_CONTROLLER_TUNE_NO_MOTION && !t.result.kp);
    assert(solar_os_controller_tuner_init(&t,&c));
    assert(!solar_os_controller_tuner_step(&t,5,.02f,&output));
    assert(t.result.reason==SOLAR_OS_CONTROLLER_TUNE_RANGE);
    c.timeout=.5f;assert(solar_os_controller_tuner_init(&t,&c));
    for(unsigned i=0;i<50 && t.result.state==SOLAR_OS_CONTROLLER_TUNE_RUNNING;++i)
        (void)solar_os_controller_tuner_step(&t,0,.02f,&output);
    assert(t.result.reason==SOLAR_OS_CONTROLLER_TUNE_TIMEOUT && !t.result.kp);
    c.safe=NAN;assert(!solar_os_controller_tuner_init(&t,&c));
    c.safe=0;c.high=1;c.timeout=120;
    assert(solar_os_controller_tuner_init(&t,&c));
    for(unsigned i=0;i<300 && t.result.state==SOLAR_OS_CONTROLLER_TUNE_RUNNING;++i)
        (void)solar_os_controller_tuner_step(&t,-output,.02f,&output);
    assert(t.result.reason==SOLAR_OS_CONTROLLER_TUNE_WRONG_DIRECTION && !t.result.kp);
    assert(solar_os_controller_tuner_init(&t,&c));
    t.result.phase=SOLAR_OS_CONTROLLER_TUNE_IDENTIFY;t.threshold=.01f;t.step_drive=.5f;
    for(unsigned i=1;i<=260 && t.result.state==SOLAR_OS_CONTROLLER_TUNE_RUNNING;++i)
        (void)solar_os_controller_tuner_step(&t,i*.002f,.02f,&output);
    assert(t.result.reason==SOLAR_OS_CONTROLLER_TUNE_NOT_SETTLED && !t.result.kp);
}

int main(void)
{
    plant_test(false,false); plant_test(true,false); plant_test(false,true);
    automatic_plant_test(false,false,false,.02f);automatic_plant_test(true,false,true,.02f);
    automatic_plant_test(false,true,true,.02f);automatic_plant_test(false,false,true,.005f);
    automatic_failure_test();
    tortuga_fit_test();
    solar_os_controller_tuner_t t;
    solar_os_controller_tune_config_t c={.setpoint=1,.low=0,.high=1,.band=.01f,.max_dt=.1f,.timeout=2,.cycles=3};
    assert(solar_os_controller_tuner_init(&t,&c));
    float output=0;
    for(unsigned i=0;i<300 && t.result.state==SOLAR_OS_CONTROLLER_TUNE_RUNNING;++i)
        (void)solar_os_controller_tuner_step(&t,0,.01f,&output);
    assert(t.result.state==SOLAR_OS_CONTROLLER_TUNE_FAILED && t.result.reason==SOLAR_OS_CONTROLLER_TUNE_TIMEOUT && !t.result.kp);
    assert(solar_os_controller_tuner_init(&t,&c));
    assert(!solar_os_controller_tuner_step(&t,NAN,.01f,&output));
    assert(t.result.reason==SOLAR_OS_CONTROLLER_TUNE_BAD_SAMPLE);
    assert(solar_os_controller_tuner_init(&t,&c));
    assert(!solar_os_controller_tuner_step(&t,0,.2f,&output));
    c.setpoint=0;c.low=-1;c.timeout=15;
    assert(solar_os_controller_tuner_init(&t,&c));
    for(unsigned i=0;i<1600 && t.result.state==SOLAR_OS_CONTROLLER_TUNE_RUNNING;++i) {
        float amplitude=(i/100)%2?.4f:.8f;
        float y=amplitude*sinf(6.283185307f*i*.01f);
        (void)solar_os_controller_tuner_step(&t,y,.01f,&output);
    }
    assert(t.result.state==SOLAR_OS_CONTROLLER_TUNE_FAILED && t.result.reason==SOLAR_OS_CONTROLLER_TUNE_UNSTABLE && !t.result.kp);
    c.low=c.high;assert(!solar_os_controller_tuner_init(&t,&c));
    puts("controller tuning: delayed/reverse/noisy plants, closed-loop candidates, timeout, instability and invalid samples passed");
}
