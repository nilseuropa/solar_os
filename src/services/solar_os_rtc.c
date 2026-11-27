#include "solar_os_rtc.h"

#include <string.h>

#include "freertos/FreeRTOS.h"

static solar_os_rtc_provider_t rtc_provider;
static bool rtc_provider_switching;
static size_t rtc_provider_calls;
static char rtc_provider_owner[SOLAR_OS_RTC_PROVIDER_NAME_MAX];
typedef struct {
    char owner[SOLAR_OS_RTC_OWNER_NAME_MAX];
    bool busy;
} rtc_timer_t;
static rtc_timer_t rtc_alarm;
static rtc_timer_t rtc_countdown;
static portMUX_TYPE rtc_provider_lock = portMUX_INITIALIZER_UNLOCKED;

#define RTC_ALARM_MATCH_ALL (SOLAR_OS_RTC_ALARM_MATCH_SECOND | \
                             SOLAR_OS_RTC_ALARM_MATCH_MINUTE | \
                             SOLAR_OS_RTC_ALARM_MATCH_HOUR | \
                             SOLAR_OS_RTC_ALARM_MATCH_DAY | \
                             SOLAR_OS_RTC_ALARM_MATCH_WEEKDAY)
#define RTC_INTERRUPT_ALL (SOLAR_OS_RTC_INTERRUPT_ALARM | \
                           SOLAR_OS_RTC_INTERRUPT_COUNTDOWN)

static bool callback_pair_is_valid(bool first_present, bool second_present)
{
    return first_present == second_present;
}

static bool owner_is_valid(const char *owner)
{
    return owner != NULL && owner[0] != '\0' &&
        strnlen(owner, SOLAR_OS_RTC_OWNER_NAME_MAX) < SOLAR_OS_RTC_OWNER_NAME_MAX;
}

static esp_err_t rtc_timer_begin(rtc_timer_t *timer, const char *owner, bool program, bool *new_owner)
{
    portENTER_CRITICAL(&rtc_provider_lock);
    esp_err_t error = ESP_ERR_INVALID_STATE;
    const bool available = !timer->owner[0];
    const bool can_use = program && available;
    if (!timer->busy && (can_use || !strcmp(timer->owner, owner))) {
        timer->busy = true;
        if (new_owner) *new_owner = can_use;
        if (can_use) strlcpy(timer->owner, owner, sizeof(timer->owner));
        error = ESP_OK;
    }
    portEXIT_CRITICAL(&rtc_provider_lock);
    return error;
}

static void rtc_timer_end(rtc_timer_t *timer, bool release)
{
    portENTER_CRITICAL(&rtc_provider_lock);
    if (release) timer->owner[0] = '\0';
    timer->busy = false;
    portEXIT_CRITICAL(&rtc_provider_lock);
}

static bool rtc_timer_owned(rtc_timer_t *timer, const char *owner)
{
    portENTER_CRITICAL(&rtc_provider_lock);
    const bool owned = !strcmp(timer->owner, owner);
    portEXIT_CRITICAL(&rtc_provider_lock);
    return owned;
}

static bool rtc_provider_acquire(solar_os_rtc_provider_t *provider)
{
    portENTER_CRITICAL(&rtc_provider_lock);
    const bool available = rtc_provider_owner[0] != '\0' && !rtc_provider_switching;
    if (available) {
        *provider = rtc_provider;
        rtc_provider_calls++;
    }
    portEXIT_CRITICAL(&rtc_provider_lock);
    return available;
}

static void rtc_provider_release(void)
{
    portENTER_CRITICAL(&rtc_provider_lock);
    rtc_provider_calls--;
    portEXIT_CRITICAL(&rtc_provider_lock);
}

static bool alarm_is_valid(const solar_os_rtc_alarm_t *alarm)
{
    if (alarm == NULL || alarm->match_fields == 0 ||
        (alarm->match_fields & ~RTC_ALARM_MATCH_ALL) != 0) {
        return false;
    }
    return ((alarm->match_fields & SOLAR_OS_RTC_ALARM_MATCH_SECOND) == 0 ||
            alarm->second <= 59) &&
        ((alarm->match_fields & SOLAR_OS_RTC_ALARM_MATCH_MINUTE) == 0 ||
         alarm->minute <= 59) &&
        ((alarm->match_fields & SOLAR_OS_RTC_ALARM_MATCH_HOUR) == 0 ||
         alarm->hour <= 23) &&
        ((alarm->match_fields & SOLAR_OS_RTC_ALARM_MATCH_DAY) == 0 ||
         (alarm->day >= 1 && alarm->day <= 31)) &&
        ((alarm->match_fields & SOLAR_OS_RTC_ALARM_MATCH_WEEKDAY) == 0 ||
         alarm->weekday <= 6);
}

esp_err_t solar_os_rtc_register_provider(
    const char *owner,
    const solar_os_rtc_provider_t *provider)
{
    if (owner == NULL || owner[0] == '\0' ||
        strnlen(owner, sizeof(rtc_provider_owner)) >= sizeof(rtc_provider_owner) ||
        provider == NULL || provider->get_utc_datetime == NULL ||
        provider->set_utc_datetime == NULL ||
        !callback_pair_is_valid(provider->set_alarm != NULL,
                                provider->disable_alarm != NULL) ||
        !callback_pair_is_valid(provider->set_countdown != NULL,
                                provider->disable_countdown != NULL) ||
        !callback_pair_is_valid(provider->get_interrupt_status != NULL,
                                provider->clear_interrupt_status != NULL) ||
        provider->interrupt_gpio < SOLAR_OS_RTC_INTERRUPT_GPIO_NONE ||
        (provider->interrupt_gpio != SOLAR_OS_RTC_INTERRUPT_GPIO_NONE &&
         provider->interrupt_active_level != 0 &&
         provider->interrupt_active_level != 1)) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&rtc_provider_lock);
    if (rtc_provider_owner[0] || rtc_provider_switching) {
        portEXIT_CRITICAL(&rtc_provider_lock);
        return ESP_ERR_INVALID_STATE;
    }
    rtc_provider_switching = true;
    portEXIT_CRITICAL(&rtc_provider_lock);

    /* Initialize hardware before publishing either the RTC or its clock
     * callbacks. A failed attach must never leave a callable provider. */
    esp_err_t error = provider->disable_alarm ? provider->disable_alarm(provider->user) : ESP_OK;
    if (error == ESP_OK && provider->disable_countdown)
        error = provider->disable_countdown(provider->user);
    if (error == ESP_OK && provider->clear_interrupt_status)
        error = provider->clear_interrupt_status(provider->user, RTC_INTERRUPT_ALL);
    if (error == ESP_OK) {
        const solar_os_time_provider_t clock = {
            .get_utc_datetime = provider->get_utc_datetime,
            .set_utc_datetime = provider->set_utc_datetime,
            .user = provider->user,
        };
        error = solar_os_time_register_provider(owner, &clock);
    }

    portENTER_CRITICAL(&rtc_provider_lock);
    if (error == ESP_OK) {
        rtc_provider = *provider;
        strlcpy(rtc_provider_owner, owner, sizeof(rtc_provider_owner));
    }
    rtc_provider_switching = false;
    portEXIT_CRITICAL(&rtc_provider_lock);
    return error;
}

static esp_err_t rtc_cancel_timer(rtc_timer_t *timer, const char *owner,
                                 const solar_os_rtc_provider_t *provider);

static esp_err_t rtc_unregister_provider(const char *owner, const void *user, bool require_user)
{
    if (!owner || !*owner) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&rtc_provider_lock);
    if (strcmp(rtc_provider_owner, owner) || (require_user && rtc_provider.user != user)) {
        portEXIT_CRITICAL(&rtc_provider_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (rtc_provider_switching || rtc_provider_calls) {
        portEXIT_CRITICAL(&rtc_provider_lock);
        return ESP_ERR_INVALID_STATE;
    }
    rtc_provider_switching = true;
    const solar_os_rtc_provider_t provider = rtc_provider;
    portEXIT_CRITICAL(&rtc_provider_lock);

    /* No RTC operation can take a new reference while switching. Keep any
     * state we could not retire, including the clock lease, for retry. */
    esp_err_t error = ESP_OK;
    if (rtc_alarm.owner[0]) error = rtc_cancel_timer(&rtc_alarm, rtc_alarm.owner, &provider);
    if (rtc_countdown.owner[0]) {
        const esp_err_t next = rtc_cancel_timer(&rtc_countdown, rtc_countdown.owner, &provider);
        if (error == ESP_OK) error = next;
    }
    if (error == ESP_OK && provider.clear_interrupt_status)
        error = provider.clear_interrupt_status(provider.user, RTC_INTERRUPT_ALL);
    if (error == ESP_OK) {
        error = solar_os_time_unregister_provider_owned(owner, provider.user);
        if (error == ESP_ERR_NOT_FOUND) error = ESP_OK;
    }

    portENTER_CRITICAL(&rtc_provider_lock);
    if (error == ESP_OK) {
        memset(&rtc_provider, 0, sizeof(rtc_provider));
        rtc_provider_owner[0] = '\0';
        rtc_alarm = (rtc_timer_t){0};
        rtc_countdown = (rtc_timer_t){0};
    }
    rtc_provider_switching = false;
    portEXIT_CRITICAL(&rtc_provider_lock);
    return error;
}

esp_err_t solar_os_rtc_unregister_provider(const char *owner)
{
    return rtc_unregister_provider(owner, NULL, false);
}

esp_err_t solar_os_rtc_unregister_provider_owned(const char *owner, const void *user)
{
    return rtc_unregister_provider(owner, user, true);
}

bool solar_os_rtc_has_provider(void)
{
    portENTER_CRITICAL(&rtc_provider_lock);
    const bool available = rtc_provider_owner[0] != '\0';
    portEXIT_CRITICAL(&rtc_provider_lock);
    return available;
}

esp_err_t solar_os_rtc_get_info(solar_os_rtc_info_t *info)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&rtc_provider_lock);
    if (rtc_provider_owner[0] == '\0') {
        portEXIT_CRITICAL(&rtc_provider_lock);
        return ESP_ERR_NOT_SUPPORTED;
    }
    strlcpy(info->provider, rtc_provider_owner, sizeof(info->provider));
    info->capabilities = SOLAR_OS_RTC_CAP_CALENDAR;
    if (rtc_provider.set_alarm != NULL) {
        info->capabilities |= SOLAR_OS_RTC_CAP_ALARM;
    }
    if (rtc_provider.set_countdown != NULL) {
        info->capabilities |= SOLAR_OS_RTC_CAP_COUNTDOWN;
    }
    if (rtc_provider.get_interrupt_status != NULL) {
        info->capabilities |= SOLAR_OS_RTC_CAP_INTERRUPT_STATUS;
    }
    info->interrupt_gpio = rtc_provider.interrupt_gpio;
    info->interrupt_active_level = rtc_provider.interrupt_active_level;
    strlcpy(info->alarm_owner, rtc_alarm.owner, sizeof(info->alarm_owner));
    strlcpy(info->countdown_owner,
            rtc_countdown.owner,
            sizeof(info->countdown_owner));
    portEXIT_CRITICAL(&rtc_provider_lock);
    return ESP_OK;
}

static rtc_timer_t *rtc_timer_for_kind(solar_os_rtc_timer_kind_t kind)
{
    switch (kind) {
    case SOLAR_OS_RTC_TIMER_ALARM: return &rtc_alarm;
    case SOLAR_OS_RTC_TIMER_COUNTDOWN: return &rtc_countdown;
    default: return NULL;
    }
}

esp_err_t solar_os_rtc_program_timer(const char *owner, const solar_os_rtc_timer_request_v1_t *request)
{
    if (!owner_is_valid(owner) || !request) return ESP_ERR_INVALID_ARG;
    rtc_timer_t *timer = rtc_timer_for_kind(request->kind);
    const bool alarm = request->kind == SOLAR_OS_RTC_TIMER_ALARM;
    if (!timer || (alarm && !alarm_is_valid(&request->configuration.alarm)) ||
        (!alarm && !request->configuration.countdown.seconds)) return ESP_ERR_INVALID_ARG;
    solar_os_rtc_provider_t provider;
    if (!rtc_provider_acquire(&provider)) return ESP_ERR_NOT_SUPPORTED;
    const bool supported = alarm ? provider.set_alarm != NULL : provider.set_countdown != NULL;
    bool new_owner = false;
    esp_err_t error = supported ? rtc_timer_begin(timer, owner, true, &new_owner) : ESP_ERR_NOT_SUPPORTED;
    if (error == ESP_OK) {
        error = alarm ? provider.set_alarm(provider.user, &request->configuration.alarm) :
            provider.set_countdown(provider.user, request->configuration.countdown.seconds,
                                   request->configuration.countdown.repeat);
        rtc_timer_end(timer, error != ESP_OK && new_owner);
    }
    rtc_provider_release();
    return error;
}

static esp_err_t rtc_cancel_timer(rtc_timer_t *timer, const char *owner,
                                 const solar_os_rtc_provider_t *provider)
{
    esp_err_t error = rtc_timer_begin(timer, owner, false, NULL);
    if (error != ESP_OK) return error;
    const solar_os_rtc_disable_alarm_fn_t disable = timer == &rtc_alarm ?
        provider->disable_alarm : provider->disable_countdown;
    error = disable ? disable(provider->user) : ESP_ERR_NOT_SUPPORTED;
    rtc_timer_end(timer, error == ESP_OK);
    return error;
}

esp_err_t solar_os_rtc_cancel_timer(const char *owner, solar_os_rtc_timer_kind_t kind)
{
    rtc_timer_t *timer = rtc_timer_for_kind(kind);
    if (!owner_is_valid(owner) || !timer) return ESP_ERR_INVALID_ARG;
    solar_os_rtc_provider_t provider;
    if (!rtc_provider_acquire(&provider)) return ESP_ERR_INVALID_STATE;
    const esp_err_t error = rtc_cancel_timer(timer, owner, &provider);
    rtc_provider_release();
    return error;
}

esp_err_t solar_os_rtc_release_owner_checked(const char *owner)
{
    if (!owner_is_valid(owner)) return ESP_ERR_INVALID_ARG;
    solar_os_rtc_provider_t provider;
    if (!rtc_provider_acquire(&provider))
        return solar_os_rtc_has_provider() ? ESP_ERR_INVALID_STATE : ESP_OK;
    esp_err_t error = ESP_OK;
    if (rtc_timer_owned(&rtc_alarm, owner)) error = rtc_cancel_timer(&rtc_alarm, owner, &provider);
    if (rtc_timer_owned(&rtc_countdown, owner)) {
        const esp_err_t next = rtc_cancel_timer(&rtc_countdown, owner, &provider);
        if (error == ESP_OK) error = next;
    }
    rtc_provider_release();
    return error;
}

void solar_os_rtc_release_owner(const char *owner)
{
    (void)solar_os_rtc_release_owner_checked(owner);
}

esp_err_t solar_os_rtc_set_alarm_for(const char *owner, const solar_os_rtc_alarm_t *alarm)
{
    if (!alarm) return ESP_ERR_INVALID_ARG;
    const solar_os_rtc_timer_request_v1_t request = {
        .kind = SOLAR_OS_RTC_TIMER_ALARM, .configuration.alarm = *alarm,
    };
    return solar_os_rtc_program_timer(owner, &request);
}

esp_err_t solar_os_rtc_disable_alarm_for(const char *owner)
{
    return solar_os_rtc_cancel_timer(owner, SOLAR_OS_RTC_TIMER_ALARM);
}

esp_err_t solar_os_rtc_set_countdown_for(const char *owner, uint32_t seconds, bool repeat)
{
    const solar_os_rtc_timer_request_v1_t request = {
        .kind = SOLAR_OS_RTC_TIMER_COUNTDOWN, .configuration.countdown = {seconds, repeat},
    };
    return solar_os_rtc_program_timer(owner, &request);
}

esp_err_t solar_os_rtc_disable_countdown_for(const char *owner)
{
    return solar_os_rtc_cancel_timer(owner, SOLAR_OS_RTC_TIMER_COUNTDOWN);
}

esp_err_t solar_os_rtc_set_alarm(const solar_os_rtc_alarm_t *alarm)
{
    return solar_os_rtc_set_alarm_for("direct", alarm);
}

esp_err_t solar_os_rtc_disable_alarm(void)
{
    return solar_os_rtc_disable_alarm_for("direct");
}

esp_err_t solar_os_rtc_set_countdown(uint32_t period_seconds, bool repeat)
{
    return solar_os_rtc_set_countdown_for("direct", period_seconds, repeat);
}

esp_err_t solar_os_rtc_disable_countdown(void)
{
    return solar_os_rtc_disable_countdown_for("direct");
}

esp_err_t solar_os_rtc_get_interrupt_status(uint32_t *interrupts)
{
    if (interrupts == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_rtc_provider_t provider;
    if (!rtc_provider_acquire(&provider)) return ESP_ERR_NOT_SUPPORTED;
    const esp_err_t error = provider.get_interrupt_status ?
        provider.get_interrupt_status(provider.user, interrupts) : ESP_ERR_NOT_SUPPORTED;
    rtc_provider_release();
    return error;
}

esp_err_t solar_os_rtc_clear_interrupt_status(uint32_t interrupts)
{
    if (interrupts == 0 || (interrupts & ~RTC_INTERRUPT_ALL) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_rtc_provider_t provider;
    if (!rtc_provider_acquire(&provider)) return ESP_ERR_NOT_SUPPORTED;
    const esp_err_t error = provider.clear_interrupt_status ?
        provider.clear_interrupt_status(provider.user, interrupts) : ESP_ERR_NOT_SUPPORTED;
    rtc_provider_release();
    return error;
}
