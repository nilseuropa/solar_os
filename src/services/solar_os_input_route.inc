/* Private part of service.input; input_lock protects every field. */
#define INPUT_ROUTE_MAX 32U
#define INPUT_ROUTE_HELD_MAX (SOLAR_OS_INPUT_MAX_PRESSED_KEYS + INPUT_SOURCE_MAX)
static struct {
    const void *owner;
    solar_os_input_route_config_t config;
    solar_os_input_route_event_t *queue;
    size_t head, count;
    bool reset;
    uint32_t generation;
    struct { solar_os_input_source_t source; uint16_t physical; uint8_t key; uint32_t generation; } held[INPUT_ROUTE_HELD_MAX];
    bool pointer_active[INPUT_SOURCE_MAX], touch_owned[INPUT_SOURCE_MAX];
    uint8_t contacts[INPUT_SOURCE_MAX][32];
    uint16_t contact_count[INPUT_SOURCE_MAX];
    bool pointer_blocked[INPUT_SOURCE_MAX];
    uint32_t pointer_generation[INPUT_SOURCE_MAX];
} input_route;

static void input_route_push_locked(const solar_os_input_route_event_t *event)
{
    if (!input_route.owner) return;
    if (event->type == SOLAR_OS_INPUT_ROUTE_POINTER &&
        event->data.pointer.event.action == SOLAR_OS_INPUT_POINTER_MOVE && input_route.count) {
        size_t tail = (input_route.head + input_route.count - 1U) % INPUT_ROUTE_MAX;
        solar_os_input_route_event_t *last = &input_route.queue[tail];
        if (last->type == event->type && last->data.pointer.event.action == SOLAR_OS_INPUT_POINTER_MOVE &&
            last->data.pointer.event.source == event->data.pointer.event.source) {
            *last = *event;
            return;
        }
    }
    if (input_route.count == INPUT_ROUTE_MAX) {
        input_route.head = input_route.count = 0;
        input_route.reset = true;
    }
    input_route.queue[(input_route.head + input_route.count++) % INPUT_ROUTE_MAX] = *event;
    input_wake_generation++;
    input_capture_activity = true;
}

static unsigned input_route_modifiers(uint8_t mods)
{
    return ((mods & SOLAR_OS_INPUT_MOD_CTRL) ? 1U : 0U) |
        ((mods & SOLAR_OS_INPUT_MOD_SHIFT) ? 2U : 0U) |
        ((mods & SOLAR_OS_INPUT_MOD_ALT) ? 4U : 0U) |
        ((mods & (SOLAR_OS_INPUT_MOD_LEFT_GUI | SOLAR_OS_INPUT_MOD_RIGHT_GUI)) ? 8U : 0U);
}

static bool input_route_key_locked(const solar_os_input_key_event_t *key)
{
    const solar_os_input_route_config_t *c = &input_route.config;
    bool consume = false;
    bool deliver = true;
    size_t empty = INPUT_ROUTE_HELD_MAX;
    if (!key->physical_key && key->action==SOLAR_OS_INPUT_KEY_PRESS)
        for (size_t i=0;i<INPUT_ROUTE_HELD_MAX;i++)
            if (input_route.held[i].source==key->source && !input_route.held[i].physical)
                input_route.held[i].source=0;
    for (size_t i = 0; i < INPUT_ROUTE_HELD_MAX; i++) {
        if (!input_route.held[i].source) { empty = i; continue; }
        if (input_route.held[i].source == key->source && input_route.held[i].physical == key->physical_key &&
            (key->physical_key || input_route.held[i].key == key->key)) {
            consume = true;
            deliver = input_route.held[i].generation==input_route.generation;
            if (key->action == SOLAR_OS_INPUT_KEY_RELEASE) input_route.held[i].source = 0;
            break;
        }
    }
    if (!consume && input_route.owner && key->source != c->excluded_source &&
        key->action == SOLAR_OS_INPUT_KEY_PRESS) {
        consume = (!c->trigger_source || c->trigger_source == key->source) &&
            (c->trigger_physical_key ? c->trigger_physical_key == key->physical_key :
                c->trigger_key && c->trigger_key == key->key) &&
            input_route_modifiers(c->trigger_modifiers) == input_route_modifiers(key->modifiers);
        if (c->navigation && key->modifiers == 0) {
            consume |= key->key == SOLAR_OS_KEY_LEFT || key->key == SOLAR_OS_KEY_RIGHT ||
                key->key == SOLAR_OS_KEY_UP || key->key == SOLAR_OS_KEY_DOWN ||
                key->key == '\r' || key->key == '\n' || key->key == '\t' || key->key == SOLAR_OS_KEY_ESCAPE;
        }
        if (consume && key->action == SOLAR_OS_INPUT_KEY_PRESS && empty < INPUT_ROUTE_HELD_MAX) {
            input_route.held[empty].source = key->source;
            input_route.held[empty].physical = key->physical_key;
            input_route.held[empty].key = key->key;
            input_route.held[empty].generation = input_route.generation;
        }
    }
    if (consume && deliver) {
        const solar_os_input_route_event_t event = {.type=SOLAR_OS_INPUT_ROUTE_KEY, .data.key=*key};
        input_route_push_locked(&event);
    }
    return consume;
}

static bool input_route_pointer_locked(const solar_os_input_pointer_sample_t *sample)
{
    const solar_os_input_pointer_event_t *p = &sample->event;
    const size_t i = p->source - 1U;
    const solar_os_input_route_config_t *c = &input_route.config;
    if (p->mode != SOLAR_OS_INPUT_POINTER_ABSOLUTE ||
        input_sources[i].source_class != SOLAR_OS_INPUT_SOURCE_TOUCH) return false;
    uint8_t *contact = &input_route.contacts[i][p->pointer_id / 8U];
    const uint8_t mask = (uint8_t)(1U << (p->pointer_id & 7U));
    if (p->action == SOLAR_OS_INPUT_POINTER_PRESS && !(*contact & mask)) {
        *contact |= mask;
        if (input_route.contact_count[i]++) {
            /* One owner keeps the entire contact batch. A second finger
             * cancels selection until every contact is up. */
            if (input_route.pointer_active[i]) {
                input_route.pointer_blocked[i] = true;
                input_route.head = input_route.count = 0;
                input_route.reset = true;
                input_wake_generation++;
                input_capture_activity = true;
            }
        } else {
            bool selected = false;
            if (input_route.owner && p->mode == SOLAR_OS_INPUT_POINTER_ABSOLUTE &&
                input_sources[i].source_class == SOLAR_OS_INPUT_SOURCE_TOUCH &&
                (!c->touch_source || c->touch_source == p->source) && strcmp(c->target, p->target) == 0) {
                solar_os_input_pointer_event_t oriented = *p;
                uint16_t nw = c->native_width, nh = c->native_height;
                if (input_diagnostics[i].calibration_enabled) {
                    nw = input_diagnostics[i].calibration.width;
                    nh = input_diagnostics[i].calibration.height;
                }
                unsigned width = (c->orientation == 90 || c->orientation == 270) ? nh : nw;
                unsigned height = (c->orientation == 90 || c->orientation == 270) ? nw : nh;
                if (solar_os_input_pointer_apply_display_transform(&oriented, nw, nh, c->orientation, 1) == ESP_OK &&
                    oriented.x >= 0 && oriented.y >= 0 && (unsigned)oriented.x < width && (unsigned)oriented.y < height) {
                    unsigned scale = c->scale ? c->scale : 1;
                    unsigned x = oriented.x/scale, y = oriented.y/scale;
                    selected = c->visible && x >= c->x && x < c->x+c->width && y >= c->y && y < c->y+c->height;
                    unsigned band = ((nw < nh ? nw : nh) + 19U)/20U;
                    if (band < INPUT_TOUCH_EDGE_MIN) band = INPUT_TOUCH_EDGE_MIN;
                    if (band > INPUT_TOUCH_EDGE_MAX) band = INPUT_TOUCH_EDGE_MAX;
                    if (c->edge_enabled) {
                        selected |= (c->edge_direction == SOLAR_OS_INPUT_GESTURE_DIRECTION_NORTH && (unsigned)oriented.y+band >= height) ||
                            (c->edge_direction == SOLAR_OS_INPUT_GESTURE_DIRECTION_SOUTH && (unsigned)oriented.y < band) ||
                            (c->edge_direction == SOLAR_OS_INPUT_GESTURE_DIRECTION_EAST && (unsigned)oriented.x < band) ||
                            (c->edge_direction == SOLAR_OS_INPUT_GESTURE_DIRECTION_WEST && (unsigned)oriented.x+band >= width);
                    }
                }
            }
            if (selected != input_route.touch_owned[i]) memset(&input_sources[i].touch, 0, sizeof(input_sources[i].touch));
            input_route.touch_owned[i] = selected;
            input_route.pointer_active[i] = selected;
            input_route.pointer_blocked[i] = false;
            input_route.pointer_generation[i] = input_route.generation;
        }
    }
    bool consume = input_route.pointer_active[i];
    if (consume) {
        const solar_os_input_route_event_t event = {.type=SOLAR_OS_INPUT_ROUTE_POINTER, .data.pointer=*sample};
        if (!input_route.pointer_blocked[i] && input_route.pointer_generation[i]==input_route.generation)
            input_route_push_locked(&event);
    }
    if (p->action == SOLAR_OS_INPUT_POINTER_RELEASE && (*contact & mask)) {
        *contact &= (uint8_t)~mask;
        if (--input_route.contact_count[i] == 0) input_route.pointer_active[i] = false;
    }
    return consume;
}

static bool input_route_config_valid(const solar_os_input_route_config_t *c)
{
    if (!c || !c->target[0] || !memchr(c->target, 0, sizeof(c->target)) ||
        !c->native_width || !c->native_height || !c->scale ||
        (c->orientation != 0 && c->orientation != 90 && c->orientation != 180 && c->orientation != 270) ||
        (c->trigger_physical_key && !c->trigger_source)) return false;
    if (c->edge_enabled && (c->edge_direction < SOLAR_OS_INPUT_GESTURE_DIRECTION_WEST ||
        c->edge_direction > SOLAR_OS_INPUT_GESTURE_DIRECTION_SOUTH)) return false;
    unsigned width = (c->orientation == 90 || c->orientation == 270) ? c->native_height : c->native_width;
    unsigned height = (c->orientation == 90 || c->orientation == 270) ? c->native_width : c->native_height;
    return !c->visible || (c->width && c->height &&
        (uint32_t)c->x+c->width <= width/c->scale && (uint32_t)c->y+c->height <= height/c->scale);
}

esp_err_t solar_os_input_route_claim(const void *owner, const solar_os_input_route_config_t *config)
{
    if (!owner || !input_route_config_valid(config)) return ESP_ERR_INVALID_ARG;
    solar_os_input_route_event_t *queue = solar_os_memory_calloc(INPUT_ROUTE_MAX, sizeof(*queue),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "input.os-route");
    if (!queue) return ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&input_lock);
    esp_err_t err = input_route.owner ? ESP_ERR_INVALID_STATE : ESP_OK;
    if (err == ESP_OK) {
        input_route.config = *config;
        input_route.generation++;
        input_route.queue = queue; input_route.owner = owner;
        input_route.head = input_route.count = 0; input_route.reset = false;
        queue = NULL;
    }
    portEXIT_CRITICAL(&input_lock);
    solar_os_memory_free(queue);
    return err;
}

esp_err_t solar_os_input_route_update(const void *owner, const solar_os_input_route_config_t *config)
{
    if (!input_route_config_valid(config)) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&input_lock);
    esp_err_t err = owner && input_route.owner == owner ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) input_route.config = *config;
    portEXIT_CRITICAL(&input_lock);
    return err;
}

void solar_os_input_route_release(const void *owner)
{
    solar_os_input_route_event_t *queue = NULL;
    portENTER_CRITICAL(&input_lock);
    if (owner && input_route.owner == owner) {
        queue = input_route.queue;
        input_route.queue = NULL; input_route.owner = NULL;
        input_route.head = input_route.count = 0;
        memset(&input_route.config, 0, sizeof(input_route.config));
    }
    portEXIT_CRITICAL(&input_lock);
    solar_os_memory_free(queue);
}

bool solar_os_input_route_read(const void *owner, solar_os_input_route_event_t *event, bool *reset)
{
    portENTER_CRITICAL(&input_lock);
    bool valid = owner && input_route.owner == owner;
    if (reset) *reset = valid && input_route.reset;
    if (valid) input_route.reset = false;
    bool found = valid && input_route.count;
    if (found && event) {
        *event = input_route.queue[input_route.head];
        input_route.head = (input_route.head + 1U) % INPUT_ROUTE_MAX; input_route.count--;
    }
    portEXIT_CRITICAL(&input_lock);
    return found;
}
