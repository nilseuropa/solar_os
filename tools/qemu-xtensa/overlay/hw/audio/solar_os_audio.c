/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "audio/audio.h"
#include "solar_os_qemu_audio_protocol.h"

#define TYPE_SOLAR_OS_AUDIO "solar-os-audio"
OBJECT_DECLARE_SIMPLE_TYPE(SolarOSAudio, SOLAR_OS_AUDIO)
#define FRAME_BYTES SOLAR_OS_QEMU_AUDIO_FRAME_BYTES

struct SolarOSAudio {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    QEMUSoundCard card;
    SWVoiceOut *voice;
    bool ready, active, stopping;
    uint32_t head, count, volume, error, written, submitted;
    uint8_t ring[SOLAR_OS_QEMU_AUDIO_QUEUE_FRAMES * FRAME_BYTES];
    uint8_t stage[SOLAR_OS_QEMU_AUDIO_STAGE_FRAMES * FRAME_BYTES];
};

static void audio_volume(SolarOSAudio *s)
{
    uint8_t gain = s->volume * 255U / 100U;
    AUD_set_volume_out(s->voice, !gain, gain, gain);
}

static void audio_pump(void *opaque, int available)
{
    SolarOSAudio *s = opaque;
    if (!s->active) return;
    while (s->count && available >= FRAME_BYTES) {
        size_t length = MIN(s->count, MIN(sizeof(s->ring) - s->head, available));
        length -= length % FRAME_BYTES;
        size_t sent = AUD_write(s->voice, s->ring + s->head, length);
        if (!sent) break;
        s->head = (s->head + sent) % sizeof(s->ring);
        s->count -= sent;
        s->submitted += sent / FRAME_BYTES;
        available -= sent;
    }
    if (s->stopping && !s->count && AUD_is_empty_out(s->voice)) {
        /* Retain the backend across playback sessions; reopening a voice can
         * reset its host output or replace an existing WAV capture. */
        AUD_set_active_out(s->voice, 0);
        s->active = s->stopping = false;
    }
}

static void audio_reset(Object *obj, ResetType type)
{
    SolarOSAudio *s = SOLAR_OS_AUDIO(obj);
    if (s->voice) AUD_close_out(&s->card, s->voice);
    s->voice = NULL;
    s->active = s->stopping = false;
    s->head = s->count = s->written = s->submitted = s->error = 0;
    s->volume = 100;
    memset(s->stage, 0, sizeof(s->stage));
}

static void audio_control(SolarOSAudio *s, uint32_t command)
{
    s->error = 0;
    if (command == SOLAR_OS_QEMU_AUDIO_RESET) {
        audio_reset(OBJECT(s), RESET_TYPE_COLD);
    } else if (command == SOLAR_OS_QEMU_AUDIO_START) {
        if (!s->ready || s->active) {
            s->error = SOLAR_OS_QEMU_AUDIO_ERROR_STATE;
            return;
        }
        if (!s->voice) {
            struct audsettings settings = {
                .freq = SOLAR_OS_QEMU_AUDIO_RATE, .nchannels = 2,
                .fmt = AUDIO_FORMAT_S16, .endianness = 0,
            };
            s->voice = AUD_open_out(&s->card, NULL, "SolarOS playback", s,
                                   audio_pump, &settings);
        }
        if (!s->voice) { s->error = SOLAR_OS_QEMU_AUDIO_ERROR_BACKEND; return; }
        s->head = s->count = s->written = s->submitted = 0;
        s->stopping = false;
        s->active = true;
        audio_volume(s);
        AUD_set_active_out(s->voice, 1);
    } else if (command == SOLAR_OS_QEMU_AUDIO_STOP) {
        s->stopping = true;
        if (!s->count && AUD_is_empty_out(s->voice)) {
            AUD_set_active_out(s->voice, 0);
            s->active = s->stopping = false;
        }
    } else s->error = SOLAR_OS_QEMU_AUDIO_ERROR_ARGUMENT;
}

static uint64_t audio_read(void *opaque, hwaddr offset, unsigned size)
{
    SolarOSAudio *s = opaque;
    if (offset >= SOLAR_OS_QEMU_AUDIO_REG_DATA &&
        offset + size <= SOLAR_OS_QEMU_AUDIO_REG_DATA + sizeof(s->stage)) {
        uint64_t value = 0;
        for (unsigned i = 0; i < size; i++) value |= (uint64_t)s->stage[offset - SOLAR_OS_QEMU_AUDIO_REG_DATA + i] << (i * 8);
        return value;
    }
    if (size != 4) return 0;
    switch (offset) {
    case SOLAR_OS_QEMU_AUDIO_REG_MAGIC: return SOLAR_OS_QEMU_AUDIO_MAGIC;
    case SOLAR_OS_QEMU_AUDIO_REG_VERSION: return SOLAR_OS_QEMU_AUDIO_VERSION;
    case SOLAR_OS_QEMU_AUDIO_REG_READY: return s->ready;
    case SOLAR_OS_QEMU_AUDIO_REG_ACTIVE: return s->active;
    case SOLAR_OS_QEMU_AUDIO_REG_FREE: return s->active && !s->stopping ? (sizeof(s->ring) - s->count) / FRAME_BYTES : 0;
    case SOLAR_OS_QEMU_AUDIO_REG_QUEUED: return s->count / FRAME_BYTES;
    case SOLAR_OS_QEMU_AUDIO_REG_VOLUME: return s->volume;
    case SOLAR_OS_QEMU_AUDIO_REG_ERROR: return s->error;
    case SOLAR_OS_QEMU_AUDIO_REG_WRITTEN: return s->written;
    case SOLAR_OS_QEMU_AUDIO_REG_SUBMITTED: return s->submitted;
    default: return 0;
    }
}

static void audio_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    SolarOSAudio *s = opaque;
    if (offset >= SOLAR_OS_QEMU_AUDIO_REG_DATA &&
        offset + size <= SOLAR_OS_QEMU_AUDIO_REG_DATA + sizeof(s->stage)) {
        for (unsigned i = 0; i < size; i++) s->stage[offset - SOLAR_OS_QEMU_AUDIO_REG_DATA + i] = value >> (i * 8);
        return;
    }
    if (size != 4) return;
    if (offset == SOLAR_OS_QEMU_AUDIO_REG_CONTROL) audio_control(s, value);
    else if (offset == SOLAR_OS_QEMU_AUDIO_REG_VOLUME) {
        if (value > 100) { s->error = SOLAR_OS_QEMU_AUDIO_ERROR_ARGUMENT; return; }
        s->volume = value;
        audio_volume(s);
        s->error = 0;
    } else if (offset == SOLAR_OS_QEMU_AUDIO_REG_COMMIT) {
        s->error = 0;
        if (!s->active || s->stopping) s->error = SOLAR_OS_QEMU_AUDIO_ERROR_STATE;
        else if (!value || value > SOLAR_OS_QEMU_AUDIO_STAGE_FRAMES) s->error = SOLAR_OS_QEMU_AUDIO_ERROR_ARGUMENT;
        else if (value > (sizeof(s->ring) - s->count) / FRAME_BYTES) s->error = SOLAR_OS_QEMU_AUDIO_ERROR_FULL;
        else {
            size_t bytes = value * FRAME_BYTES;
            size_t tail = (s->head + s->count) % sizeof(s->ring);
            size_t first = MIN(bytes, sizeof(s->ring) - tail);
            memcpy(s->ring + tail, s->stage, first);
            memcpy(s->ring, s->stage + first, bytes - first);
            s->count += bytes;
            s->written += value;
        }
    }
}

static const MemoryRegionOps audio_ops = {
    .read = audio_read, .write = audio_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1, .valid.max_access_size = 4,
};

static void audio_realize(DeviceState *dev, Error **errp)
{
    SolarOSAudio *s = SOLAR_OS_AUDIO(dev);
    /* No backend was requested: leave the hardware present but unavailable. */
    if (s->card.state) s->ready = AUD_register_card("SolarOS audio", &s->card, errp);
}

static void audio_unrealize(DeviceState *dev)
{
    SolarOSAudio *s = SOLAR_OS_AUDIO(dev);
    audio_reset(OBJECT(s), RESET_TYPE_COLD);
    if (s->ready) AUD_remove_card(&s->card);
    s->ready = false;
}

static void audio_init(Object *obj)
{
    SolarOSAudio *s = SOLAR_OS_AUDIO(obj);
    s->volume = 100;
    memory_region_init_io(&s->mmio, obj, &audio_ops, s, TYPE_SOLAR_OS_AUDIO, 0x2000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static Property audio_properties[] = {
    DEFINE_AUDIO_PROPERTIES(SolarOSAudio, card), DEFINE_PROP_END_OF_LIST(),
};

static void audio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = audio_realize;
    dc->unrealize = audio_unrealize;
    device_class_set_props(dc, audio_properties);
    RESETTABLE_CLASS(klass)->phases.hold = audio_reset;
}

static const TypeInfo audio_type = {
    .name = TYPE_SOLAR_OS_AUDIO, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SolarOSAudio), .instance_init = audio_init,
    .class_init = audio_class_init,
};
static void audio_register(void) { type_register_static(&audio_type); }
type_init(audio_register)
