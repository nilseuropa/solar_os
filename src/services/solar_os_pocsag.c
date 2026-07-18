#include "solar_os_pocsag.h"

#include <string.h>

#define POCSAG_IDLE_CODEWORD 0x7A89C197U
#define POCSAG_SYNC_CODEWORD 0x7CD215D8U
#define POCSAG_BCH_GENERATOR 0x769U
#define POCSAG_RIC_MAX 2097151U
#define POCSAG_CODEWORDS_PER_BATCH 16U

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

static uint32_t encode_codeword(uint32_t data)
{
    uint32_t value = data >> 1;
    for (int bit = 30; bit >= 10; bit--) {
        if ((value & (1UL << bit)) != 0) {
            value ^= POCSAG_BCH_GENERATOR << (bit - 10);
        }
    }

    uint32_t word = data | ((value & 0x3FFU) << 1);
    if ((__builtin_popcount(word) & 1) != 0) {
        word |= 1U;
    }
    return word;
}

static bool numeric_symbol(char character, uint8_t *symbol)
{
    static const char numeric[] = "0123456789*U -)(";
    const char *match = strchr(numeric, character);
    if (match == NULL) {
        return false;
    }
    if (symbol != NULL) {
        *symbol = (uint8_t)(match - numeric);
    }
    return true;
}

static bool message_valid(solar_os_pocsag_format_t format, const char *text, size_t *text_len)
{
    if (text == NULL || text_len == NULL ||
        (format != SOLAR_OS_POCSAG_FORMAT_ALPHA && format != SOLAR_OS_POCSAG_FORMAT_NUMERIC)) {
        return false;
    }

    const size_t len = strnlen(text, SOLAR_OS_POCSAG_TEXT_MAX);
    if (len == 0 || len >= SOLAR_OS_POCSAG_TEXT_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        const unsigned char character = (unsigned char)text[i];
        if (format == SOLAR_OS_POCSAG_FORMAT_ALPHA) {
            if (character < 0x20 || character > 0x7E) {
                return false;
            }
        } else if (!numeric_symbol((char)character, NULL)) {
            return false;
        }
    }
    *text_len = len;
    return true;
}

static uint32_t message_data_word(solar_os_pocsag_format_t format,
                                  const char *text,
                                  size_t text_len,
                                  size_t message_word)
{
    const size_t width = format == SOLAR_OS_POCSAG_FORMAT_NUMERIC ? 4U : 7U;
    const size_t message_bits = text_len * width;
    uint32_t data = 0;

    for (size_t output_bit = 0; output_bit < 20; output_bit++) {
        const size_t bit_index = message_word * 20U + output_bit;
        size_t character_bit = 0;
        uint8_t character = 0;
        if (bit_index < message_bits) {
            const size_t character_index = bit_index / width;
            character_bit = bit_index % width;
            character = (uint8_t)text[character_index];
            if (format == SOLAR_OS_POCSAG_FORMAT_NUMERIC) {
                (void)numeric_symbol(text[character_index], &character);
            }
        } else {
            character_bit = (bit_index - message_bits) % width;
            character = format == SOLAR_OS_POCSAG_FORMAT_NUMERIC ? 12U : (uint8_t)' ';
        }
        data |= ((uint32_t)((character >> character_bit) & 1U)) << (19U - output_bit);
    }
    return data;
}

static void write_word(uint8_t *output, uint32_t word)
{
    output[0] = (uint8_t)(word >> 24);
    output[1] = (uint8_t)(word >> 16);
    output[2] = (uint8_t)(word >> 8);
    output[3] = (uint8_t)word;
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

esp_err_t solar_os_pocsag_encode_payload(uint32_t ric,
                                         uint8_t function,
                                         solar_os_pocsag_format_t format,
                                         const char *text,
                                         uint8_t *payload,
                                         size_t payload_capacity,
                                         size_t *payload_len,
                                         size_t *batch_count)
{
    size_t text_len = 0;
    if (ric > POCSAG_RIC_MAX || function > 3 || payload == NULL || payload_len == NULL ||
        !message_valid(format, text, &text_len)) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t width = format == SOLAR_OS_POCSAG_FORMAT_NUMERIC ? 4U : 7U;
    const size_t message_words = (text_len * width + 19U) / 20U;
    const size_t address_index = (ric & 7U) * 2U;
    const size_t batches = (address_index + 1U + message_words +
                            POCSAG_CODEWORDS_PER_BATCH - 1U) /
                           POCSAG_CODEWORDS_PER_BATCH;
    const size_t required = batches * SOLAR_OS_POCSAG_BATCH_BYTES + (batches - 1U) * 4U;
    if (required > payload_capacity || required > SOLAR_OS_POCSAG_PAYLOAD_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t output_offset = 0;
    for (size_t batch = 0; batch < batches; batch++) {
        if (batch > 0) {
            write_word(&payload[output_offset], POCSAG_SYNC_CODEWORD);
            output_offset += 4;
        }
        for (size_t slot = 0; slot < POCSAG_CODEWORDS_PER_BATCH; slot++) {
            const size_t codeword_index = batch * POCSAG_CODEWORDS_PER_BATCH + slot;
            uint32_t word = POCSAG_IDLE_CODEWORD;
            if (codeword_index == address_index) {
                const uint32_t address = ((ric >> 3) & 0x3FFFFU) << 13;
                word = encode_codeword(address | ((uint32_t)function << 11));
            } else if (codeword_index > address_index &&
                       codeword_index <= address_index + message_words) {
                const size_t message_word = codeword_index - address_index - 1U;
                word = encode_codeword(0x80000000U |
                                       (message_data_word(format,
                                                          text,
                                                          text_len,
                                                          message_word) << 11));
            }
            write_word(&payload[output_offset], word);
            output_offset += 4;
        }
    }

    *payload_len = output_offset;
    if (batch_count != NULL) {
        *batch_count = batches;
    }
    return ESP_OK;
}

const char *solar_os_pocsag_format_name(solar_os_pocsag_format_t format)
{
    return format == SOLAR_OS_POCSAG_FORMAT_NUMERIC ? "numeric" : "alpha";
}
