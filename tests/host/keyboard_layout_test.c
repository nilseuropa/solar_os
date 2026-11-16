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
                const uint8_t got = solar_os_input_translate_hid_usage((uint16_t)usage,
                                                                       modifiers, caps);
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
    /* Caps Lock + ß has no key-event capital, so it types ß. */
    assert(solar_os_input_translate_hid_usage(0x2d, 0, true) == INPUT_LATIN1_SHARP_S);
    /* Key codes are never produced as characters: AltGr+2 is ² (0xb2). */
    assert(solar_os_input_translate_hid_usage(0x1f, SOLAR_OS_INPUT_MOD_RIGHT_ALT, false) == 0);
    assert(solar_os_input_set_keyboard_layout(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_US) == ESP_OK);
    assert(solar_os_input_translate_hid_usage(0x28, SOLAR_OS_INPUT_MOD_RIGHT_ALT, false) == '\n');
    puts("keyboard_layout_test: ok");
    return 0;
}
