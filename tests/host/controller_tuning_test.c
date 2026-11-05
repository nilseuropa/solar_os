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

int main(void)
{
    plant_test(false,false); plant_test(true,false); plant_test(false,true);
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
