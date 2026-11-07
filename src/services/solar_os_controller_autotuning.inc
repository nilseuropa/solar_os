/* Automatic experiment and step-response fit adapted from Nils' working
 * tortuga_controller2/lib/pid_tuner and src/main.cpp autotune sequence.
 * Inputs/outputs retain their OS-native units: no wheel geometry or scaling.
 * Candidate gains are exercised privately, never installed in the controller. */

static void automatic_phase(solar_os_controller_tuner_t *t,uint32_t phase)
{
    t->result.phase=phase;t->phase_started=t->elapsed;
    t->window_time=t->window_sum=t->window_square=0;t->window_count=0;
    t->count=0;t->stable_windows=0;
}

static void automatic_window(solar_os_controller_tuner_t *t,float y,float dt)
{
    t->window_time+=dt;t->window_sum+=y;t->window_square+=(double)y*y;t->window_count++;
}

static float automatic_mean(const solar_os_controller_tuner_t *t)
{ return t->window_count?(float)(t->window_sum/t->window_count):t->baseline; }

static void automatic_clear_window(solar_os_controller_tuner_t *t)
{ t->window_time=t->window_sum=t->window_square=0;t->window_count=0; }

static bool automatic_crossing(const solar_os_controller_tuner_t *t,float delta,float fraction,float *time)
{
    float previous=0,previous_time=0,sign=delta>0?1:-1,target=fabsf(delta)*fraction;
    for(unsigned i=0;i<t->count;++i) {
        float value=(t->values[i]-t->initial)*sign;
        if(value>=target && value>previous) {
            *time=previous_time+(target-previous)/(value-previous)*(t->times[i]-previous_time);
            return isfinite(*time);
        }
        previous=value;previous_time=t->times[i];
    }
    return false;
}

static bool automatic_fit(solar_os_controller_tuner_t *t)
{
    /* Average the last second, exactly as Tortuga's steady_window_s=1.
     * Unlike the original, reject a tail that is still visibly drifting. */
    double sums[2]={0};unsigned counts[2]={0};
    for(unsigned i=0;i<t->count;++i) if(t->times[i]>=4) {
        unsigned half=t->times[i]>=4.5f;
        sums[half]+=t->values[i];counts[half]++;
    }
    if(!counts[0] || !counts[1]) return failed(t,SOLAR_OS_CONTROLLER_TUNE_BAD_SAMPLE);
    float first=(float)(sums[0]/counts[0]),last=(float)(sums[1]/counts[1]);
    float final=(float)((sums[0]+sums[1])/(counts[0]+counts[1]));
    float delta=final-t->initial;
    if(fabsf(delta)<3*t->threshold) return failed(t,SOLAR_OS_CONTROLLER_TUNE_NO_HEADROOM);
    if(delta*t->direction*(t->config.reverse?-1:1)<=0)
        return failed(t,SOLAR_OS_CONTROLLER_TUNE_WRONG_DIRECTION);
    if(fabsf(last-first)>fmaxf(2*t->threshold,.05f*fabsf(delta)))
        return failed(t,SOLAR_OS_CONTROLLER_TUNE_NOT_SETTLED);
    float t10,t63;
    if(!automatic_crossing(t,delta,.1f,&t10) || !automatic_crossing(t,delta,.632f,&t63) ||
        t63-t10<t->largest_dt) return failed(t,SOLAR_OS_CONTROLLER_TUNE_BAD_SAMPLE);
    double gain=fabs((double)delta/(t->step_drive-t->config.safe));
    double dead=t10,tau=t63-t10,lambda=fmax(tau,4*t->largest_dt);
    /* Tortuga's IMC PID rule, in the parallel Kp/Ki/Kd form used by SolarOS. */
    double kp=(tau+.5*dead)/(gain*(lambda+.5*dead));
    double ki=kp/(tau+.5*dead),kd=kp*tau*dead/(2*tau+dead);
    if(!isfinite(kp) || !isfinite(ki) || !isfinite(kd) || kp<=0 || ki<=0 || kd<0 ||
        kp>FLT_MAX/t->config.cycles || ki>FLT_MAX/t->config.cycles || kd>FLT_MAX/t->config.cycles)
        return failed(t,SOLAR_OS_CONTROLLER_TUNE_BAD_SAMPLE);
    t->kp_sum+=(float)kp;t->ki_sum+=(float)ki;t->kd_sum+=(float)kd;t->valid_steps++;
    /* Leave actuator authority for feedback corrections. A target equal to
     * the speed at maximum drive cannot reject even small disturbances. */
    t->step_targets[t->result.point]=t->baseline+.8f*(final-t->baseline);
    t->result.process_gain=(float)gain;t->result.dead_time=(float)dead;t->result.time_constant=(float)tau;
    t->result.derivative_tau=fmaxf(t->result.derivative_tau,fmaxf(t->largest_dt,(float)dead/10));
    t->result.cycles=t->valid_steps;
    return true;
}

static solar_os_controller_math_config_t automatic_candidate(const solar_os_controller_tuner_t *t)
{
    solar_os_controller_math_config_t c=solar_os_controller_math_defaults(SOLAR_OS_CONTROLLER_PID);
    c.kp=t->candidate_kp;c.ki=t->candidate_ki;c.kd=t->candidate_kd;
    c.derivative_tau=t->result.derivative_tau;c.output_min=t->config.low;c.output_max=t->config.high;
    c.reverse=t->config.reverse;c.max_dt=t->config.max_dt;
    return c;
}

static bool automatic_step(solar_os_controller_tuner_t *t,float y,float dt,float *output)
{
    const solar_os_controller_tune_config_t *c=&t->config;
    float age=(float)(t->elapsed-t->phase_started);
    float margin=fmaxf(c->band,3*t->result.noise);
    if((double)y<(double)c->measurement_min-margin || (double)y>(double)c->measurement_max+margin)
        return failed(t,SOLAR_OS_CONTROLLER_TUNE_RANGE);
    switch(t->result.phase) {
    case SOLAR_OS_CONTROLLER_TUNE_BASELINE:
        t->result.drive=c->safe;automatic_window(t,y,dt);
        if(age>=1 && t->window_count>=4) {
            t->baseline=automatic_mean(t);
            double variance=fmax(0,t->window_square/t->window_count-(double)t->baseline*t->baseline);
            t->result.noise=(float)sqrt(variance);
            t->threshold=fmaxf(c->band>0?c->band:.02f,6*t->result.noise);
            t->result.test_setpoint=t->baseline;
            /* Choose only a direction allowed by the measurement envelope. */
            float sign=t->direction*(c->reverse?-1:1);
            if((sign>0 && (double)c->measurement_max-t->baseline<6*t->threshold) ||
                (sign<0 && (double)t->baseline-c->measurement_min<6*t->threshold)) {
                t->direction=-t->direction;t->limit=t->direction>0?c->high:c->low;
                if((t->limit-c->safe)*t->direction<=0) return failed(t,SOLAR_OS_CONTROLLER_TUNE_NO_HEADROOM);
            }
            automatic_phase(t,SOLAR_OS_CONTROLLER_TUNE_FIND_MOTION);
            t->result.drive=c->safe+.02f*(t->limit-c->safe);
        }
        break;
    case SOLAR_OS_CONTROLLER_TUNE_FIND_MOTION:
        automatic_window(t,y,dt);
        if(age>=1 && t->window_count>=4) {
            float change=automatic_mean(t)-t->baseline;
            if(fabsf(change)>=t->threshold) {
                if(change*t->direction*(c->reverse?-1:1)<=0) return failed(t,SOLAR_OS_CONTROLLER_TUNE_WRONG_DIRECTION);
                t->result.breakaway=t->result.drive;
                if(fabsf(t->limit-t->result.drive)<.02f*fabsf(t->limit-c->safe))
                    return failed(t,SOLAR_OS_CONTROLLER_TUNE_NO_HEADROOM);
                t->result.drive=c->safe;
                automatic_phase(t,SOLAR_OS_CONTROLLER_TUNE_SETTLE);
            } else {
                if(t->result.drive==t->limit) return failed(t,SOLAR_OS_CONTROLLER_TUNE_NO_MOTION);
                float next=t->result.drive+.02f*(t->limit-c->safe);
                t->result.drive=t->direction>0?fminf(next,t->limit):fmaxf(next,t->limit);
                automatic_phase(t,SOLAR_OS_CONTROLLER_TUNE_FIND_MOTION);
            }
        }
        break;
    case SOLAR_OS_CONTROLLER_TUNE_SETTLE:
        t->result.drive=c->safe;automatic_window(t,y,dt);
        if(t->window_time>=.5f && t->window_count>=4) {
            float mean=automatic_mean(t);
            if(age>=1 && fabsf(mean-t->baseline)<=t->threshold) {
                if(t->valid_steps==c->cycles) {
                    t->candidate_kp=t->kp_sum/t->valid_steps;t->candidate_ki=t->ki_sum/t->valid_steps;
                    t->candidate_kd=t->kd_sum/t->valid_steps;
                    solar_os_controller_math_config_t pid=automatic_candidate(t);
                    if(!solar_os_controller_math_reset(&pid,&t->validation,c->safe)) return failed(t,SOLAR_OS_CONTROLLER_TUNE_BAD_SAMPLE);
                    t->validation.i=c->safe;
                    automatic_phase(t,SOLAR_OS_CONTROLLER_TUNE_VALIDATE);
                    t->result.point=0;t->result.test_setpoint=t->step_targets[0];
                } else {
                    t->initial=mean;
                    t->step_drive=t->result.breakaway+(t->limit-t->result.breakaway)*(t->result.point+1)/c->cycles;
                    t->result.drive=t->step_drive;
                    automatic_phase(t,SOLAR_OS_CONTROLLER_TUNE_IDENTIFY);
                }
            } else if(age>=5) return failed(t,SOLAR_OS_CONTROLLER_TUNE_NOT_SETTLED);
            automatic_clear_window(t);
        }
        break;
    case SOLAR_OS_CONTROLLER_TUNE_IDENTIFY:
        /* Keep a bounded history covering all five seconds even at >50 Hz.
         * A full buffer never truncates the response window. */
        if(t->count<SOLAR_OS_CONTROLLER_TUNE_SAMPLES_MAX &&
            age>=(float)t->count*5/(SOLAR_OS_CONTROLLER_TUNE_SAMPLES_MAX-1)) {
            t->times[t->count]=age;t->values[t->count++]=y;
        }
        if(age>=5) {
            if(!automatic_fit(t)) return false;
            t->result.point++;t->result.drive=c->safe;
            automatic_phase(t,SOLAR_OS_CONTROLLER_TUNE_SETTLE);
        }
        break;
    case SOLAR_OS_CONTROLLER_TUNE_VALIDATE: {
        solar_os_controller_math_config_t pid=automatic_candidate(t);
        if(!solar_os_controller_math_step(&pid,&t->validation,t->result.test_setpoint,y,dt))
            return failed(t,SOLAR_OS_CONTROLLER_TUNE_BAD_SAMPLE);
        t->result.drive=t->validation.output;
        float tolerance=fmaxf(2*t->threshold,.05f*fabsf(t->result.test_setpoint-t->baseline));
        if(fabsf(y-t->result.test_setpoint)<=tolerance) t->window_time+=dt;
        else t->window_time=0;
        if(age>=1 && t->window_time>=1) {
            t->validation_point++;t->result.point=t->validation_point;
            if(t->validation_point==c->cycles) {
                t->result.kp=t->candidate_kp;t->result.ki=t->candidate_ki;t->result.kd=t->candidate_kd;
                t->result.state=SOLAR_OS_CONTROLLER_TUNE_COMPLETE;
                t->result.phase=SOLAR_OS_CONTROLLER_TUNE_DONE;t->result.drive=c->safe;
            } else {
                automatic_phase(t,SOLAR_OS_CONTROLLER_TUNE_VALIDATE);
                t->result.test_setpoint=t->step_targets[t->validation_point];
            }
        } else if(age>=10) return failed(t,SOLAR_OS_CONTROLLER_TUNE_VALIDATION);
        break;
    }
    default:return failed(t,SOLAR_OS_CONTROLLER_TUNE_BAD_SAMPLE);
    }
    *output=t->result.drive;
    return true;
}
