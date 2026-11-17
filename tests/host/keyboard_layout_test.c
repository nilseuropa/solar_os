/* Checks the generated us/de layouts against the hand-written tables they
 * replaced. The legacy_* functions below are a verbatim copy of those tables;
 * every difference from them must be listed in expected_change(). */
#define main input_service_tests
#include "input_test.c"
#undef main

#include <ctype.h>

#define INPUT_LATIN1_A_UMLAUT_UPPER ((uint8_t)0xc4)
#define INPUT_LATIN1_O_UMLAUT_UPPER ((uint8_t)0xd6)
#define INPUT_LATIN1_U_UMLAUT_UPPER ((uint8_t)0xdc)
#define INPUT_LATIN1_SHARP_S ((uint8_t)0xdf)
#define INPUT_LATIN1_A_UMLAUT_LOWER ((uint8_t)0xe4)
#define INPUT_LATIN1_O_UMLAUT_LOWER ((uint8_t)0xf6)
#define INPUT_LATIN1_U_UMLAUT_LOWER ((uint8_t)0xfc)

static uint8_t legacy_shifted_digit(uint16_t usage)
{
    static const uint8_t shifted[] = {'!', '@', '#', '$', '%', '^', '&', '*', '(', ')'};
    return shifted[usage - 0x1eU];
}

static uint8_t legacy_unshifted_digit(uint16_t usage)
{
    return usage == 0x27U ? '0' : (uint8_t)('1' + usage - 0x1eU);
}

static uint8_t legacy_usage_to_us(uint16_t usage, bool shift, bool caps_lock)
{
    if (usage >= 0x04U && usage <= 0x1dU) {
        const bool upper = shift ^ caps_lock;
        return (uint8_t)((upper ? 'A' : 'a') + usage - 0x04U);
    }
    if (usage >= 0x1eU && usage <= 0x27U) {
        return shift ? legacy_shifted_digit(usage) : legacy_unshifted_digit(usage);
    }
    switch (usage) {
    case 0x28: return '\n';
    case 0x29: return SOLAR_OS_KEY_ESCAPE;
    case 0x2a: return '\b';
    case 0x2b: return '\t';
    case 0x2c: return ' ';
    case 0x2d: return shift ? '_' : '-';
    case 0x2e: return shift ? '+' : '=';
    case 0x2f: return shift ? '{' : '[';
    case 0x30: return shift ? '}' : ']';
    case 0x31: return shift ? '|' : '\\';
    case 0x32: return shift ? '~' : '#';
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '"' : '\'';
    case 0x35: return shift ? '~' : '`';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
    default: return 0;
    }
}

static uint8_t legacy_usage_to_de(uint16_t usage,
                                 uint8_t modifiers,
                                 bool caps_lock)
{
    const bool shift = (modifiers & SOLAR_OS_INPUT_MOD_SHIFT) != 0;
    const bool altgr = (modifiers & SOLAR_OS_INPUT_MOD_RIGHT_ALT) != 0;
    if (altgr) {
        switch (usage) {
        case 0x28: return SOLAR_OS_KEY_ENTER;
        case 0x2b: return '\t';
        case 0x14: return '@';
        case 0x24: return '{';
        case 0x25: return '[';
        case 0x26: return ']';
        case 0x27: return '}';
        case 0x2d: return '\\';
        case 0x30: return '~';
        case 0x64: return '|';
        default: return 0;
        }
    }
    if (usage >= 0x04U && usage <= 0x1dU) {
        uint8_t base = (uint8_t)('a' + usage - 0x04U);
        if (base == 'y') {
            base = 'z';
        } else if (base == 'z') {
            base = 'y';
        }
        return (shift ^ caps_lock) ? (uint8_t)toupper(base) : base;
    }
    if (usage >= 0x1eU && usage <= 0x27U) {
        static const uint8_t shifted[] = {'!', '"', '#', '$', '%', '&', '/', '(', ')', '='};
        return shift ? shifted[usage - 0x1eU] : legacy_unshifted_digit(usage);
    }
    switch (usage) {
    case 0x28: return '\n';
    case 0x29: return SOLAR_OS_KEY_ESCAPE;
    case 0x2a: return '\b';
    case 0x2b: return '\t';
    case 0x2c: return ' ';
    case 0x2d: return shift ? '?' : INPUT_LATIN1_SHARP_S;
    case 0x2e: return shift ? '`' : 0;
    case 0x2f: return shift ? INPUT_LATIN1_U_UMLAUT_UPPER : INPUT_LATIN1_U_UMLAUT_LOWER;
    case 0x30: return shift ? '*' : '+';
    case 0x31:
    case 0x32: return shift ? '\'' : '#';
    case 0x33: return shift ? INPUT_LATIN1_O_UMLAUT_UPPER : INPUT_LATIN1_O_UMLAUT_LOWER;
    case 0x34: return shift ? INPUT_LATIN1_A_UMLAUT_UPPER : INPUT_LATIN1_A_UMLAUT_LOWER;
    case 0x35: return shift ? 0 : '^';
    case 0x36: return shift ? ';' : ',';
    case 0x37: return shift ? ':' : '.';
    case 0x38: return shift ? '_' : '-';
    case 0x64: return shift ? '>' : '<';
    default: return 0;
    }
}

typedef struct {
    solar_os_input_keyboard_layout_t layout;
    uint8_t usage;
    uint8_t level; /* 0 base, 1 Shift, 2 AltGr, 3 Shift+AltGr */
    bool caps;
    uint8_t now;   /* what the XKB-generated table produces instead */
} layout_change_t;

/* Characters the hand-written tables did not have, taken from XKB. */
static const layout_change_t added[] = {
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x04, 2, false, 0xe6}, /* æ */
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x04, 2, true, 0xc6},
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x04, 3, false, 0xc6}, /* Æ */
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x04, 3, true, 0xe6},
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x07, 2, false, 0xf0}, /* ð */
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x07, 2, true, 0xd0},
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x07, 3, false, 0xd0}, /* Ð */
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x07, 3, true, 0xf0},
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x0c, 3, true, 'I'},   /* Caps Lock + dotless i */
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x0e, 3, false, '&'},
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x0e, 3, true, '&'},
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x12, 2, false, 0xf8}, /* ø */
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x12, 2, true, 0xd8},
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x12, 3, false, 0xd8}, /* Ø */
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x12, 3, true, 0xf8},
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x13, 2, false, 0xfe}, /* þ */
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x13, 2, true, 0xde},
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x13, 3, false, 0xde}, /* Þ */
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x13, 3, true, 0xfe},
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x1a, 2, true, 'S'},   /* Caps Lock + long s */
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x2e, 0, false, '\''}, /* dead acute */
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x2e, 0, true, '\''},
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x2f, 2, false, '"'},  /* dead diaeresis */
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x2f, 2, true, '"'},
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x2c, 2, false, ' '},
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x2c, 2, true, ' '},
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x2c, 3, false, ' '},
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x2c, 3, true, ' '},
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x34, 2, false, '^'},  /* dead circumflex */
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x34, 2, true, '^'},
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x36, 3, false, 0xd7}, /* × */
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x36, 3, true, 0xd7},
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x37, 3, false, 0xf7}, /* ÷ */
    {SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x37, 3, true, 0xf7},
};

static bool in_list(uint8_t usage, const uint8_t *list, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (list[i] == usage) {
            return true;
        }
    }
    return false;
}

/* Returns true and sets *now when the generated table may differ. */
static bool expected_change(solar_os_input_keyboard_layout_t layout, uint8_t usage,
                            unsigned level, bool caps, uint8_t old, uint8_t *now)
{
    for (size_t i = 0; i < sizeof(added) / sizeof(added[0]); i++) {
        if (added[i].layout == layout && added[i].usage == usage &&
            added[i].level == level && added[i].caps == caps) {
            *now = added[i].now;
            return true;
        }
    }
    if (layout == SOLAR_OS_INPUT_KEYBOARD_LAYOUT_US) {
        /* ISO keys: XKB gives \ | next to Enter (the kernel reports it as
         * backslash) and < > left of Z, instead of # ~ and nothing. */
        if (usage == 0x32U) {
            *now = (level & 1U) ? '|' : '\\';
            return true;
        }
        if (usage == 0x64U) {
            *now = (level & 1U) ? '>' : '<';
            return true;
        }
        return false;
    }
    /* German umlauts follow Caps Lock like the other letters. */
    static const uint8_t umlauts[] = {0x2f, 0x33, 0x34};
    if (caps && level < 2U && in_list(usage, umlauts, sizeof(umlauts))) {
        *now = (uint8_t)(old ^ 0x20U);
        return true;
    }
    /* Shift+3 is § in XKB, which a key event cannot carry (0xa7 is a key code). */
    if (usage == 0x20U && level == 1U) {
        *now = 0;
        return true;
    }
    /* Shift+AltGr no longer falls back to AltGr: XKB has Ω ⅞ ™ ± ° ¿ ¯ and a
     * dead key there, none of which a key event can carry. */
    static const uint8_t shift_altgr[] = {0x14, 0x24, 0x25, 0x26, 0x27, 0x2d, 0x30, 0x64};
    if (level == 3U && in_list(usage, shift_altgr, sizeof(shift_altgr))) {
        *now = 0;
        return true;
    }
    return false;
}

static uint8_t legacy_char(solar_os_input_keyboard_layout_t layout, uint8_t usage,
                           uint8_t modifiers, bool caps)
{
    if (layout == SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE) {
        return legacy_usage_to_de(usage, modifiers, caps);
    }
    return legacy_usage_to_us(usage, (modifiers & SOLAR_OS_INPUT_MOD_SHIFT) != 0, caps);
}

/* What a key event delivers to a byte consumer, and its text. */
static uint8_t typed(uint16_t usage, uint8_t modifiers, bool caps, solar_os_input_text_t *text)
{
    solar_os_input_key_event_t event = {
        .source = 1, .physical_key = usage, .usage = usage,
        .key = solar_os_input_translate_hid_usage(usage, modifiers, caps),
        .modifiers = modifiers, .action = SOLAR_OS_INPUT_KEY_PRESS,
    };
    const uint8_t code = event.key;
    const solar_os_input_text_t resolved = solar_os_input_resolve_layout_key(&event);
    if (code == SOLAR_OS_KEY_LAYOUT_TEXT || code == SOLAR_OS_KEY_LAYOUT_TEXT_CAPS) {
        /* Layout codes only appear for text the byte cannot carry. */
        assert((code == SOLAR_OS_KEY_LAYOUT_TEXT_CAPS) == caps);
        assert(resolved.dead != 0U || resolved.codepoint != event.key);
    } else {
        assert(event.key == code);
    }
    if (text != NULL) {
        *text = resolved;
    }
    return event.key;
}

static uint16_t text_of(uint16_t usage, uint8_t modifiers, bool caps)
{
    solar_os_input_text_t text;
    (void)typed(usage, modifiers, caps, &text);
    assert(text.dead == 0U);
    return text.codepoint;
}

static uint8_t dead_of(uint16_t usage, uint8_t modifiers)
{
    solar_os_input_text_t text;
    (void)typed(usage, modifiers, false, &text);
    assert(text.dead != 0U);
    return text.dead;
}

/* Feeds the texts and checks the code points typed by the last one. */
static void expect_compose(solar_os_input_compose_t *state, solar_os_input_text_t text,
                           size_t count, uint16_t first, uint16_t second)
{
    uint16_t out[2] = {0, 0};
    const size_t got = solar_os_input_compose_feed(state, text, out);
    if (got != count || (count > 0 && out[0] != first) || (count > 1 && out[1] != second)) {
        fprintf(stderr, "compose: got %zu U+%04X U+%04X, want %zu U+%04X U+%04X\n",
                got, out[0], out[1], count, first, second);
        assert(false);
    }
}

static solar_os_input_text_t ch(uint16_t codepoint)
{
    return (solar_os_input_text_t){.codepoint = codepoint};
}

static solar_os_input_text_t dead(uint8_t index)
{
    return (solar_os_input_text_t){.dead = index};
}

static void check_dead_keys(void)
{
    solar_os_input_compose_t state = {0};

    assert(solar_os_input_set_keyboard_layout(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_FR) == ESP_OK);
    const uint8_t circumflex = dead_of(0x2f, 0);   /* ^ key right of P */
    const uint8_t diaeresis = dead_of(0x2f, SOLAR_OS_INPUT_MOD_LEFT_SHIFT);
    /* Byte consumers get the spacing character, as before. */
    assert(typed(0x2f, 0, false, NULL) == '^');
    assert(typed(0x2f, SOLAR_OS_INPUT_MOD_LEFT_SHIFT, false, NULL) == '"');

    expect_compose(&state, dead(circumflex), 0, 0, 0);
    expect_compose(&state, ch('e'), 1, 0x00ea, 0);            /* ê */
    expect_compose(&state, dead(circumflex), 0, 0, 0);
    expect_compose(&state, ch('E'), 1, 0x00ca, 0);            /* Ê */
    expect_compose(&state, dead(diaeresis), 0, 0, 0);
    expect_compose(&state, ch('i'), 1, 0x00ef, 0);            /* ï */
    expect_compose(&state, dead(circumflex), 0, 0, 0);
    expect_compose(&state, ch(' '), 1, '^', 0);               /* dead key + space */
    expect_compose(&state, dead(circumflex), 0, 0, 0);
    expect_compose(&state, dead(circumflex), 1, '^', 0);      /* twice */
    expect_compose(&state, dead(circumflex), 0, 0, 0);
    expect_compose(&state, ch('x'), 2, '^', 'x');             /* no composition */
    expect_compose(&state, dead(circumflex), 0, 0, 0);
    expect_compose(&state, dead(diaeresis), 1, '^', 0);       /* another dead key */
    expect_compose(&state, ch('u'), 1, 0x00fc, 0);            /* ü */
    expect_compose(&state, ch('a'), 1, 'a', 0);               /* nothing pending */
    expect_compose(&state, ch(0), 0, 0, 0);

    assert(solar_os_input_set_keyboard_layout(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE) == ESP_OK);
    expect_compose(&state, dead(dead_of(0x2e, 0)), 0, 0, 0);  /* ´ */
    expect_compose(&state, ch('e'), 1, 0x00e9, 0);            /* é */
    expect_compose(&state, dead(dead_of(0x35, 0)), 0, 0, 0);  /* ^ */
    expect_compose(&state, ch('o'), 1, 0x00f4, 0);            /* ô */

    assert(solar_os_input_set_keyboard_layout(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_RO) == ESP_OK);
    /* AltGr+S is ș; the Romanian layout has no dead key there. */
    assert(text_of(0x16, SOLAR_OS_INPUT_MOD_RIGHT_ALT, false) == 0x0219);
    assert(typed(0x16, SOLAR_OS_INPUT_MOD_RIGHT_ALT, false, NULL) == 0);
}

/* Every built-in layout: layout codes only for text a byte cannot carry, and
 * the byte is the text whenever a byte can carry it. */
static void check_invariants(void)
{
    for (unsigned layout = 0; layout <= SOLAR_OS_INPUT_KEYBOARD_LAYOUT_RO; layout++) {
        assert(solar_os_input_set_keyboard_layout(layout) == ESP_OK);
        for (unsigned usage = 0x04; usage <= 0x89; usage++) {
            for (unsigned modifiers = 0; modifiers < 0x100U; modifiers += 0x11U) {
                for (int caps = 0; caps < 2; caps++) {
                    solar_os_input_text_t text;
                    const uint8_t byte = typed((uint16_t)usage, (uint8_t)modifiers, caps, &text);
                    if (text.dead == 0U && ((text.codepoint >= 0x20U && text.codepoint < 0x7fU) ||
                                            (text.codepoint >= 0xc0U && text.codepoint <= 0xffU))) {
                        assert(byte == text.codepoint);
                    }
                }
            }
        }
    }
}

/* Press and release one key from a keyboard source, as a driver does. */
static void press(solar_os_input_source_t keyboard, uint16_t usage, uint8_t modifiers, bool caps)
{
    const uint8_t key = solar_os_input_translate_hid_usage(usage, modifiers, caps);
    assert(solar_os_input_write_key(keyboard, usage, usage, key, modifiers,
                                    SOLAR_OS_INPUT_KEY_PRESS) == ESP_OK);
    assert(solar_os_input_write_key(keyboard, usage, usage, key, modifiers,
                                    SOLAR_OS_INPUT_KEY_RELEASE) == ESP_OK);
}

/* Byte readers (shell, scripts, BLE) never see the layout codes. */
static void check_byte_readers(void)
{
    solar_os_input_source_t keyboard = SOLAR_OS_INPUT_SOURCE_INVALID;
    assert(solar_os_input_keyboard_source_open("layout-bytes", true, &keyboard) == ESP_OK);
    assert(solar_os_input_set_keyboard_layout(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE) == ESP_OK);
    char chars[8];
    while (solar_os_input_read_chars(chars, sizeof(chars)) != 0) {
    }

    press(keyboard, 0x35, 0, false);   /* dead ^ */
    press(keyboard, 0x2d, 0, true);    /* Caps Lock + ß */
    press(keyboard, 0x1f, SOLAR_OS_INPUT_MOD_RIGHT_ALT, false);  /* ², no byte */
    press(keyboard, 0x04, 0, false);
    assert(solar_os_input_read_source_chars(keyboard, chars, sizeof(chars)) == 3);
    assert((uint8_t)chars[0] == '^' && (uint8_t)chars[1] == INPUT_LATIN1_SHARP_S &&
           chars[2] == 'a');

    solar_os_input_source_diagnostics_t diagnostics;
    assert(solar_os_input_source_get_diagnostics(keyboard, &diagnostics));
    assert(diagnostics.has_key && diagnostics.last_key.key == 'a');
    press(keyboard, 0x35, 0, false);
    assert(solar_os_input_source_get_diagnostics(keyboard, &diagnostics));
    assert(diagnostics.last_key.key == '^');

    /* A reader of another source leaves the key for the next reader intact. */
    solar_os_input_source_t other = SOLAR_OS_INPUT_SOURCE_INVALID;
    assert(solar_os_input_keyboard_source_open("layout-other", true, &other) == ESP_OK);
    assert(solar_os_input_read_source_chars(other, chars, sizeof(chars)) == 0);
    solar_os_input_source_close(other);
    solar_os_input_key_event_t events[4];
    const size_t count = solar_os_input_read_events(events, 4);
    assert(count == 2 && events[0].key == SOLAR_OS_KEY_LAYOUT_TEXT);

    assert(solar_os_input_translate_hid_usage_byte(0x35, 0, false) == '^');
    assert(solar_os_input_translate_hid_usage_byte(0x2d, 0, true) == INPUT_LATIN1_SHARP_S);
    assert(solar_os_input_translate_hid_usage_byte(0x04, 0, false) == 'a');
    solar_os_input_source_close(keyboard);
}

/* A release delivers the byte its press delivered, even when the modifiers
 * differ (AltGr released before the key). */
static void check_release_matches_press(void)
{
    solar_os_input_source_t keyboard = SOLAR_OS_INPUT_SOURCE_INVALID;
    assert(solar_os_input_keyboard_source_open("layout-release", true, &keyboard) == ESP_OK);
    assert(solar_os_input_set_keyboard_layout(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE) == ESP_OK);
    solar_os_input_key_event_t events[8];
    while (solar_os_input_read_events(events, 8) != 0) {
    }
    static const struct { uint16_t usage; uint8_t modifiers; bool caps; } keys[] = {
        {0x1f, SOLAR_OS_INPUT_MOD_RIGHT_ALT, false},                          /* ² */
        {0x35, 0, false},                                                     /* dead ^ */
        {0x2d, 0, true},                                                      /* ẞ */
        {0x20, SOLAR_OS_INPUT_MOD_LEFT_SHIFT, false},                         /* § */
        {0x04, SOLAR_OS_INPUT_MOD_RIGHT_ALT, false},                          /* æ */
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const uint8_t code = solar_os_input_translate_hid_usage(keys[i].usage, keys[i].modifiers,
                                                                keys[i].caps);
        assert(solar_os_input_write_key(keyboard, keys[i].usage, keys[i].usage, code,
                                        keys[i].modifiers, SOLAR_OS_INPUT_KEY_PRESS) == ESP_OK);
        assert(solar_os_input_write_key(keyboard, keys[i].usage, keys[i].usage, code, 0,
                                        SOLAR_OS_INPUT_KEY_RELEASE) == ESP_OK);
        assert(solar_os_input_read_events(events, 8) == 2);
        const uint8_t press = events[0].key;
        solar_os_input_key_event_t press_event = events[0], release_event = events[1];
        (void)solar_os_input_resolve_layout_key(&press_event);
        (void)solar_os_input_resolve_layout_key(&release_event);
        assert(press_event.action == SOLAR_OS_INPUT_KEY_PRESS);
        assert(release_event.action == SOLAR_OS_INPUT_KEY_RELEASE);
        if (press_event.key != release_event.key) {
            fprintf(stderr, "usage 0x%02x: press 0x%02x (code 0x%02x), release 0x%02x\n",
                    keys[i].usage, press_event.key, press, release_event.key);
            assert(press_event.key == release_event.key);
        }
    }
    /* Releasing every key at once (a source going away) does the same. */
    assert(solar_os_input_write_key(keyboard, 0x1f, 0x1f,
                                    solar_os_input_translate_hid_usage(
                                        0x1f, SOLAR_OS_INPUT_MOD_RIGHT_ALT, false),
                                    SOLAR_OS_INPUT_MOD_RIGHT_ALT,
                                    SOLAR_OS_INPUT_KEY_PRESS) == ESP_OK);
    solar_os_input_source_release_all(keyboard);
    assert(solar_os_input_read_events(events, 8) == 2);
    solar_os_input_key_event_t held = events[0], released = events[1];
    (void)solar_os_input_resolve_layout_key(&held);
    (void)solar_os_input_resolve_layout_key(&released);
    assert(held.key == released.key);
    solar_os_input_source_close(keyboard);
}

/* The input route owner (on-screen keyboard) gets byte keys too. */
static void check_route_owner(void)
{
    solar_os_input_source_t keyboard = SOLAR_OS_INPUT_SOURCE_INVALID;
    assert(solar_os_input_keyboard_source_open("layout-route", true, &keyboard) == ESP_OK);
    assert(solar_os_input_set_keyboard_layout(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE) == ESP_OK);
    int owner;
    const solar_os_input_route_config_t config = {
        .target = "display0", .trigger_source = keyboard, .trigger_physical_key = 0x35,
        .native_width = 320, .native_height = 240, .scale = 1,
        .x = 0, .y = 120, .width = 320, .height = 120,
    };
    assert(solar_os_input_route_claim(&owner, &config) == ESP_OK);
    assert(solar_os_input_write_key(keyboard, 0x35, 0x35, SOLAR_OS_KEY_LAYOUT_TEXT, 0,
                                    SOLAR_OS_INPUT_KEY_PRESS) == ESP_OK);
    solar_os_input_route_event_t event;
    bool reset = false;
    assert(solar_os_input_route_read(&owner, &event, &reset));
    assert(event.type == SOLAR_OS_INPUT_ROUTE_KEY && event.data.key.usage == 0x35);
    assert(event.data.key.key == '^');
    solar_os_input_route_release(&owner);
    solar_os_input_source_close(keyboard);
}

static void check_ukrainian(void)
{
    assert(solar_os_input_set_keyboard_layout(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_UA) == ESP_OK);
    assert(text_of(0x04, 0, false) == 0x0444);                              /* ф */
    assert(text_of(0x04, SOLAR_OS_INPUT_MOD_LEFT_SHIFT, false) == 0x0424);  /* Ф */
    assert(text_of(0x04, 0, true) == 0x0424);
    assert(text_of(0x04, SOLAR_OS_INPUT_MOD_LEFT_SHIFT, true) == 0x0444);
    assert(text_of(0x31, 0, false) == 0x0491);                              /* ґ */
    assert(text_of(0x30, 0, false) == 0x0457);                              /* ї */
    assert(typed(0x04, 0, false, NULL) == 0);
    assert(typed(0x1e, 0, false, NULL) == '1');
    assert(typed(0x2c, 0, false, NULL) == ' ');
    /* Shortcuts use the US letter on Cyrillic keys. */
    assert(typed(0x04, SOLAR_OS_INPUT_MOD_LEFT_CTRL, false, NULL) == 0x01);
    assert(typed(0x1d, SOLAR_OS_INPUT_MOD_LEFT_CTRL, false, NULL) == 0x1a);
    assert(typed(0x04, SOLAR_OS_INPUT_MOD_LEFT_ALT, false, NULL) == 'a');
    assert(typed(0x04, SOLAR_OS_INPUT_MOD_LEFT_GUI | SOLAR_OS_INPUT_MOD_LEFT_SHIFT,
                 false, NULL) == 'A');
    assert(text_of(0x04, SOLAR_OS_INPUT_MOD_LEFT_ALT, false) == 'a');
    /* Ctrl with a Cyrillic key that is not a letter types the US character
     * too, which Ctrl then turns into its control code (Ctrl+[ is Escape). */
    assert(typed(0x2f, SOLAR_OS_INPUT_MOD_LEFT_CTRL, false, NULL) == '[');
    assert(text_of(0x2f, SOLAR_OS_INPUT_MOD_LEFT_CTRL, false) == '[');
    assert(typed(0x30, SOLAR_OS_INPUT_MOD_RIGHT_CTRL, false, NULL) == 0x1d); /* Ctrl+] */
    assert(typed(0x33, SOLAR_OS_INPUT_MOD_LEFT_CTRL, false, NULL) == ';');
    assert(solar_os_input_translate_char_key('[', SOLAR_OS_INPUT_MOD_LEFT_CTRL) == 0x1b);
    /* Without the modifier the key still types its own letter. */
    assert(text_of(0x2f, 0, false) == 0x0445);
}

static void check_shortcut_letters(void)
{
    /* Ctrl+letter follows the letter on the key. */
    assert(solar_os_input_set_keyboard_layout(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_FR) == ESP_OK);
    assert(typed(0x14, SOLAR_OS_INPUT_MOD_LEFT_CTRL, false, NULL) == 0x01); /* A */
    assert(typed(0x04, SOLAR_OS_INPUT_MOD_LEFT_CTRL, false, NULL) == 0x11); /* Q */
    assert(typed(0x1a, SOLAR_OS_INPUT_MOD_LEFT_CTRL, false, NULL) == 0x1a); /* Z */
    assert(typed(0x1d, SOLAR_OS_INPUT_MOD_LEFT_CTRL, false, NULL) == 0x17); /* W */
    assert(typed(0x33, SOLAR_OS_INPUT_MOD_LEFT_CTRL, false, NULL) == 0x0d); /* M */
    assert(typed(0x10, SOLAR_OS_INPUT_MOD_LEFT_CTRL, false, NULL) == ',');  /* , key */
    assert(solar_os_input_set_keyboard_layout(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE) == ESP_OK);
    assert(typed(0x1c, SOLAR_OS_INPUT_MOD_LEFT_CTRL, false, NULL) == 0x1a); /* Z */
    assert(typed(0x1d, SOLAR_OS_INPUT_MOD_LEFT_CTRL, false, NULL) == 0x19); /* Y */
    assert(solar_os_input_set_keyboard_layout(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_US) == ESP_OK);
    for (uint16_t usage = 0x04; usage <= 0x1d; usage++) {
        assert(typed(usage, SOLAR_OS_INPUT_MOD_RIGHT_CTRL, false, NULL) == usage - 0x04U + 1U);
    }
}

static void check_layout(solar_os_input_keyboard_layout_t layout)
{
    /* Both shifts and left/right Alt, GUI: only Shift and right Alt matter. */
    static const uint8_t modifier_sets[] = {
        0x00, SOLAR_OS_INPUT_MOD_LEFT_SHIFT, SOLAR_OS_INPUT_MOD_RIGHT_SHIFT,
        SOLAR_OS_INPUT_MOD_RIGHT_ALT, SOLAR_OS_INPUT_MOD_LEFT_SHIFT | SOLAR_OS_INPUT_MOD_RIGHT_ALT,
        SOLAR_OS_INPUT_MOD_LEFT_ALT, SOLAR_OS_INPUT_MOD_LEFT_GUI,
        SOLAR_OS_INPUT_MOD_LEFT_SHIFT | SOLAR_OS_INPUT_MOD_LEFT_ALT,
    };
    unsigned changed = 0;
    assert(solar_os_input_set_keyboard_layout(layout) == ESP_OK);
    for (unsigned usage = 0x04; usage <= 0x64; usage++) {
        if (usage > 0x38U && usage < 0x64U) {
            continue; /* function, navigation and keypad keys are not layout data */
        }
        for (size_t m = 0; m < sizeof(modifier_sets); m++) {
            for (int caps = 0; caps < 2; caps++) {
                const uint8_t modifiers = modifier_sets[m];
                const bool shift = (modifiers & SOLAR_OS_INPUT_MOD_SHIFT) != 0;
                const bool altgr = layout == SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE &&
                    (modifiers & SOLAR_OS_INPUT_MOD_RIGHT_ALT) != 0;
                const unsigned level = (shift ? 1U : 0U) + (altgr ? 2U : 0U);
                const uint8_t old = legacy_char(layout, (uint8_t)usage, modifiers, caps);
                const uint8_t got = typed((uint16_t)usage, modifiers, caps, NULL);
                uint8_t want = old;
                if (expected_change(layout, (uint8_t)usage, level, caps, old, &want)) {
                    changed += want != old;
                }
                if (got != want) {
                    fprintf(stderr, "%s usage 0x%02x modifiers 0x%02x caps %d: "
                            "got 0x%02x, want 0x%02x (legacy 0x%02x)\n",
                            solar_os_input_keyboard_layout_name(layout), usage,
                            modifiers, caps, got, want, old);
                    assert(got == want);
                }
            }
        }
    }
    assert(changed > 0);
}

int main(void)
{
    assert(solar_os_input_init() == ESP_OK);
    check_layout(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_US);
    check_layout(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE);

    /* Layout-independent keys and the German AltGr exceptions stay in C. */
    assert(solar_os_input_translate_hid_usage(0x28, SOLAR_OS_INPUT_MOD_RIGHT_ALT, false) ==
           SOLAR_OS_KEY_ENTER);
    assert(solar_os_input_translate_hid_usage(0x2a, SOLAR_OS_INPUT_MOD_RIGHT_ALT, false) == 0);
    /* Caps Lock + ß has no key-byte capital, so byte consumers get ß; text is ẞ. */
    assert(typed(0x2d, 0, true, NULL) == INPUT_LATIN1_SHARP_S);
    assert(text_of(0x2d, 0, true) == 0x1e9e);
    /* Key codes are never produced as characters: AltGr+2 is ² (0xb2), which
     * only text carries. Shift+3 is §. */
    assert(typed(0x1f, SOLAR_OS_INPUT_MOD_RIGHT_ALT, false, NULL) == 0);
    assert(text_of(0x1f, SOLAR_OS_INPUT_MOD_RIGHT_ALT, false) == 0x00b2);
    assert(text_of(0x20, SOLAR_OS_INPUT_MOD_LEFT_SHIFT, false) == 0x00a7);
    assert(solar_os_input_set_keyboard_layout(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_US) == ESP_OK);
    assert(solar_os_input_translate_hid_usage(0x28, SOLAR_OS_INPUT_MOD_RIGHT_ALT, false) == '\n');
    /* Plain bytes are their own text. */
    assert(text_of(0x04, 0, false) == 'a');
    solar_os_input_key_event_t nav = {.key = SOLAR_OS_KEY_UP};
    assert(solar_os_input_resolve_layout_key(&nav).codepoint == 0 && nav.key == SOLAR_OS_KEY_UP);

    check_invariants();
    check_byte_readers();
    check_release_matches_press();
    check_route_owner();
    check_dead_keys();
    check_ukrainian();
    check_shortcut_letters();
    assert(solar_os_input_set_keyboard_layout(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_US) == ESP_OK);
    puts("keyboard_layout_test: ok");
    return 0;
}
