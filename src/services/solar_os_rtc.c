#include "solar_os_rtc.h"

#include <string.h>

#include "freertos/FreeRTOS.h"

static solar_os_rtc_provider_t rtc_provider;
static char rtc_provider_owner[SOLAR_OS_RTC_PROVIDER_NAME_MAX];
static char rtc_alarm_owner[SOLAR_OS_RTC_OWNER_NAME_MAX];
static char rtc_countdown_owner[SOLAR_OS_RTC_OWNER_NAME_MAX];
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

static esp_err_t claim_slot(char *slot, const char *owner)
{
    if (!owner_is_valid(owner)) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&rtc_provider_lock);
    esp_err_t ret = ESP_OK;
    if (slot[0] != '\0' && strcmp(slot, owner) != 0) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        strlcpy(slot, owner, SOLAR_OS_RTC_OWNER_NAME_MAX);
    }
    portEXIT_CRITICAL(&rtc_provider_lock);
    return ret;
}

static esp_err_t check_slot_owner(const char *slot, const char *owner)
{
    if (!owner_is_valid(owner)) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&rtc_provider_lock);
    const bool matches = slot[0] != '\0' && strcmp(slot, owner) == 0;
    portEXIT_CRITICAL(&rtc_provider_lock);
    return matches ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static void release_slot(char *slot, const char *owner)
{
    portENTER_CRITICAL(&rtc_provider_lock);
    if (strcmp(slot, owner) == 0) {
        slot[0] = '\0';
    }
    portEXIT_CRITICAL(&rtc_provider_lock);
}

static bool rtc_provider_snapshot(solar_os_rtc_provider_t *provider)
{
    if (provider == NULL) {
        return false;
    }
    portENTER_CRITICAL(&rtc_provider_lock);
    const bool available = rtc_provider_owner[0] != '\0';
    if (available) {
        *provider = rtc_provider;
    }
    portEXIT_CRITICAL(&rtc_provider_lock);
    return available;
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

    const solar_os_time_provider_t time_provider = {
        .get_utc_datetime = provider->get_utc_datetime,
        .set_utc_datetime = provider->set_utc_datetime,
        .user = provider->user,
    };
    esp_err_t ret = solar_os_time_register_provider(owner, &time_provider);
    if (ret != ESP_OK) {
        return ret;
    }

    portENTER_CRITICAL(&rtc_provider_lock);
    if (rtc_provider_owner[0] != '\0') {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        rtc_provider = *provider;
        strlcpy(rtc_provider_owner, owner, sizeof(rtc_provider_owner));
    }
    portEXIT_CRITICAL(&rtc_provider_lock);

    if (ret == ESP_OK && provider->disable_alarm != NULL) {
        ret = provider->disable_alarm(provider->user);
    }
    if (ret == ESP_OK && provider->disable_countdown != NULL) {
        ret = provider->disable_countdown(provider->user);
    }
    if (ret == ESP_OK && provider->clear_interrupt_status != NULL) {
        ret = provider->clear_interrupt_status(
            provider->user,
            SOLAR_OS_RTC_INTERRUPT_ALARM | SOLAR_OS_RTC_INTERRUPT_COUNTDOWN);
    }
    if (ret != ESP_OK) {
        portENTER_CRITICAL(&rtc_provider_lock);
        if (strcmp(rtc_provider_owner, owner) == 0) {
            memset(&rtc_provider, 0, sizeof(rtc_provider));
            rtc_provider_owner[0] = '\0';
            rtc_alarm_owner[0] = '\0';
            rtc_countdown_owner[0] = '\0';
        }
        portEXIT_CRITICAL(&rtc_provider_lock);
        (void)solar_os_time_unregister_provider(owner);
    }
    return ret;
}

esp_err_t solar_os_rtc_unregister_provider(const char *owner)
{
    if (owner == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&rtc_provider_lock);
    const bool owner_matches = strcmp(rtc_provider_owner, owner) == 0;
    portEXIT_CRITICAL(&rtc_provider_lock);
    if (!owner_matches) {
        return ESP_ERR_NOT_FOUND;
    }

    const esp_err_t ret = solar_os_time_unregister_provider(owner);
    if (ret != ESP_OK) {
        return ret;
    }

    portENTER_CRITICAL(&rtc_provider_lock);
    memset(&rtc_provider, 0, sizeof(rtc_provider));
    rtc_provider_owner[0] = '\0';
    rtc_alarm_owner[0] = '\0';
    rtc_countdown_owner[0] = '\0';
    portEXIT_CRITICAL(&rtc_provider_lock);
    return ESP_OK;
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
    strlcpy(info->alarm_owner, rtc_alarm_owner, sizeof(info->alarm_owner));
    strlcpy(info->countdown_owner,
            rtc_countdown_owner,
            sizeof(info->countdown_owner));
    portEXIT_CRITICAL(&rtc_provider_lock);
    return ESP_OK;
}

esp_err_t solar_os_rtc_set_alarm_for(const char *owner,
                                     const solar_os_rtc_alarm_t *alarm)
{
    if (!alarm_is_valid(alarm)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = claim_slot(rtc_alarm_owner, owner);
    if (ret != ESP_OK) {
        return ret;
    }
    solar_os_rtc_provider_t provider;
    if (!rtc_provider_snapshot(&provider) || provider.set_alarm == NULL) {
        release_slot(rtc_alarm_owner, owner);
        return ESP_ERR_NOT_SUPPORTED;
    }
    ret = provider.set_alarm(provider.user, alarm);
    if (ret != ESP_OK) {
        release_slot(rtc_alarm_owner, owner);
    }
    return ret;
}

esp_err_t solar_os_rtc_disable_alarm_for(const char *owner)
{
    esp_err_t ret = check_slot_owner(rtc_alarm_owner, owner);
    if (ret != ESP_OK) {
        return ret;
    }
    solar_os_rtc_provider_t provider;
    if (!rtc_provider_snapshot(&provider) || provider.disable_alarm == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    ret = provider.disable_alarm(provider.user);
    if (ret == ESP_OK) {
        release_slot(rtc_alarm_owner, owner);
    }
    return ret;
}

esp_err_t solar_os_rtc_set_countdown_for(const char *owner,
                                         uint32_t period_seconds,
                                         bool repeat)
{
    if (period_seconds == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = claim_slot(rtc_countdown_owner, owner);
    if (ret != ESP_OK) {
        return ret;
    }
    solar_os_rtc_provider_t provider;
    if (!rtc_provider_snapshot(&provider) || provider.set_countdown == NULL) {
        release_slot(rtc_countdown_owner, owner);
        return ESP_ERR_NOT_SUPPORTED;
    }
    ret = provider.set_countdown(provider.user, period_seconds, repeat);
    if (ret != ESP_OK) {
        release_slot(rtc_countdown_owner, owner);
    }
    return ret;
}

esp_err_t solar_os_rtc_disable_countdown_for(const char *owner)
{
    esp_err_t ret = check_slot_owner(rtc_countdown_owner, owner);
    if (ret != ESP_OK) {
        return ret;
    }
    solar_os_rtc_provider_t provider;
    if (!rtc_provider_snapshot(&provider) || provider.disable_countdown == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    ret = provider.disable_countdown(provider.user);
    if (ret == ESP_OK) {
        release_slot(rtc_countdown_owner, owner);
    }
    return ret;
}

void solar_os_rtc_release_owner(const char *owner)
{
    if (!owner_is_valid(owner)) {
        return;
    }
    solar_os_rtc_provider_t provider;
    if (!rtc_provider_snapshot(&provider)) {
        return;
    }
    portENTER_CRITICAL(&rtc_provider_lock);
    const bool owns_alarm = strcmp(rtc_alarm_owner, owner) == 0;
    const bool owns_countdown = strcmp(rtc_countdown_owner, owner) == 0;
    portEXIT_CRITICAL(&rtc_provider_lock);
    if (owns_alarm && provider.disable_alarm != NULL &&
        provider.disable_alarm(provider.user) == ESP_OK) {
        release_slot(rtc_alarm_owner, owner);
    }
    if (owns_countdown && provider.disable_countdown != NULL &&
        provider.disable_countdown(provider.user) == ESP_OK) {
        release_slot(rtc_countdown_owner, owner);
    }
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
    if (!rtc_provider_snapshot(&provider) || provider.get_interrupt_status == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return provider.get_interrupt_status(provider.user, interrupts);
}

esp_err_t solar_os_rtc_clear_interrupt_status(uint32_t interrupts)
{
    if (interrupts == 0 || (interrupts & ~RTC_INTERRUPT_ALL) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_rtc_provider_t provider;
    if (!rtc_provider_snapshot(&provider) || provider.clear_interrupt_status == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return provider.clear_interrupt_status(provider.user, interrupts);
}
