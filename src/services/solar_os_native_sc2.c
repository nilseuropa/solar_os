#include "solar_os_native_sc2.h"
#include "solar_os_config.h"
#include "solar_os_native_sc2_abi.h"
#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_SC2
#include "solar_os_native_support.h"
#include "solar_os_sc2.h"

_Static_assert(SOLAR_OS_SC2_DEVICE_ID_LENGTH == SC2_DEVICE_ID_HEX_LEN, "SC2 device ID size");
_Static_assert(SOLAR_OS_SC2_PUBLIC_KEY_BASE64_LENGTH == SC2_B64_ENCODED_LEN(SC2_KEY_LEN),
               "SC2 public key size");
_Static_assert(SOLAR_OS_SC2_FINGERPRINT_LENGTH == SC2_SAFETY_SHORT_LEN &&
                   SOLAR_OS_SC2_SAFETY_FULL_LENGTH == SC2_SAFETY_FULL_LEN &&
                   SOLAR_OS_SC2_SAFETY_WORDS_LENGTH == SC2_SAFETY_WORDS_MAX,
               "SC2 safety record sizes");
_Static_assert(SOLAR_OS_SC2_MESSAGE_ID_LENGTH == 2U * SC2_MSG_ID_LEN &&
                   SOLAR_OS_SC2_PUBLIC_ROOM_NAME_LENGTH == SC2_ROOM_NAME_MAX &&
                   SOLAR_OS_SC2_PUBLIC_AUTHOR_LENGTH == SC2_POST_NAME_MAX,
               "SC2 public post sizes");
_Static_assert(SOLAR_OS_SC2_STORAGE_PATH_CAPACITY == SC2E_NAME_MAX + 64U, "SC2 storage path size");
_Static_assert(SOLAR_OS_SC2_FRAME_CAPACITY == SC2E_FRAME_MAX &&
                   SOLAR_OS_SC2_MESSAGE_CAPACITY == SC2E_INNER_MAX + 1U &&
                   SOLAR_OS_SC2_PREKEYS_SIGNED_ONLY == SC2E_OPK_NONE,
               "SC2 framing limits");

static int sc2_result(esp_err_t error)
{
    static const int16_t protocol_error[] = {
        [-SC2_ERR_ARG - 1] = SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT,
        [-SC2_ERR_BOUNDS - 1] = SOLAR_OS_NATIVE_ERROR_INVALID_SIZE,
        [-SC2_ERR_FORMAT - 1] = SOLAR_OS_NATIVE_SC2_ERROR_FORMAT,
        [-SC2_ERR_AUTH - 1] = SOLAR_OS_NATIVE_SC2_ERROR_AUTH,
        [-SC2_ERR_KEY - 1] = SOLAR_OS_NATIVE_SC2_ERROR_KEY,
        [-SC2_ERR_RNG - 1] = SOLAR_OS_NATIVE_SC2_ERROR_RANDOM,
        [-SC2_ERR_REPLAY - 1] = SOLAR_OS_NATIVE_SC2_ERROR_REPLAY,
        [-SC2_ERR_SKIP - 1] = SOLAR_OS_NATIVE_SC2_ERROR_GAP,
        [-SC2_ERR_STATE - 1] = SOLAR_OS_NATIVE_ERROR_INVALID_STATE,
        [-SC2_ERR_BACKEND - 1] = SOLAR_OS_NATIVE_ERROR_FAILED,
        [-SC2_ERR_NOT_FOUND - 1] = SOLAR_OS_NATIVE_ERROR_NOT_FOUND,
    };
    if (error > SOLAR_OS_SC2_ERR_BASE &&
        error <= SOLAR_OS_SC2_ERR_BASE + (int)(sizeof(protocol_error) / sizeof(*protocol_error))) {
        return protocol_error[error - SOLAR_OS_SC2_ERR_BASE - 1];
    }
    return solar_os_native_result(error);
}

static int native_initialize(void)
{
    return sc2_result(solar_os_sc2_init());
}

static int native_status(solar_os_sc2_status_v1_t *status)
{
    return sc2_result(solar_os_sc2_get_status(status));
}

static int native_identity(solar_os_sc2_identity_v1_t *info)
{
    return sc2_result(solar_os_sc2_identity(info));
}

static int native_peer_info(const char *peer, solar_os_sc2_peer_v1_t *info)
{
    return sc2_result(solar_os_sc2_peer_info(peer, info));
}

static int native_safety(const char *peer, solar_os_sc2_safety_v1_t *safety)
{
    return sc2_result(solar_os_sc2_safety(peer, safety));
}

static int native_forget_peer(const char *peer)
{
    return sc2_result(solar_os_sc2_forget_peer(peer));
}

static int native_resolve_peer(const char *ref, char out[SOLAR_OS_SC2_DEVICE_ID_LENGTH + 1])
{
    return sc2_result(solar_os_sc2_resolve_peer(ref, out));
}

static int native_alias_set(const char *name, const char *device)
{
    return sc2_result(solar_os_sc2_alias_set(name, device));
}

static int native_alias_get(size_t index, char name[SOLAR_OS_SC2_ALIAS_LENGTH + 1],
                            char device[SOLAR_OS_SC2_DEVICE_ID_LENGTH + 1])
{
    return sc2_result(solar_os_sc2_alias_get(index, name, device));
}

static int native_verified_get(const char *device, bool *verified)
{
    return sc2_result(solar_os_sc2_verified_get(device, verified));
}

static int native_verified_set(const char *device, bool verified)
{
    return sc2_result(solar_os_sc2_verified_set(device, verified));
}

static int native_notice_mark(const char *room)
{
    return sc2_result(solar_os_sc2_notice_mark(room));
}

static int native_keys_publish(const char *rid, uint32_t opk_target, char *out, size_t cap,
                               size_t *out_len)
{
    return sc2_result(solar_os_sc2_keys_publish(rid, opk_target, out, cap, out_len));
}

static int native_add_bundle(const char *line, size_t len,
                             char device[SOLAR_OS_SC2_DEVICE_ID_LENGTH + 1])
{
    return sc2_result(solar_os_sc2_add_bundle(line, len, device));
}

static int native_dm_encrypt(const char *peer, const char *rid, const char *inner, size_t inner_len,
                             char *out, size_t cap, size_t *out_len)
{
    return sc2_result(solar_os_sc2_dm_encrypt(peer, rid, inner, inner_len, out, cap, out_len));
}

static int native_dm_decrypt(const char *line, size_t len, solar_os_sc2_dm_v1_t *result, char *pt,
                             size_t pt_cap, size_t *pt_len)
{
    return sc2_result(solar_os_sc2_dm_decrypt(line, len, result, pt, pt_cap, pt_len));
}

static int native_room_create(const char *room, char room_out[SOLAR_OS_SC2_DEVICE_ID_LENGTH + 1],
                              uint32_t *epoch)
{
    return sc2_result(solar_os_sc2_room_create(room, room_out, epoch));
}

static int native_room_rotate(const char *room, uint32_t *epoch)
{
    return sc2_result(solar_os_sc2_room_rotate(room, epoch));
}

static int native_room_share(const char *room, const char *peer, const char *rid, char *out,
                             size_t cap, size_t *out_len)
{
    return sc2_result(solar_os_sc2_room_share(room, peer, rid, out, cap, out_len));
}

static int native_room_encrypt(const char *room, const char *rid, const char *inner,
                               size_t inner_len, char *out, size_t cap, size_t *out_len)
{
    return sc2_result(solar_os_sc2_room_encrypt(room, rid, inner, inner_len, out, cap, out_len));
}

static int native_room_decrypt(const char *line, size_t len, solar_os_sc2_room_message_v1_t *result,
                               char *pt, size_t pt_cap, size_t *pt_len)
{
    return sc2_result(solar_os_sc2_room_decrypt(line, len, result, pt, pt_cap, pt_len));
}

static int native_room_info(const char *room, solar_os_sc2_room_v1_t *info)
{
    return sc2_result(solar_os_sc2_room_info(room, info));
}

static int native_room_forget(const char *room)
{
    return sc2_result(solar_os_sc2_room_forget(room));
}

static int native_post_sign(const char *relay_id, const char *room, const char *name,
                            const char *body, size_t body_len, uint64_t ts, const char *ref,
                            bool deleted, const char *rid, char *out, size_t cap, size_t *out_len)
{
    return sc2_result(solar_os_sc2_post_sign(relay_id, room, name, body, body_len, ts, ref, deleted,
                                             rid, out, cap, out_len));
}

static int native_post_verify(const char *relay_id, const char *line, size_t len, size_t max_body,
                              solar_os_sc2_post_v1_t *result, char *body, size_t body_cap)
{
    return sc2_result(
        solar_os_sc2_post_verify(relay_id, line, len, max_body, result, body, body_cap));
}

static int native_selftest(solar_os_sc2_selftest_fn report, void *ctx, unsigned *passed,
                           unsigned *failed)
{
    return sc2_result(solar_os_sc2_selftest(report, ctx, passed, failed));
}

static int native_bench(uint32_t iterations, solar_os_sc2_benchmark_fn report, void *ctx)
{
    return sc2_result(solar_os_sc2_bench(iterations, report, ctx));
}

static const solar_os_native_sc2_api_v1_t sc2_api = {
    .abi_version = SOLAR_OS_NATIVE_SC2_ABI,
    .struct_size = sizeof(sc2_api),
    .initialize = native_initialize,
    .status = native_status,
    .identity = native_identity,
    .peer_info = native_peer_info,
    .safety = native_safety,
    .forget_peer = native_forget_peer,
    .resolve_peer = native_resolve_peer,
    .peer_next = solar_os_sc2_peer_next,
    .alias_name_valid = solar_os_sc2_alias_name_valid,
    .alias_set = native_alias_set,
    .alias_get = native_alias_get,
    .verified_get = native_verified_get,
    .verified_set = native_verified_set,
    .notice_seen = solar_os_sc2_notice_seen,
    .notice_mark = native_notice_mark,
    .keys_publish = native_keys_publish,
    .add_bundle = native_add_bundle,
    .dm_encrypt = native_dm_encrypt,
    .dm_decrypt = native_dm_decrypt,
    .room_create = native_room_create,
    .room_rotate = native_room_rotate,
    .room_share = native_room_share,
    .room_encrypt = native_room_encrypt,
    .room_decrypt = native_room_decrypt,
    .room_info = native_room_info,
    .room_forget = native_room_forget,
    .post_sign = native_post_sign,
    .post_verify = native_post_verify,
    .selftest = native_selftest,
    .bench = native_bench,
};
#endif

const void *solar_os_native_sc2_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_sc2_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
