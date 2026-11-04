#include "model.h"
#include <math.h>

#define ARG SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT
#define OK SOLAR_OS_NATIVE_OK

static bool finite_vector(const float *v, unsigned n)
{
    if (!v) return false;
    for (unsigned i = 0; i < n; ++i) if (!isfinite(v[i])) return false;
    return true;
}

bool solar_os_robot_normalize_vector(float *v, unsigned count)
{
    if (!v || !count || count > 4 || !finite_vector(v, count)) return false;
    float scale = 0;
    for (unsigned i = 0; i < count; ++i) if (fabsf(v[i]) > scale) scale = fabsf(v[i]);
    if (!scale) return false;
    float sum = 0;
    for (unsigned i = 0; i < count; ++i) { v[i] /= scale; sum += v[i]*v[i]; }
    /* sum is in [1,4]. Newton's method converges to float precision in six
     * iterations and avoids libm's errno dependency in the native ELF. */
    float root = 1;
    for (unsigned i = 0; i < 6; ++i) root = .5f*(root + sum/root);
    for (unsigned i = 0; i < count; ++i) v[i] /= root;
    return true;
}

static bool normalize(solar_os_robot_transform_t *t)
{
    return finite_vector(t->translation, 3) && solar_os_robot_normalize_vector(t->rotation, 4);
}

static void multiply(const float a[4], const float b[4], float out[4])
{
    out[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    out[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    out[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    out[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
}

static void rotate(const float q[4], const float v[3], float out[3])
{
    float c[3] = {2*(q[1]*v[2]-q[2]*v[1]), 2*(q[2]*v[0]-q[0]*v[2]), 2*(q[0]*v[1]-q[1]*v[0])};
    out[0] = v[0] + q[3]*c[0] + q[1]*c[2] - q[2]*c[1];
    out[1] = v[1] + q[3]*c[1] + q[2]*c[0] - q[0]*c[2];
    out[2] = v[2] + q[3]*c[2] + q[0]*c[1] - q[1]*c[0];
}

int solar_os_robot_transform_from_rpy(const float xyz[3], const float rpy[3], solar_os_robot_transform_t *out)
{
    if (!out || !finite_vector(xyz, 3) || !finite_vector(rpy, 3)) return ARG;
    float sr = sinf(rpy[0]/2), cr = cosf(rpy[0]/2);
    float sp = sinf(rpy[1]/2), cp = cosf(rpy[1]/2);
    float sy = sinf(rpy[2]/2), cy = cosf(rpy[2]/2);
    solar_os_robot_transform_t t = {.translation = {xyz[0], xyz[1], xyz[2]},
        .rotation = {sr*cp*cy-cr*sp*sy, cr*sp*cy+sr*cp*sy, cr*cp*sy-sr*sp*cy, cr*cp*cy+sr*sp*sy}};
    if (!normalize(&t)) return ARG;
    *out = t;
    return OK;
}

int solar_os_robot_transform_compose(const solar_os_robot_transform_t *a, const solar_os_robot_transform_t *b, solar_os_robot_transform_t *out)
{
    if (!a || !b || !out) return ARG;
    solar_os_robot_transform_t aa = *a, bb = *b, t;
    if (!normalize(&aa) || !normalize(&bb)) return ARG;
    rotate(aa.rotation, bb.translation, t.translation);
    for (unsigned i = 0; i < 3; ++i) t.translation[i] += aa.translation[i];
    multiply(aa.rotation, bb.rotation, t.rotation);
    if (!normalize(&t)) return ARG;
    *out = t;
    return OK;
}

int solar_os_robot_transform_inverse(const solar_os_robot_transform_t *in, solar_os_robot_transform_t *out)
{
    if (!in || !out) return ARG;
    solar_os_robot_transform_t t = *in;
    if (!normalize(&t)) return ARG;
    for (unsigned i = 0; i < 3; ++i) t.rotation[i] = -t.rotation[i];
    float v[3] = {-t.translation[0], -t.translation[1], -t.translation[2]};
    rotate(t.rotation, v, t.translation);
    if (!normalize(&t)) return ARG;
    *out = t;
    return OK;
}

int solar_os_robot_transform_apply(const solar_os_robot_transform_t *in, const float v[3], uint32_t direction, float out[3])
{
    if (!in || !out || direction > 1 || !finite_vector(v, 3)) return ARG;
    solar_os_robot_transform_t t = *in;
    if (!normalize(&t)) return ARG;
    float result[3];
    rotate(t.rotation, v, result);
    for (unsigned i = 0; i < 3; ++i) {
        if (!direction) result[i] += t.translation[i];
        if (!isfinite(result[i])) return ARG;
    }
    for (unsigned i = 0; i < 3; ++i) out[i] = result[i];
    return OK;
}
