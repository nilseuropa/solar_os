#include "solar_os_controller_tuning.h"
#include <float.h>
#include <math.h>
#include <string.h>

static bool failed(solar_os_controller_tuner_t *t, uint32_t reason)
{
    t->result.state=SOLAR_OS_CONTROLLER_TUNE_FAILED;
    t->result.reason=reason;
    t->result.kp=t->result.ki=t->result.kd=t->result.derivative_tau=0;
    return false;
}

#include "solar_os_controller_autotuning.inc"

bool solar_os_controller_tuner_init(solar_os_controller_tuner_t *t,
    const solar_os_controller_tune_config_t *c)
{
    if(!t || !c || !isfinite(c->setpoint) || !isfinite(c->low) || !isfinite(c->high) || c->low>=c->high ||
        !isfinite(c->band) || c->band<0 || (!c->automatic && c->band==0) || !isfinite(c->max_dt) || c->max_dt<=0 ||
        !isfinite(c->timeout) || c->timeout<=0 || c->cycles<3 || c->cycles>SOLAR_OS_CONTROLLER_TUNE_CYCLES_MAX)
        return false;
    if(c->automatic && (!isfinite(c->safe) || c->safe<c->low || c->safe>c->high ||
        !isfinite(c->measurement_min) || !isfinite(c->measurement_max) || c->measurement_min>=c->measurement_max)) return false;
    *t=(solar_os_controller_tuner_t){.config=*c,.last_rising=-1};
    t->result.state=SOLAR_OS_CONTROLLER_TUNE_RUNNING;
    t->result.required_cycles=c->cycles;
    if(c->automatic) {
        t->result.phase=SOLAR_OS_CONTROLLER_TUNE_BASELINE;
        t->result.points=c->cycles;t->result.drive=c->safe;
        /* Exercise one direction at a time. Prefer increasing actuator output,
         * or decreasing output when safe is already at its upper bound. */
        t->direction=c->safe<c->high?1:-1;
        t->limit=t->direction>0?c->high:c->low;
    }
    return true;
}

static bool converged(solar_os_controller_tuner_t *t)
{
    const unsigned n=t->config.cycles;
    double period=0, amplitude=0;
    for(unsigned i=0;i<n;++i) { period+=t->periods[i]; amplitude+=t->amplitudes[i]; }
    period/=n; amplitude/=n;
    /* Reject aliased cycles and oscillations dominated by the relay band.
     * A broad consistency check permits sensor noise but rejects drift. */
    if(period<8*t->largest_dt || amplitude<3*t->config.band) return false;
    for(unsigned i=0;i<n;++i)
        if(fabs(t->periods[i]-period)>.2*period || fabs(t->amplitudes[i]-amplitude)>.2*amplitude) return false;
    double d=((double)t->config.high-t->config.low)/2;
    double ku=4*d/(3.14159265358979323846*amplitude);
    /* Relay describing-function estimate (Astrom/Hagglund), followed by
     * classic Ziegler-Nichols PID in parallel Kp/Ki/Kd form. These are reported
     * candidates, never automatically installed in the running controller. */
    double kp=.6*ku, ki=1.2*ku/period, kd=.075*ku*period;
    double tau=fmax(period/80,t->largest_dt);
    if(!isfinite(ku) || !isfinite(kp) || !isfinite(ki) || !isfinite(kd) ||
        ku>FLT_MAX || kp>FLT_MAX || ki>FLT_MAX || kd>FLT_MAX || ku<=0 || tau>FLT_MAX)
        return failed(t,SOLAR_OS_CONTROLLER_TUNE_BAD_SAMPLE);
    t->result.period=(float)period; t->result.amplitude=(float)amplitude;
    t->result.ultimate_gain=(float)ku;
    t->result.kp=(float)kp; t->result.ki=(float)ki; t->result.kd=(float)kd;
    t->result.derivative_tau=(float)tau;
    if(t->result.kp<=0 || t->result.ki<=0 || t->result.kd<=0 || t->result.derivative_tau<=0)
        return failed(t,SOLAR_OS_CONTROLLER_TUNE_BAD_SAMPLE);
    t->result.state=SOLAR_OS_CONTROLLER_TUNE_COMPLETE;
    return true;
}

bool solar_os_controller_tuner_step(solar_os_controller_tuner_t *t,
    float measurement, float dt, float *output)
{
    if(!t || !output || t->result.state!=SOLAR_OS_CONTROLLER_TUNE_RUNNING) return false;
    if(!isfinite(measurement) || !isfinite(dt) || dt<=0 || dt>t->config.max_dt)
        return failed(t,SOLAR_OS_CONTROLLER_TUNE_BAD_SAMPLE);
    t->elapsed+=dt; t->result.elapsed=(float)t->elapsed; t->result.samples++;
    if(t->elapsed>=t->config.timeout)
        return failed(t,!t->config.automatic && t->result.cycles>=t->config.cycles?SOLAR_OS_CONTROLLER_TUNE_UNSTABLE:SOLAR_OS_CONTROLLER_TUNE_TIMEOUT);
    if(dt>t->largest_dt) t->largest_dt=dt;
    if(t->config.automatic) return automatic_step(t,measurement,dt,output);
    double value=((double)measurement-t->config.setpoint)*(t->config.reverse?-1:1);
    if(!t->initialized) {
        t->initialized=true; t->high=value<0;
        t->minimum=t->maximum=value;
    }
    if(value<t->minimum) t->minimum=value;
    if(value>t->maximum) t->maximum=value;
    if(t->high && value>=t->config.band) {
        t->high=false;
        if(t->last_rising>=0 && t->saw_low) {
            if(t->warmup<2) t->warmup++;
            else {
                unsigned index=t->next++%t->config.cycles;
                t->periods[index]=(float)(t->elapsed-t->last_rising);
                t->amplitudes[index]=(float)((t->maximum-t->minimum)/2);
                if(t->result.cycles<t->config.cycles) t->result.cycles++;
                if(t->result.cycles==t->config.cycles && converged(t)) return true;
                if(t->result.state==SOLAR_OS_CONTROLLER_TUNE_FAILED) return false;
            }
        }
        t->last_rising=t->elapsed; t->saw_low=false;
        t->minimum=t->maximum=value;
    } else if(!t->high && value<=-t->config.band) {
        t->high=true; t->saw_low=true;
    }
    *output=t->high?t->config.high:t->config.low;
    return true;
}

const char *solar_os_controller_tune_state_name(uint32_t state)
{
    switch(state) {
    case SOLAR_OS_CONTROLLER_TUNE_RUNNING: return "running";
    case SOLAR_OS_CONTROLLER_TUNE_COMPLETE: return "complete";
    case SOLAR_OS_CONTROLLER_TUNE_ABORTED: return "aborted";
    case SOLAR_OS_CONTROLLER_TUNE_FAILED: return "failed";
    default: return "idle";
    }
}
const char *solar_os_controller_tune_reason_name(uint32_t reason)
{
    switch(reason) {
    case SOLAR_OS_CONTROLLER_TUNE_TIMEOUT: return "timeout";
    case SOLAR_OS_CONTROLLER_TUNE_UNSTABLE: return "unstable-oscillation";
    case SOLAR_OS_CONTROLLER_TUNE_BAD_SAMPLE: return "invalid-sample";
    case SOLAR_OS_CONTROLLER_TUNE_CLEANUP: return "cleanup-failed";
    case SOLAR_OS_CONTROLLER_TUNE_NO_MOTION: return "no-motion-within-output-limit";
    case SOLAR_OS_CONTROLLER_TUNE_NO_HEADROOM: return "insufficient-output-headroom";
    case SOLAR_OS_CONTROLLER_TUNE_WRONG_DIRECTION: return "wrong-feedback-direction";
    case SOLAR_OS_CONTROLLER_TUNE_RANGE: return "measurement-outside-test-range";
    case SOLAR_OS_CONTROLLER_TUNE_NOT_SETTLED: return "response-not-settled";
    case SOLAR_OS_CONTROLLER_TUNE_VALIDATION: return "candidate-validation-failed";
    default: return "none";
    }
}

const char *solar_os_controller_tune_phase_name(uint32_t phase)
{
    switch(phase) {
    case SOLAR_OS_CONTROLLER_TUNE_BASELINE:return "baseline";
    case SOLAR_OS_CONTROLLER_TUNE_FIND_MOTION:return "finding-motion";
    case SOLAR_OS_CONTROLLER_TUNE_SETTLE:return "settling";
    case SOLAR_OS_CONTROLLER_TUNE_IDENTIFY:return "identifying";
    case SOLAR_OS_CONTROLLER_TUNE_VALIDATE:return "validating";
    case SOLAR_OS_CONTROLLER_TUNE_DONE:return "done";
    default:return "unknown";
    }
}
