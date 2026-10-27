/* Run the same maintenance contract against each real panel transport.
 * RELAX_WIRE points at the visible current-image plane in captured SPI data.
 */
static void test_relax_transaction(void)
{
    RELAX_TYPE d = make_display();
#ifdef RELAX_SETUP
    RELAX_SETUP(d);
#endif
    frame[0] = 0xfe; frame[sizeof(frame) - 1] = 0x7f;
    assert(RELAX_REFRESH(&d) == ESP_OK);
    d.refresh_mode = RELAX_PARTIAL;
    uint8_t original[sizeof(frame)];
    memcpy(original, frame, sizeof(frame));
    assert(RELAX_APPLY(&d, SOLAR_OS_DISPLAY_RELAX_END) == ESP_ERR_INVALID_STATE);
    assert(RELAX_APPLY(&d, (solar_os_display_relax_phase_t)-1) == ESP_ERR_INVALID_ARG);
    assert(RELAX_APPLY(&d, SOLAR_OS_DISPLAY_RELAX_BEGIN) == ESP_OK);
    assert(RELAX_APPLY(&d, SOLAR_OS_DISPLAY_RELAX_BEGIN) == ESP_ERR_INVALID_STATE);
    assert(RELAX_MODE(&d, "refresh=full") == ESP_ERR_INVALID_STATE);
    RELAX_COUNT = 0;
    assert(RELAX_REFRESH(&d) == ESP_OK && !RELAX_COUNT);
    for (unsigned n = 0; n < 3; ++n) {
        RELAX_COUNT = 0;
        assert(RELAX_APPLY(&d, n ? SOLAR_OS_DISPLAY_RELAX_WHITE : SOLAR_OS_DISPLAY_RELAX_BLACK) == ESP_OK);
        const uint8_t *wire = RELAX_WIRE;
        for (size_t i = 0; i < sizeof(frame); ++i) assert(wire[i] == (n ? 0xff : 0));
        assert(memcmp(frame, original, sizeof(frame)) == 0);
        assert(d.refresh_mode == RELAX_PARTIAL);
    }
    memset(frame, 0xa5, sizeof(frame)); /* background drawing must not replace the saved image */
    RELAX_COUNT = 0;
    assert(RELAX_APPLY(&d, SOLAR_OS_DISPLAY_RELAX_END) == ESP_OK);
    assert(!d.relax.storage && d.shadow_valid);
    assert(memcmp(frame, original, sizeof(frame)) == 0 && memcmp(shadow, original, sizeof(frame)) == 0);
    assert(d.refresh_mode == RELAX_PARTIAL);
    RELAX_COUNT = 0;
    assert(RELAX_REFRESH(&d) == ESP_OK && !RELAX_COUNT);

    assert(RELAX_APPLY(&d, SOLAR_OS_DISPLAY_RELAX_BEGIN) == ESP_OK);
    fail_after = 0; RELAX_COUNT = 0;
    assert(RELAX_APPLY(&d, SOLAR_OS_DISPLAY_RELAX_BLACK) == ESP_FAIL);
    assert(!d.controller_ready && !d.shadow_valid && d.relax.storage);
    RELAX_COUNT = 0;
    assert(RELAX_APPLY(&d, SOLAR_OS_DISPLAY_RELAX_END) == ESP_OK);
    assert(!d.relax.storage && d.shadow_valid && memcmp(shadow, original, sizeof(frame)) == 0);

    assert(RELAX_APPLY(&d, SOLAR_OS_DISPLAY_RELAX_BEGIN) == ESP_OK);
    fail_after = 0; RELAX_COUNT = 0;
    assert(RELAX_APPLY(&d, SOLAR_OS_DISPLAY_RELAX_END) == ESP_FAIL);
    assert(!d.relax.storage && !d.controller_ready && !d.shadow_valid);
    assert(memcmp(frame, original, sizeof(frame)) == 0 && d.refresh_mode == RELAX_PARTIAL);
    RELAX_COUNT = 0;
    assert(RELAX_REFRESH(&d) == ESP_OK && d.shadow_valid);
    assert(RELAX_APPLY(&d, SOLAR_OS_DISPLAY_RELAX_BEGIN) == ESP_OK);
    d.powered = false; RELAX_COUNT = 0;
    assert(RELAX_APPLY(&d, SOLAR_OS_DISPLAY_RELAX_END) == ESP_ERR_INVALID_STATE);
    assert(!d.relax.storage && !RELAX_COUNT && !d.shadow_valid);
    assert(memcmp(frame, original, sizeof(frame)) == 0);
}
