#include "solar_os_native_expr.h"
#include <string.h>
#include "solar_os_config.h"
#include "solar_os_expr.h"
#include "solar_os_memory.h"
#include "solar_os_native_expr_abi.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"

#if SOLAR_OS_PACKAGE_SERVICE_EXPR
_Static_assert(SOLAR_OS_EXPR_ERROR_MAX <= SOLAR_OS_NATIVE_EXPR_ERROR_MAX, "expression error capacity");
static void expr_error_copy(solar_os_native_expr_error_v1_t *out, const solar_os_expr_error_t *error)
{
    if (!out) return;
    *out = (solar_os_native_expr_error_v1_t){0};
    if (!error) return;
    const uint64_t position = error->position;
    out->position = position > UINT32_MAX ? UINT32_MAX : (uint32_t)position;
    memcpy(out->message, error->message, sizeof(error->message));
    out->message[sizeof(out->message)-1] = '\0';
}
static int expr_destroy(void *program)
{ solar_os_memory_free(program); return SOLAR_OS_NATIVE_OK; }
static int expr_compile(const char *source, solar_os_native_handle_t *out, solar_os_native_expr_error_v1_t *error)
{
    expr_error_copy(error, NULL);
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!source) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_expr_program_t *program = solar_os_memory_calloc(1, sizeof(*program),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.expr");
    if (!program) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    solar_os_expr_error_t detail = {0};
    int result = solar_os_native_result(solar_os_expr_compile(source, program, &detail));
    expr_error_copy(error, &detail);
    if (!result) result = solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_EXPR, program, expr_destroy, out);
    if (result) solar_os_memory_free(program);
    return result;
}
static bool expr_variable(void *user, const char *name, double *value)
{
    const solar_os_native_expr_context_v1_t *context = user;
    return context->variable && context->variable(context->user, name, value);
}
static bool expr_function(void *user, const char *name, const double *arguments, size_t count, double *value)
{
    const solar_os_native_expr_context_v1_t *context = user;
    return context->function && context->function(context->user, name, arguments, count, value);
}
static int expr_evaluate(solar_os_native_handle_t id, const solar_os_native_expr_context_v1_t *context,
    double *value, solar_os_native_expr_error_v1_t *error)
{
    expr_error_copy(error, NULL);
    if (!value) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *value = 0;
    if (context && (context->struct_size < sizeof(*context) || (context->flags & ~SOLAR_OS_NATIVE_EXPR_HAS_X)))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_expr_program_t *program = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_EXPR);
    if (!program) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_native_expr_context_v1_t copied = {0};
    solar_os_expr_context_t input = {0};
    if (context) {
        copied = *context;
        input = (solar_os_expr_context_t){.user = &copied, .variable = expr_variable, .function = expr_function,
            .has_x = (copied.flags & SOLAR_OS_NATIVE_EXPR_HAS_X) != 0, .x = copied.x};
    }
    solar_os_expr_error_t detail = {0};
    int result = solar_os_native_result(solar_os_expr_evaluate(program, context ? &input : NULL, value, &detail));
    solar_os_native_handle_release(id);
    expr_error_copy(error, &detail);
    if (result) *value = 0;
    return result;
}
static int expr_uses_x(solar_os_native_handle_t id, bool *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = false;
    const solar_os_expr_program_t *program = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_EXPR);
    if (!program) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    *out = program->uses_x;
    solar_os_native_handle_release(id);
    return SOLAR_OS_NATIVE_OK;
}
static int expr_close(solar_os_native_handle_t id)
{ return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_EXPR); }
static const solar_os_native_expr_api_v1_t expr_api = {
    .abi_version = SOLAR_OS_NATIVE_EXPR_ABI, .struct_size = sizeof(expr_api),
    .compile = expr_compile, .evaluate = expr_evaluate, .uses_x = expr_uses_x, .close = expr_close,
};
#endif
const void *solar_os_native_expr_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_expr_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
