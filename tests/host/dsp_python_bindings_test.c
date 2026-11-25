#include "py/builtin.h"
#include "py/compile.h"
#include "py/cstack.h"
#include "py/gc.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include "shared/runtime/gchelper.h"
#include "solar_os_native_client.h"
#include "solar_os_native_dsp.h"
#include "solar_os_native_handles.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static bool available = true;
static unsigned lookups;
static const void *lookup(const char *name, uint32_t abi, uint32_t minimum) {
    ++lookups;
    return available ? solar_os_native_dsp_get_service(name, abi, minimum) : NULL;
}
static const solar_os_native_host_api_v1_t host = {
    .abi_version = 1, .struct_size = sizeof(host), .get_service = lookup};
const solar_os_native_host_api_v1_t *solar_os_native_host_v1(void) { return &host; }
static void python_dict_store_cstr(mp_obj_t d, const char *k, const char *v) {
    mp_obj_dict_store(d, mp_obj_new_str(k, strlen(k)), mp_obj_new_str(v, strlen(v)));
}
static void python_dict_store_uint(mp_obj_t d, const char *k, uint32_t v) {
    mp_obj_dict_store(d, mp_obj_new_str(k, strlen(k)), mp_obj_new_int_from_uint(v));
}
static mp_obj_t python_i64_to_obj(int64_t v) { return mp_obj_new_int_from_ll(v); }
static size_t python_size_from_obj(mp_obj_t o) {
    mp_int_t v = mp_obj_get_int(o);
    if (v < 0)
        mp_raise_ValueError(MP_ERROR_TEXT("bad size"));
    return (size_t)v;
}
static mp_obj_t python_kw_value(mp_map_t *m, const char *k) {
    if (!m)
        return MP_OBJ_NULL;
    mp_map_elem_t *e = mp_map_lookup(m, mp_obj_new_str(k, strlen(k)), MP_MAP_LOOKUP);
    return e ? e->value : MP_OBJ_NULL;
}
static void python_check_known_kwargs(mp_map_t *m, const char *a, const char *b, const char *c,
                                      const char *d) {
    if (!m)
        return;
    const char *names[] = {a, b, c, d};
    for (size_t i = 0; i < m->alloc; i++)
        if (mp_map_slot_is_filled(m, i)) {
            const char *k = mp_obj_str_get_str(m->table[i].key);
            bool found = false;
            for (unsigned j = 0; j < 4; j++)
                if (names[j] && !strcmp(k, names[j]))
                    found = true;
            if (!found)
                mp_raise_TypeError(MP_ERROR_TEXT("bad keyword"));
        }
}
#include "../../src/apps/solar_os_python_dsp.inc"
void solar_os_micropython_vm_hook(void) {}
void solar_os_micropython_check_interrupt(void) {}
uint32_t solar_os_micropython_random_seed(void) { return 42; }
void mp_hal_stdout_tx_strn(const char *s, size_t n) { fwrite(s, 1, n, stdout); }
void mp_hal_stdout_tx_strn_cooked(const char *s, size_t n) { fwrite(s, 1, n, stdout); }
int solar_os_micropython_resolve_path(const char *in, char *out, size_t size) {
    if (strlen(in) >= size)
        return -1;
    strcpy(out, in);
    return 0;
}
mp_import_stat_t mp_import_stat(const char *path) {
    (void)path;
    return MP_IMPORT_STAT_NO_EXIST;
}
static mp_obj_t no_open(size_t n, const mp_obj_t *a, mp_map_t *k) {
    (void)n;
    (void)a;
    (void)k;
    mp_raise_OSError(2);
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 1, no_open);
void gc_collect(void) {
    gc_collect_start();
    gc_helper_collect_regs_and_stack();
    gc_collect_end();
}
void nlr_jump_fail(void *v) {
    (void)v;
    assert(0);
    abort();
}
static void run(const char *source) {
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_lexer_t *lexer =
            mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_, source, strlen(source), 0);
        mp_parse_tree_t tree = mp_parse(lexer, MP_PARSE_FILE_INPUT);
        mp_obj_t fn = mp_compile(&tree, MP_QSTR__lt_stdin_gt_, false);
        mp_call_function_0(fn);
        nlr_pop();
    } else {
        mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
        assert(0);
    }
}
int main(void) {
    char top;
    void *heap = malloc(2 * 1024 * 1024);
    assert(heap);
    mp_cstack_init_with_top(&top, 1024 * 1024);
    gc_init(heap, (char *)heap + 2 * 1024 * 1024);
    mp_init();
    mp_obj_t module = mp_obj_new_module(qstr_from_str("dsp"));
    mp_obj_dict_t *globals = ((mp_obj_module_t *)MP_OBJ_TO_PTR(module))->globals;
#define EXPOSE(name)                                                                               \
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), mp_obj_new_str(#name, sizeof(#name) - 1),          \
                      MP_OBJ_FROM_PTR(&solaros_dsp_##name##_obj))
    EXPOSE(backend);
    EXPOSE(capabilities);
    EXPOSE(dot);
    EXPOSE(gain);
    EXPOSE(mix);
    EXPOSE(clip);
    EXPOSE(level);
    EXPOSE(window);
    EXPOSE(fir);
    EXPOSE(fft);
    mp_store_global(qstr_from_str("dsp"), module);
    run("import struct,gc\nassert "
        "dsp.backend()=='portable'\na=struct.pack('<hhhh',-32768,-1000,1000,32767)\nb=struct.pack('"
        "<hhhh',-32768,2000,-2000,32767)\nassert "
        "dsp.dot(a,b)==32768*32768-4000000+32767*32767\nassert "
        "dsp.gain(a,0.5)==struct.pack('<hhhh',-16384,-500,500,16383)\nassert "
        "dsp.level(struct.pack('<hh',-3,4))==(4,3)\nf=dsp.fir(struct.pack('<hh',16384,16384))"
        "\nassert "
        "f.process(struct.pack('<hhhh',1000,2000,3000,4000))==struct.pack('<hhhh',500,1500,2500,"
        "3500)\nf.reset()\ng=dsp.fir(struct.pack('<h',32767),decimation=2)\nassert "
        "g.process(struct.pack('<hhh',10,20,30))==struct.pack('<hh',9,29)\nfft=dsp.fft(8)"
        "\nspectrum,scale=fft.execute(struct.pack('<hhhhhhhh',32767,0,0,0,0,0,0,0))\nassert "
        "scale==3 and len(spectrum)==32\nfor i in range(8):\n assert "
        "struct.unpack('<hh',spectrum[i*4:i*4+4])==(4095,0)\n");
    unsigned cached = lookups;
    available = false;
    run("f.process(struct.pack('<h',1000))\ng.reset()\nfft.execute(bytes(16))\nf.close()\nf.close()"
        "\ng.close()\nfft.close()\ntry:\n dsp.backend()\n assert False\nexcept RuntimeError:\n "
        "pass\n");
    assert(lookups == cached + 1 && !solar_os_native_handle_count());
    available = true;
    run("f=dsp.fir(struct.pack('<h',32767))\nf=None\ngc.collect()\n");
    assert(!solar_os_native_handle_count());
    mp_deinit();
    free(heap);
    puts("MicroPython DSP: real VM, ABI arithmetic, streaming, FFT, cached calls and GC passed");
    return 0;
}
