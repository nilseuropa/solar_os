/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SolarOS virtual input: native QEMU events, independent of the serial console. */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "ui/input.h"
#include "solar_os_qemu_input_protocol.h"

#define TYPE_SOLAR_OS_INPUT "solar-os-input"
OBJECT_DECLARE_SIMPLE_TYPE(SolarOSInputState, SOLAR_OS_INPUT)
#define QUEUE_LENGTH 256

typedef struct SolarOSInputEvent {
    uint32_t type, a, b, c, d, sequence;
} SolarOSInputEvent;

struct SolarOSInputState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    QemuInputHandlerState *keyboard, *touch, *mouse;
    SolarOSInputEvent queue[QUEUE_LENGTH], latched;
    unsigned head, count;
    uint32_t sequence, dropped, generation;
    bool latch_valid, focused, absolute;
    bool keys[256];
    uint8_t modifiers;
    int32_t x, y, dx, dy, touch_wheel, mouse_wheel;
    uint32_t touch_buttons, mouse_buttons;
    bool touch_pending, mouse_pending;
};

static void solar_os_input_neutral(SolarOSInputState *s);

static void solar_os_input_queue(SolarOSInputState *s, SolarOSInputEvent event)
{
    if (s->count == QUEUE_LENGTH) {
        s->dropped += s->count + 1;
        solar_os_input_neutral(s);
        return;
    }
    event.sequence = ++s->sequence;
    s->queue[(s->head + s->count++) % QUEUE_LENGTH] = event;
}

static void solar_os_input_neutral(SolarOSInputState *s)
{
    s->generation++;
    s->head = s->count = 0;
    s->modifiers = 0;
    memset(s->keys, 0, sizeof(s->keys));
    s->touch_buttons = s->mouse_buttons = 0;
    s->dx = s->dy = s->touch_wheel = s->mouse_wheel = 0;
    s->touch_pending = s->mouse_pending = false;
    solar_os_input_queue(s, (SolarOSInputEvent) {
        .type = SOLAR_OS_QEMU_INPUT_RELEASE_ALL,
    });
}

static void solar_os_input_focus(DeviceState *dev, bool focused)
{
    SolarOSInputState *s = SOLAR_OS_INPUT(dev);
    if (!focused && s->focused) {
        solar_os_input_neutral(s);
    }
    s->focused = focused;
}

static bool solar_os_input_get_focus(Object *obj, Error **errp)
{
    return SOLAR_OS_INPUT(obj)->focused;
}

static void solar_os_input_set_focus(Object *obj, bool value, Error **errp)
{
    solar_os_input_focus(DEVICE(obj), value);
}

static void solar_os_input_key(DeviceState *dev, QemuConsole *src, InputEvent *evt)
{
    SolarOSInputState *s = SOLAR_OS_INPUT(dev);
    if (!s->focused) {
        return;
    }
    InputKeyEvent *key = evt->u.key.data;
    int qcode = qemu_input_key_value_to_qcode(key->key);
    unsigned usage;
    /* Invert QEMU's canonical USB map instead of inventing a text encoding. */
    for (usage = 4; usage < MIN(256, qemu_input_map_usb_to_qcode_len); usage++) {
        if (qcode > 0 && qemu_input_map_usb_to_qcode[usage] == qcode) {
            break;
        }
    }
    if (usage >= MIN(256, qemu_input_map_usb_to_qcode_len) ||
        s->keys[usage] == key->down) {
        return;
    }
    s->keys[usage] = key->down;
    if (usage >= 0xe0 && usage <= 0xe7) {
        unsigned bit = 1U << (usage - 0xe0);
        s->modifiers = key->down ? s->modifiers | bit : s->modifiers & ~bit;
    }
    solar_os_input_queue(s, (SolarOSInputEvent) {
        .type = SOLAR_OS_QEMU_INPUT_KEY,
        .a = usage, .b = s->modifiers, .c = key->down,
    });
}

static void solar_os_input_pointer(SolarOSInputState *s, InputEvent *evt, bool touch)
{
    if (!s->focused) {
        return;
    }
    if (evt->type == INPUT_EVENT_KIND_ABS) {
        InputMoveEvent *move = evt->u.abs.data;
        if (move->axis == INPUT_AXIS_X) {
            s->x = CLAMP(move->value, 0, (int)SOLAR_OS_QEMU_INPUT_AXIS_MAX);
        } else {
            s->y = CLAMP(move->value, 0, (int)SOLAR_OS_QEMU_INPUT_AXIS_MAX);
        }
    } else if (evt->type == INPUT_EVENT_KIND_REL) {
        InputMoveEvent *move = evt->u.rel.data;
        int32_t *delta = move->axis == INPUT_AXIS_X ? &s->dx : &s->dy;
        *delta = CLAMP((int64_t)*delta + move->value, INT16_MIN, INT16_MAX);
    } else if (evt->type == INPUT_EVENT_KIND_BTN) {
        InputBtnEvent *button = evt->u.btn.data;
        uint32_t bit = 0;
        switch (button->button) {
        case INPUT_BUTTON_LEFT: case INPUT_BUTTON_TOUCH: bit = 1; break;
        case INPUT_BUTTON_RIGHT: bit = 2; break;
        case INPUT_BUTTON_MIDDLE: bit = 4; break;
        case INPUT_BUTTON_WHEEL_UP: case INPUT_BUTTON_WHEEL_DOWN:
            if (button->down) {
                int32_t *wheel = touch ? &s->touch_wheel : &s->mouse_wheel;
                *wheel = CLAMP((int64_t)*wheel +
                              (button->button == INPUT_BUTTON_WHEEL_UP ? 1 : -1),
                              INT16_MIN, INT16_MAX);
            }
            break;
        default: return;
        }
        uint32_t *buttons = touch ? &s->touch_buttons : &s->mouse_buttons;
        *buttons = button->down ? *buttons | bit : *buttons & ~bit;
    }
    if (touch) {
        s->touch_pending = true;
    } else {
        s->mouse_pending = true;
    }
}

static void solar_os_input_touch(DeviceState *dev, QemuConsole *src, InputEvent *evt)
{
    solar_os_input_pointer(SOLAR_OS_INPUT(dev), evt, true);
}

static void solar_os_input_mouse(DeviceState *dev, QemuConsole *src, InputEvent *evt)
{
    solar_os_input_pointer(SOLAR_OS_INPUT(dev), evt, false);
}

static void solar_os_input_sync(DeviceState *dev)
{
    SolarOSInputState *s = SOLAR_OS_INPUT(dev);
    if (s->touch_pending) {
        SolarOSInputEvent event = {.type = SOLAR_OS_QEMU_INPUT_TOUCH,
            .a = s->x, .b = s->y, .c = s->touch_buttons, .d = s->touch_wheel};
        s->touch_pending = false;
        s->touch_wheel = 0;
        solar_os_input_queue(s, event);
    }
    if (s->mouse_pending) {
        SolarOSInputEvent event = {.type = SOLAR_OS_QEMU_INPUT_MOUSE,
            .a = s->dx, .b = s->dy, .c = s->mouse_buttons, .d = s->mouse_wheel};
        s->mouse_pending = false;
        s->dx = s->dy = s->mouse_wheel = 0;
        solar_os_input_queue(s, event);
    }
}

static uint64_t solar_os_input_read(void *opaque, hwaddr offset, unsigned size)
{
    SolarOSInputState *s = opaque;
    switch (offset) {
    case SOLAR_OS_QEMU_INPUT_REG_MAGIC: return SOLAR_OS_QEMU_INPUT_MAGIC;
    case SOLAR_OS_QEMU_INPUT_REG_VERSION: return SOLAR_OS_QEMU_INPUT_VERSION;
    case SOLAR_OS_QEMU_INPUT_REG_COUNT: return s->count;
    case SOLAR_OS_QEMU_INPUT_REG_DROPPED: return s->dropped;
    case SOLAR_OS_QEMU_INPUT_REG_GENERATION: return s->generation;
    case SOLAR_OS_QEMU_INPUT_REG_TYPE:
        if (!s->latch_valid && s->count) {
            s->latched = s->queue[s->head];
            s->latch_valid = true;
        }
        return s->latch_valid ? s->latched.type : 0;
    case SOLAR_OS_QEMU_INPUT_REG_A: return s->latch_valid ? s->latched.a : 0;
    case SOLAR_OS_QEMU_INPUT_REG_B: return s->latch_valid ? s->latched.b : 0;
    case SOLAR_OS_QEMU_INPUT_REG_C: return s->latch_valid ? s->latched.c : 0;
    case SOLAR_OS_QEMU_INPUT_REG_D: return s->latch_valid ? s->latched.d : 0;
    default: return 0;
    }
}

static void solar_os_input_write(void *opaque, hwaddr offset, uint64_t value,
                                 unsigned size)
{
    SolarOSInputState *s = opaque;
    if (offset == SOLAR_OS_QEMU_INPUT_REG_POP && s->latch_valid) {
        /* A focus reset or overflow may have replaced the queue while read. */
        if (s->count && s->queue[s->head].sequence == s->latched.sequence) {
            s->head = (s->head + 1) % QUEUE_LENGTH;
            s->count--;
        }
        s->latch_valid = false;
    } else if (offset == SOLAR_OS_QEMU_INPUT_REG_RESET) {
        s->latch_valid = false;
        solar_os_input_neutral(s);
    }
}

static const MemoryRegionOps solar_os_input_ops = {
    .read = solar_os_input_read, .write = solar_os_input_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};

static const QemuInputHandler keyboard_handler = {
    .name = "SolarOS keyboard", .mask = INPUT_EVENT_MASK_KEY,
    .event = solar_os_input_key, .focus = solar_os_input_focus,
};
static const QemuInputHandler touch_handler = {
    .name = "SolarOS touch", .mask = INPUT_EVENT_MASK_ABS | INPUT_EVENT_MASK_BTN,
    .event = solar_os_input_touch, .sync = solar_os_input_sync,
};
static const QemuInputHandler mouse_handler = {
    .name = "SolarOS mouse", .mask = INPUT_EVENT_MASK_REL | INPUT_EVENT_MASK_BTN,
    .event = solar_os_input_mouse, .sync = solar_os_input_sync,
};

static void solar_os_input_realize(DeviceState *dev, Error **errp)
{
    SolarOSInputState *s = SOLAR_OS_INPUT(dev);
    s->keyboard = qemu_input_handler_register(dev, &keyboard_handler);
    s->mouse = qemu_input_handler_register(dev, &mouse_handler);
    s->touch = qemu_input_handler_register(dev, &touch_handler);
    qemu_input_handler_activate(s->absolute ? s->touch : s->mouse);
}

static void solar_os_input_unrealize(DeviceState *dev)
{
    SolarOSInputState *s = SOLAR_OS_INPUT(dev);
    qemu_input_handler_unregister(s->keyboard);
    qemu_input_handler_unregister(s->mouse);
    qemu_input_handler_unregister(s->touch);
}

static void solar_os_input_reset(Object *obj, ResetType type)
{
    SolarOSInputState *s = SOLAR_OS_INPUT(obj);
    s->latch_valid = false;
    solar_os_input_neutral(s);
}

static void solar_os_input_init(Object *obj)
{
    SolarOSInputState *s = SOLAR_OS_INPUT(obj);
    s->focused = true;
    memory_region_init_io(&s->mmio, obj, &solar_os_input_ops, s,
                         TYPE_SOLAR_OS_INPUT, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    object_property_add_bool(obj, "focused", solar_os_input_get_focus,
                             solar_os_input_set_focus);
}

static Property solar_os_input_properties[] = {
    DEFINE_PROP_BOOL("absolute", SolarOSInputState, absolute, true),
    DEFINE_PROP_END_OF_LIST(),
};

static void solar_os_input_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    dc->realize = solar_os_input_realize;
    dc->unrealize = solar_os_input_unrealize;
    device_class_set_props(dc, solar_os_input_properties);
    rc->phases.hold = solar_os_input_reset;
}

static const TypeInfo solar_os_input_type = {
    .name = TYPE_SOLAR_OS_INPUT, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SolarOSInputState), .instance_init = solar_os_input_init,
    .class_init = solar_os_input_class_init,
};

static void solar_os_input_register(void)
{
    type_register_static(&solar_os_input_type);
}
type_init(solar_os_input_register)
