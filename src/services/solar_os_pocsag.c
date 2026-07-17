#include "solar_os_pocsag.h"

#include <string.h>

#define POCSAG_IDLE_CODEWORD 0x7A89C197U
#define POCSAG_BCH_GENERATOR 0x769U

static bool codeword_valid(uint32_t word)
{
    if ((__builtin_popcount(word) & 1) != 0) {
        return false;
    }

    uint32_t value = word >> 1;
    for (int bit = 30; bit >= 10; bit--) {
        if ((value & (1UL << bit)) != 0) {
            value ^= POCSAG_BCH_GENERATOR << (bit - 10);
        }
    }
    return (value & 0x3FFU) == 0;
}

static bool correct_codeword(uint32_t received, uint32_t *corrected, uint8_t *corrected_bits)
{
    if (codeword_valid(received)) {
        *corrected = received;
        *corrected_bits = 0;
        return true;
    }

    for (uint8_t first = 0; first < 32; first++) {
        const uint32_t one_bit = received ^ (1UL << first);
        if (codeword_valid(one_bit)) {
            *corrected = one_bit;
            *corrected_bits = 1;
            return true;
        }
        for (uint8_t second = first + 1; second < 32; second++) {
            const uint32_t two_bits = one_bit ^ (1UL << second);
            if (codeword_valid(two_bits)) {
                *corrected = two_bits;
                *corrected_bits = 2;
                return true;
            }
        }
    }
    return false;
}

static void append_character(solar_os_pocsag_decoder_t *decoder, char character)
{
    if (character == '\0' || character == '\x03' || character == '\x04') {
        return;
    }
    if ((unsigned char)character < 0x20 && character != '\n' && character != '\r') {
        return;
    }
    if (decoder->text_len + 1 >= sizeof(decoder->message.text)) {
        decoder->message.truncated = true;
        return;
    }
    decoder->message.text[decoder->text_len++] = character;
    decoder->message.text[decoder->text_len] = '\0';
}

static void append_message_bits(solar_os_pocsag_decoder_t *decoder, uint32_t bits)
{
    const uint8_t width = decoder->format == SOLAR_OS_POCSAG_FORMAT_NUMERIC ? 4 : 7;
    static const char numeric[] = "0123456789*U -)(";

    for (int bit = 19; bit >= 0; bit--) {
        decoder->character |= (uint8_t)(((bits >> bit) & 1U) << decoder->character_bits);
        decoder->character_bits++;
        if (decoder->character_bits != width) {
            continue;
        }

        if (decoder->format == SOLAR_OS_POCSAG_FORMAT_NUMERIC) {
            append_character(decoder, numeric[decoder->character & 0x0FU]);
        } else {
            append_character(decoder, (char)(decoder->character & 0x7FU));
        }
        decoder->character = 0;
        decoder->character_bits = 0;
    }
}

static bool finish_message(solar_os_pocsag_decoder_t *decoder,
                           solar_os_pocsag_message_cb_t callback,
                           void *user)
{
    if (!decoder->active) {
        return false;
    }

    while (decoder->text_len > 0 &&
           (decoder->message.text[decoder->text_len - 1] == ' ' ||
            decoder->message.text[decoder->text_len - 1] == '\r' ||
            decoder->message.text[decoder->text_len - 1] == '\n')) {
        decoder->message.text[--decoder->text_len] = '\0';
    }

    const bool emitted = decoder->text_len > 0;
    if (emitted && callback != NULL) {
        callback(&decoder->message, user);
    }
    decoder->active = false;
    decoder->text_len = 0;
    decoder->character = 0;
    decoder->character_bits = 0;
    return emitted;
}

static void begin_message(solar_os_pocsag_decoder_t *decoder,
                          uint32_t ric,
                          uint8_t function,
                          int16_t rssi_dbm)
{
    memset(&decoder->message, 0, sizeof(decoder->message));
    decoder->message.ric = ric;
    decoder->message.function = function;
    decoder->message.format = decoder->format;
    decoder->message.rssi_dbm = rssi_dbm;
    decoder->active = true;
    decoder->text_len = 0;
    decoder->character = 0;
    decoder->character_bits = 0;
}

void solar_os_pocsag_decoder_init(solar_os_pocsag_decoder_t *decoder,
                                  uint32_t target_ric,
                                  solar_os_pocsag_format_t format)
{
    if (decoder == NULL) {
        return;
    }
    memset(decoder, 0, sizeof(*decoder));
    decoder->target_ric = target_ric;
    decoder->format = format;
}

size_t solar_os_pocsag_decode_batch(solar_os_pocsag_decoder_t *decoder,
                                    const uint8_t *batch,
                                    size_t len,
                                    int16_t rssi_dbm,
                                    solar_os_pocsag_message_cb_t callback,
                                    void *user)
{
    if (decoder == NULL || batch == NULL || len != SOLAR_OS_POCSAG_BATCH_BYTES) {
        return 0;
    }

    size_t emitted = 0;
    for (size_t index = 0; index < len / 4; index++) {
        const uint8_t *raw = &batch[index * 4];
        const uint32_t received = ((uint32_t)raw[0] << 24) |
                                  ((uint32_t)raw[1] << 16) |
                                  ((uint32_t)raw[2] << 8) |
                                  raw[3];
        uint32_t word = 0;
        uint8_t corrected_bits = 0;
        if (!correct_codeword(received, &word, &corrected_bits)) {
            if (decoder->active) {
                decoder->message.uncorrectable_codewords++;
            }
            continue;
        }
        if (decoder->active && corrected_bits > 0) {
            decoder->message.corrected_codewords++;
        }

        if (word == POCSAG_IDLE_CODEWORD) {
            emitted += finish_message(decoder, callback, user) ? 1U : 0U;
            continue;
        }

        if ((word & 0x80000000U) == 0) {
            emitted += finish_message(decoder, callback, user) ? 1U : 0U;
            const uint32_t ric = (((word >> 13) & 0x3FFFFU) << 3) |
                                 (uint32_t)(index / 2U);
            if (decoder->target_ric == 0 || ric == decoder->target_ric) {
                begin_message(decoder, ric, (uint8_t)((word >> 11) & 0x03U), rssi_dbm);
                decoder->message.corrected_codewords = corrected_bits > 0 ? 1U : 0U;
            }
            continue;
        }

        if (decoder->active) {
            append_message_bits(decoder, (word >> 11) & 0xFFFFFU);
        }
    }
    return emitted;
}

bool solar_os_pocsag_decoder_flush(solar_os_pocsag_decoder_t *decoder,
                                   solar_os_pocsag_message_cb_t callback,
                                   void *user)
{
    return decoder != NULL && finish_message(decoder, callback, user);
}

const char *solar_os_pocsag_format_name(solar_os_pocsag_format_t format)
{
    return format == SOLAR_OS_POCSAG_FORMAT_NUMERIC ? "numeric" : "alpha";
}
