CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror

ROOT := ../..
override CFLAGS += -idirafter $(ROOT)/include
WRITER_DIR := $(ROOT)/src/apps
TARGETS := writer_buffer_test writer_files_test file_shortcuts_test sketch_canvas_test launcher_layout_test display_layout_test gfx_icons_test text_search_test doc_layout_test shell_parse_test shell_launch_test shell_line_test shell_completion_test shell_diag_test shell_startup_policy_test scheduler_test schedule_test script_lifecycle_test app_file_types_test gameboy_support_test storage_mounts_test json_scan_test native_elf_test keys_test input_test unistroke_test graffiti_test board_caps_test hid_keyboard_report_test ble_keyboard_scan_policy_test ps2_keyboard_test ps2_mouse_test cardkb_codec_test inputronic_keyboard_test matrix_keyboard_test input_keymap_service_test cl32_keyboard_test battery_service_test battery_trend_test charger_service_test sensors_service_test midi_codec_test midi_service_test parameters_test controls_test osc_test dsp_test stream_test resources_test gpio_controller_test expansion_registry_test expansion_command_test audio_codec_test webradio_catalog_test gif_decode_stack_test nvs_backup_test agent_reference_test manual_external_metadata_test terminal_geometry_test app_exit_result_test terminal_preferences_test tui_widgets_test wireguard_crypto_test meshcore_channel_key_test meshcore_stream_codec_test meshcore_ble_protocol_test link_repeater_test time_timezone_test rtc_service_test rtc_pcf85063_test ubx_test gnss_service_test modem_service_test haptic_service_test imu_service_test bhi260ap_codec_test qmi8658_test drv2605_test bq25896_test axp2101_test max17048_test mgc3130_test sim7670_test xl9555_test st25r3916_test nfc_service_test

MANUAL_GENERATED_DIR := generated/solar_os
MANUAL_GENERATED_HEADER := $(MANUAL_GENERATED_DIR)/solar_os_manual_data.h
KEYMAP_GENERATED_HEADERS := $(addprefix $(MANUAL_GENERATED_DIR)/,solar_os_tca8418_keymap_generated.h solar_os_inputronic_keymap_generated.h solar_os_pager_keymap_generated.h)
KEYMAP_INPUTS := $(wildcard $(ROOT)/keymaps/profiles/*.json) $(ROOT)/scripts/generate_keymaps.py $(ROOT)/scripts/solaros_keymap.py $(ROOT)/scripts/solaros_keymap_build.py

.PHONY: all test clean

TARGETS += ble_service_test ble_multi_peer_test ble_nimble_test ble_hid_test ble_lua_bindings_test ble_server_test
TARGETS += input_action_match_test gesture_completion_test
TARGETS += epd_ssd1677_test epd_uc8179_test uc8179_expansion_test epd_uc8279_test uc8279_expansion_test
TARGETS += frontlight_pwm_test epd_xteink_identify_test x4pro_expansion_test
TARGETS += gt911_test
TARGETS += sdmmc_expansion_test
TARGETS += axp2101_service_test
TARGETS += camera_service_test camera_expansion_test media_test mjpeg_test
TARGETS += http_server_test
TARGETS += rtspd_job_test rtspd_audio_only_test
TARGETS += rtsp_receiver_test
TARGETS += rtsp_client_test
TARGETS += rtsp_auth_test
TARGETS += gfx_mono_raster_test
TARGETS += rgb565_raster_test
TARGETS += rtspd_completion_test
TARGETS += script_media_test script_media_rtsp_test
TARGETS += raster_image_test
TARGETS += vision_test
TARGETS += inference_test inference_lua_bindings_test inference_schema_test
TARGETS += espdl_workers_test espdl_workers_single_test
TARGETS += tensor_image_test
TARGETS += tensor_image_lua_test
TARGETS += vision_lua_bindings_test
TARGETS += media_lua_bindings_test
TARGETS += messaging_service_test
TARGETS += nmea_test nmea_gnss_test
TARGETS += cw2017_test
TARGETS += rtc_pcf8563_test
TARGETS += tab5_keyboard_test
TARGETS += i2c_buses_test

tab5_keyboard_test: tab5_keyboard_test.c $(wildcard $(ROOT)/src/services/solar_os_tab5_keyboard*) \
        $(ROOT)/src/drivers/tab5_keyboard.c $(ROOT)/src/drivers/tab5_keyboard.h \
        $(wildcard $(ROOT)/src/services/solar_os_input_keymap*) \
        $(wildcard inputronic_stubs/*.h inputronic_stubs/freertos/*.h inputronic_stubs/driver/*.h)
	$(CC) $(CFLAGS) -include compat.h -Iinputronic_stubs -Iexpansion_stubs -I. \
		-I$(ROOT)/src -I$(ROOT)/src/services -I$(ROOT)/src/drivers -I$(CJSON_DIR) \
		tab5_keyboard_test.c $(ROOT)/src/drivers/tab5_keyboard.c \
		$(ROOT)/src/services/solar_os_tab5_keyboard_profile.c \
		$(ROOT)/src/services/solar_os_input_keymap.c $(ROOT)/src/services/solar_os_input_keymap_mapper.c \
		$(ROOT)/src/services/solar_os_input_keymap_json.c $(CJSON_DIR)/cJSON.c -lm -o $@

rtc_pcf8563_test: rtc_pcf8563_test.c $(ROOT)/src/drivers/rtc_pcf8563.c \
        $(ROOT)/src/drivers/rtc_pcf8563.h $(ROOT)/src/services/solar_os_pcf8563.c \
        $(ROOT)/src/services/solar_os_pcf8563.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h -Iexpansion_stubs -I. \
		-I$(ROOT)/include -I$(ROOT)/src/services -I$(ROOT)/src/drivers \
		rtc_pcf8563_test.c $(ROOT)/src/drivers/rtc_pcf8563.c $(ROOT)/src/services/solar_os_pcf8563.c -o $@

i2c_buses_test: i2c_buses_test.c $(ROOT)/src/drivers/i2c_bus.c \
        $(ROOT)/src/drivers/i2c_bus.h $(ROOT)/src/services/solar_os_buses.c \
        $(ROOT)/src/services/solar_os_buses.h $(ROOT)/src/services/solar_os_resources.c \
        $(ROOT)/include/solar_os_bus_types.h $(wildcard i2c_stubs/*.h) \
        $(wildcard i2c_stubs/driver/*.h) $(wildcard i2c_stubs/freertos/*.h)
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include i2c_stubs/compat.h \
		-Wno-unused-parameter -ffunction-sections -fdata-sections \
		-Ii2c_stubs -Iexpansion_stubs -I. \
		-I$(ROOT)/include -I$(ROOT)/src/services -I$(ROOT)/src/drivers \
		i2c_buses_test.c $(ROOT)/src/drivers/i2c_bus.c \
		$(ROOT)/src/services/solar_os_buses.c $(ROOT)/src/services/solar_os_resources.c \
		-Wl,--gc-sections -o $@

cw2017_test: cw2017_test.c $(ROOT)/src/drivers/cw2017.c $(ROOT)/src/drivers/cw2017.h \
        $(ROOT)/src/services/solar_os_cw2017.c $(ROOT)/src/services/solar_os_cw2017.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h -Iexpansion_stubs -I. \
		-I$(ROOT)/include -I$(ROOT)/src/services -I$(ROOT)/src/drivers \
		cw2017_test.c $(ROOT)/src/drivers/cw2017.c $(ROOT)/src/services/solar_os_cw2017.c -o $@
TARGETS += pipeline_test pipeline_lua_test
TARGETS += jobs_admission_test

jobs_admission_test: jobs_admission_test.c $(ROOT)/src/solar_os_jobs.c $(ROOT)/src/solar_os_scheduler.c
	$(CC) $(CFLAGS) -DSOLAR_OS_JOBS_MAX=0 -include compat.h -ffunction-sections -fdata-sections \
		-Ijobs_admission_stubs -I. -I$(ROOT)/src -I$(ROOT)/src/services \
		$(filter-out $(ROOT)/src/solar_os_jobs.c,$^) -Wl,--gc-sections -o $@
LUA_DIR := $(ROOT)/components/lua/lua/src
LUA_TEST_SOURCES := $(filter-out $(LUA_DIR)/lua.c $(LUA_DIR)/luac.c \
	$(LUA_DIR)/linit.c $(LUA_DIR)/liolib.c $(LUA_DIR)/loadlib.c $(LUA_DIR)/loslib.c, \
	$(wildcard $(LUA_DIR)/*.c))

pipeline_test: pipeline_test.c $(ROOT)/src/services/solar_os_pipeline.c $(ROOT)/src/services/solar_os_processor.c
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -pthread -Ipipeline_stubs -Irtsp_stubs \
		-I. -I$(ROOT)/src -I$(ROOT)/src/services -I$(CJSON_DIR) $^ $(CJSON_DIR)/cJSON.c -lm -o $@

pipeline_lua_test: pipeline_lua_test.c pipeline_test.c $(ROOT)/src/apps/solar_os_lua_pipeline.inc \
        $(ROOT)/src/services/solar_os_pipeline.c $(ROOT)/src/services/solar_os_processor.c $(LUA_TEST_SOURCES)
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -pthread -Ipipeline_stubs -Irtsp_stubs \
		-I. -I$(ROOT)/src -I$(ROOT)/src/services -I$(ROOT)/src/apps -I$(LUA_DIR) -I$(CJSON_DIR) \
		$(filter-out pipeline_test.c %.inc,$^) $(CJSON_DIR)/cJSON.c -lm -o $@

axp2101_service_test: axp2101_service_test.c $(ROOT)/src/services/solar_os_axp2101.c \
	$(ROOT)/src/services/solar_os_axp2101.h $(ROOT)/src/drivers/axp2101.c
	$(CC) $(CFLAGS) -include compat.h -Iaxp2101_stubs -Iexpansion_stubs -I. \
		-I$(ROOT)/include -I$(ROOT)/src/services -I$(ROOT)/src/drivers \
		axp2101_service_test.c $(ROOT)/src/services/solar_os_axp2101.c \
		$(ROOT)/src/drivers/axp2101.c -o $@

uc8279_expansion_test: uc8279_expansion_test.c $(ROOT)/src/services/solar_os_uc8279.c \
        $(ROOT)/src/services/solar_os_uc8279_driver.c $(ROOT)/src/drivers/epd_ultrachip.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h \
		-Iepd_stubs -Iexpansion_stubs -I. -I$(ROOT)/include -I$(ROOT)/src \
		-I$(ROOT)/src/services -I$(ROOT)/src/drivers -I$(ROOT)/src/board \
		-I$(ROOT)/components/u8g2/src/clib uc8279_expansion_test.c -o $@

sdmmc_expansion_test: sdmmc_expansion_test.c $(ROOT)/src/services/solar_os_sdmmc.c $(ROOT)/src/services/solar_os_sdmmc_driver.c
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h \
		-Iexpansion_stubs -I. -I$(ROOT)/include -I$(ROOT)/src/services -I$(ROOT)/src/drivers sdmmc_expansion_test.c -o $@

gt911_test: gt911_test.c $(ROOT)/src/drivers/gt911.c $(ROOT)/src/drivers/gt911.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h -Iepd_stubs -I. gt911_test.c -o $@

frontlight_pwm_test: frontlight_pwm_test.c $(ROOT)/src/drivers/frontlight_pwm.c $(ROOT)/src/drivers/frontlight_pwm.h
	$(CC) $(CFLAGS) -Ix4pro_stubs -Iepd_stubs -I. frontlight_pwm_test.c -o $@

epd_xteink_identify_test: epd_xteink_identify_test.c $(ROOT)/src/drivers/epd_xteink_identify.c $(ROOT)/src/drivers/epd_xteink_identify.h
	$(CC) $(CFLAGS) -Iepd_stubs -I. epd_xteink_identify_test.c -o $@

x4pro_expansion_test: x4pro_expansion_test.c $(ROOT)/src/services/solar_os_x4pro.c $(ROOT)/src/services/solar_os_x4pro_driver.c
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h \
		-Iepd_stubs -Iexpansion_stubs -I. -I$(ROOT)/include -I$(ROOT)/src \
		-I$(ROOT)/src/services -I$(ROOT)/src/drivers -I$(ROOT)/src/board \
		-I$(ROOT)/components/u8g2/src/clib x4pro_expansion_test.c -o $@

EPD_U8G2_TEST_SOURCES := $(addprefix $(ROOT)/components/u8g2/src/clib/, \
        u8g2_setup.c u8g2_buffer.c u8g2_font.c u8g2_ll_hvline.c u8g2_hvline.c \
        u8g2_intersection.c u8x8_setup.c u8x8_display.c)

epd_uc8279_test: epd_uc8279_test.c $(ROOT)/src/drivers/epd_ultrachip.c $(ROOT)/src/drivers/epd_ultrachip.h \
        $(ROOT)/src/drivers/epd_uc8279.c $(ROOT)/src/drivers/epd_uc8279.h \
        $(EPD_U8G2_TEST_SOURCES) \
        $(wildcard epd_stubs/*.h epd_stubs/driver/*.h epd_stubs/freertos/*.h)
	$(CC) $(CFLAGS) -Wno-unused-parameter -ffunction-sections -fdata-sections \
		-Iepd_stubs -I. -I$(ROOT)/components/u8g2/src/clib \
		epd_uc8279_test.c $(EPD_U8G2_TEST_SOURCES) -Wl,--gc-sections -o $@

uc8179_expansion_test: uc8179_expansion_test.c $(ROOT)/src/services/solar_os_uc8179.c \
        $(ROOT)/src/services/solar_os_uc8179_driver.c $(ROOT)/src/drivers/epd_ultrachip.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h \
		-Iepd_stubs -Iexpansion_stubs -I. -I$(ROOT)/include -I$(ROOT)/src \
		-I$(ROOT)/src/services -I$(ROOT)/src/drivers -I$(ROOT)/src/board \
		-I$(ROOT)/components/u8g2/src/clib uc8179_expansion_test.c -o $@

epd_uc8179_test: epd_uc8179_test.c $(ROOT)/src/drivers/epd_ultrachip.c $(ROOT)/src/drivers/epd_ultrachip.h \
        $(ROOT)/src/drivers/epd_uc8179.c $(ROOT)/src/drivers/epd_uc8179.h \
        $(wildcard epd_stubs/*.h epd_stubs/driver/*.h epd_stubs/freertos/*.h)
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections \
		-Iepd_stubs -I. -I$(ROOT)/components/u8g2/src/clib \
		epd_uc8179_test.c -Wl,--gc-sections -o $@

epd_ssd1677_test: epd_ssd1677_test.c $(ROOT)/src/drivers/epd_ssd1677.c $(ROOT)/src/drivers/epd_ssd1677.h \
	$(wildcard epd_stubs/*.h epd_stubs/driver/*.h epd_stubs/freertos/*.h)
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections \
		-Iepd_stubs -I. -I$(ROOT)/components/u8g2/src/clib \
		epd_ssd1677_test.c -Wl,--gc-sections -o $@

messaging_service_test: messaging_service_test.c $(ROOT)/src/services/solar_os_contacts.c \
        $(ROOT)/src/services/solar_os_messaging.c $(ROOT)/src/services/solar_os_messaging_types.c \
        $(ROOT)/src/services/solar_os_contacts.h $(ROOT)/src/services/solar_os_messaging.h \
        $(ROOT)/src/services/solar_os_messaging_types.h messaging_stubs/freertos/semphr.h \
        $(ROOT)/src/services/solar_os_storage.c $(ROOT)/src/services/solar_os_storage.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -DpdTICKS_TO_MS\(ticks\)=\(ticks\) -include compat.h \
		-ffunction-sections -fdata-sections \
		-Imessaging_stubs -I. -I$(ROOT)/src -I$(ROOT)/src/services \
		-I$(ROOT)/src/board -I$(ROOT)/src/drivers \
		messaging_service_test.c $(ROOT)/src/services/solar_os_messaging_types.c \
		$(ROOT)/src/services/solar_os_storage.c -Wl,--gc-sections \
		-Wl,--wrap=solar_os_storage_is_mounted,--wrap=solar_os_storage_sd_is_mounted \
		-Wl,--wrap=solar_os_storage_default_path,--wrap=solar_os_storage_mkdir \
		-Wl,--wrap=solar_os_storage_remove,--wrap=solar_os_storage_get_usage,--wrap=rename -o $@

raster_image_test: raster_image_test.c $(ROOT)/src/services/solar_os_raster_image.c
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I. -I$(ROOT)/src -I$(ROOT)/src/services \
		-I$(ROOT)/components/stb_image/include -I$(ROOT)/components/webp_decoder/include $^ -o $@

tensor_image_test: tensor_image_test.c $(ROOT)/src/services/solar_os_tensor_image.c
	$(CC) $(CFLAGS) -Iinference_stubs -Irtsp_stubs -I. -I$(ROOT)/src -I$(ROOT)/src/services $^ -lm -o $@

# Uses the same cJSON source as ESP-IDF and OpenSSL only as a host SHA-256 adapter.
# Override CJSON_DIR when the SDK is installed elsewhere.
CJSON_DIR ?= $(firstword $(wildcard $(HOME)/.platformio/packages/framework-espidf*/components/json/cJSON))
IDF_HOST_DIR := $(patsubst %/components/json/cJSON,%,$(CJSON_DIR))
zoo_test: zoo_test.c $(ROOT)/src/services/solar_os_zoo.c $(ROOT)/src/services/solar_os_zip.c
	$(CC) $(CFLAGS) -D_GNU_SOURCE -DSOLAR_OS_PACKAGE_SERVICE_IMAGE=1 -DSOLAR_OS_VERSION=\"99.0.0\" \
		-include compat.h -ffunction-sections -fdata-sections -Imodel_bundle_stubs -I. \
		-I$(ROOT)/src -I$(ROOT)/src/services -I$(CJSON_DIR) \
		-isystem $(IDF_HOST_DIR)/components/esp_rom/include -isystem $(IDF_HOST_DIR)/components/esp_rom/esp32s3 \
		$^ $(CJSON_DIR)/cJSON.c -Wl,--gc-sections,--wrap=rename,--wrap=fwrite -lcrypto -lz -lm -o $@
model_bundle_test: model_bundle_test.c inference_test.c $(ROOT)/src/services/solar_os_inference.c \
        $(ROOT)/src/services/solar_os_model_bundle.c $(ROOT)/src/services/solar_os_tensor_image.c
	$(CC) $(CFLAGS) -D_GNU_SOURCE -pthread -Imodel_bundle_stubs -Iinference_stubs -Irtsp_stubs \
		-I. -I$(ROOT)/src -I$(ROOT)/src/services -I$(CJSON_DIR) \
		$(filter-out inference_test.c,$^) $(CJSON_DIR)/cJSON.c -lcrypto -lm -o $@

QUIRC_DIR := $(ROOT)/components/quirc/lib
QUIRC_HOST_OBJECTS := $(addprefix generated/vision/,$(notdir $(patsubst %.c,%.o,$(wildcard $(QUIRC_DIR)/*.c))))
IMLIB_DIR := $(ROOT)/components/solaros_imlib
IMLIB_SOURCES := $(wildcard $(IMLIB_DIR)/upstream/*.c)
IMLIB_FLAGS := -DCMSIS_MCU_H=\"cmsis_compiler.h\" -I$(IMLIB_DIR)/include -I$(IMLIB_DIR)/upstream \
    -ffunction-sections -fdata-sections -fno-strict-aliasing -Wno-error -Wno-sign-compare -Wno-unused-parameter
TARGETS += native_image_api_test native_inference_api_test native_video_api_test native_video_rtsp_api_test
native_image_api_test: native_image_api_test.c imlib_test.c vision_test.c \
        $(ROOT)/src/services/solar_os_native_media.c $(ROOT)/src/services/solar_os_imlib.c \
        $(ROOT)/src/services/solar_os_raster_image.c $(ROOT)/src/services/solar_os_vision.c $(IMLIB_SOURCES) $(QUIRC_HOST_OBJECTS)
	$(CC) $(CFLAGS) -D_GNU_SOURCE -pthread $(IMLIB_FLAGS) -DSOLAR_OS_PACKAGE_SERVICE_IMAGE=1 \
		-DSOLAR_OS_PACKAGE_SERVICE_IMLIB=1 -DSOLAR_OS_PACKAGE_SERVICE_VISION=1 \
		-I$(ROOT)/src -Irtsp_stubs -I. -I$(ROOT)/src/services \
		-I$(ROOT)/components/stb_image/include -I$(ROOT)/components/webp_decoder/include -I$(QUIRC_DIR) \
		$(filter-out imlib_test.c vision_test.c,$^) -Wl,--gc-sections -lm -o $@

native_inference_api_test: native_inference_api_test.c inference_test.c \
        $(ROOT)/src/services/solar_os_native_media.c $(ROOT)/src/services/solar_os_inference.c \
        $(ROOT)/src/services/solar_os_tensor_image.c
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -pthread -DSOLAR_OS_PACKAGE_SERVICE_INFERENCE=1 \
		-I$(ROOT)/src -Irtsp_stubs -I. -I$(ROOT)/src/services $(filter-out inference_test.c,$^) -lm -o $@

native_video_api_test native_video_rtsp_api_test: native_video_api_test.c script_media_test.c \
        $(ROOT)/src/services/solar_os_native_media.c $(ROOT)/src/services/solar_os_script_media.c \
        $(ROOT)/src/services/solar_os_stream.c $(ROOT)/src/services/solar_os_camera.c $(ROOT)/src/services/solar_os_camera_stream.c
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h -pthread -DSOLAR_OS_PACKAGE_SERVICE_SCRIPT_MEDIA=1 \
		$(if $(filter native_video_rtsp_api_test,$@),-DSOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT=1,) \
		-Iscript_media_stubs -Irtsp_stubs -I. -I$(ROOT)/src -I$(ROOT)/src/services \
		$(filter-out script_media_test.c,$^) -o $@

TARGETS += imlib_test
TARGETS += imlib_lua_test
imlib_test: imlib_test.c vision_test.c $(ROOT)/src/services/solar_os_imlib.c \
        $(ROOT)/src/services/solar_os_raster_image.c $(ROOT)/src/services/solar_os_vision.c $(IMLIB_SOURCES) $(QUIRC_HOST_OBJECTS)
	$(CC) $(CFLAGS) -D_GNU_SOURCE -pthread $(IMLIB_FLAGS) -I$(ROOT)/src -Irtsp_stubs -I. -I$(ROOT)/src/services \
		-I$(ROOT)/components/stb_image/include -I$(ROOT)/components/webp_decoder/include -I$(QUIRC_DIR) \
		$(filter-out vision_test.c,$^) -Wl,--gc-sections -lm -o $@

imlib_lua_test: imlib_lua_test.c imlib_test.c vision_test.c $(ROOT)/src/apps/solar_os_lua_imlib.inc \
        $(ROOT)/src/services/solar_os_imlib.c $(ROOT)/src/services/solar_os_raster_image.c \
        $(ROOT)/src/services/solar_os_vision.c $(IMLIB_SOURCES) $(LUA_TEST_SOURCES)
	$(CC) $(CFLAGS) -D_GNU_SOURCE -pthread $(IMLIB_FLAGS) -I$(ROOT)/src -Irtsp_stubs -I. -I$(ROOT)/src/services \
		-I$(ROOT)/src/apps -I$(LUA_DIR) -I$(ROOT)/components/stb_image/include \
		-I$(ROOT)/components/webp_decoder/include -I$(QUIRC_DIR) \
		$(filter-out imlib_test.c vision_test.c %.inc,$^) -Wl,--gc-sections -lm -ldl -o $@
generated/vision/%.o: $(QUIRC_DIR)/%.c $(QUIRC_DIR)/quirc.h $(QUIRC_DIR)/quirc_internal.h
	@mkdir -p generated/vision
	$(CC) $(CFLAGS) -Wno-sign-compare -Wno-unused-parameter \
		-DQUIRC_FLOAT_TYPE=float -DQUIRC_USE_TGMATH=1 \
		-Dmalloc=solar_os_quirc_malloc -Dcalloc=solar_os_quirc_calloc -Dfree=solar_os_quirc_free -c $< -o $@

vision_test: vision_test.c $(ROOT)/src/services/solar_os_vision.c \
		$(ROOT)/src/services/solar_os_raster_image.c $(QUIRC_HOST_OBJECTS)
	$(CC) $(CFLAGS) -Wno-unused-function -D_POSIX_C_SOURCE=200809L -pthread \
		-I$(ROOT)/src -Irtsp_stubs -I. -I$(ROOT)/src/services -I$(QUIRC_DIR) \
		-I$(ROOT)/components/stb_image/include -I$(ROOT)/components/webp_decoder/include $^ -lm -o $@

SCRIPT_MEDIA_TEST_SOURCES := script_media_test.c $(ROOT)/src/services/solar_os_script_media.c \
	$(ROOT)/src/services/solar_os_stream.c $(ROOT)/src/services/solar_os_camera.c \
	$(ROOT)/src/services/solar_os_camera_stream.c
script_media_test script_media_rtsp_test: $(SCRIPT_MEDIA_TEST_SOURCES)
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h -pthread \
		$(if $(filter script_media_rtsp_test,$@),-DSOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT=1,) \
		-Iscript_media_stubs -Irtsp_stubs -I. -I$(ROOT)/src -I$(ROOT)/src/services \
		$(SCRIPT_MEDIA_TEST_SOURCES) -o $@

rtspd_completion_test: rtspd_completion_test.c $(ROOT)/src/shell/solar_os_shell_rtspd_completion.c $(ROOT)/src/shell/solar_os_shell_rtspd_completion.h $(ROOT)/src/services/solar_os_stream.h
	$(CC) $(CFLAGS) -I. -I$(ROOT)/src/services -I$(ROOT)/src/shell rtspd_completion_test.c $(ROOT)/src/shell/solar_os_shell_rtspd_completion.c -o $@

rgb565_raster_test: rgb565_raster_test.c $(ROOT)/src/services/solar_os_rgb565.h $(ROOT)/src/services/solar_os_display_surface.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/services $< -o $@

gfx_mono_raster_test: gfx_mono_raster_test.c $(ROOT)/src/services/solar_os_gfx_mono_raster.h $(ROOT)/src/services/solar_os_display_surface.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/services $< -o $@

RTSP_CLIENT_TEST_SOURCES := rtsp_client_test.c $(ROOT)/src/services/solar_os_rtsp_receiver.c $(ROOT)/src/services/solar_os_rtsp.c $(ROOT)/src/services/solar_os_rtp.c $(ROOT)/src/services/solar_os_rtp_jpeg.c $(ROOT)/src/services/solar_os_media.c $(ROOT)/src/services/solar_os_audio_pcm.c $(ROOT)/src/services/solar_os_rtsp_auth.c
rtsp_client_test: $(RTSP_CLIENT_TEST_SOURCES) $(ROOT)/src/services/solar_os_rtsp_client.c $(wildcard rtsp_stubs/*.h) $(wildcard rtsp_stubs/freertos/*.h)
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -pthread -Irtsp_stubs -Irtspd_stubs -I. -I$(ROOT)/src -I$(ROOT)/src/services $(RTSP_CLIENT_TEST_SOURCES) -o $@

rtsp_auth_test: rtsp_auth_test.c $(ROOT)/src/services/solar_os_rtsp_auth.c $(ROOT)/src/services/solar_os_rtsp_receiver.c $(ROOT)/src/services/solar_os_rtsp.c $(ROOT)/src/services/solar_os_rtp.c $(ROOT)/src/services/solar_os_media.c
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -I. -I$(ROOT)/src/services $^ -o $@

rtsp_receiver_test: rtsp_receiver_test.c $(ROOT)/src/services/solar_os_rtsp_receiver.c $(ROOT)/src/services/solar_os_rtsp.c $(ROOT)/src/services/solar_os_rtp.c $(ROOT)/src/services/solar_os_media.c
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -I. -I$(ROOT)/src/services $^ -o $@

RTSPD_TEST_SOURCES := rtspd_job_test.c $(ROOT)/src/jobs/solar_os_rtspd_options.c \
	$(ROOT)/src/services/solar_os_media.c $(ROOT)/src/services/solar_os_rtp.c \
	$(ROOT)/src/services/solar_os_rtp_jpeg.c $(ROOT)/src/services/solar_os_rtsp.c
rtspd_job_test rtspd_audio_only_test: $(RTSPD_TEST_SOURCES) $(ROOT)/src/jobs/solar_os_rtspd_job.c \
	$(wildcard rtspd_stubs/*.h) $(wildcard rtspd_stubs/*/*.h)
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -pthread \
		$(if $(filter rtspd_audio_only_test,$@),-DSOLAR_OS_PACKAGE_SERVICE_CAMERA=0,) \
		-Irtspd_stubs -I. -I$(ROOT)/src -I$(ROOT)/src/services -I$(ROOT)/src/jobs $(RTSPD_TEST_SOURCES) -o $@

all: $(TARGETS)

ble_multi_peer_test: ble_multi_peer_test.c ble_test_support.c $(ROOT)/src/services/solar_os_ble.c $(ROOT)/src/services/solar_os_ble.h
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -pthread -include compat.h -Ible_stubs -I. -I$(ROOT)/src/services \
		ble_multi_peer_test.c ble_test_support.c -o $@

ble_nimble_test ble_hid_test ble_server_test: %: %.c nimble_test_support.c ble_test_support.c \
		$(ROOT)/src/services/solar_os_ble_nimble.c $(ROOT)/src/services/solar_os_ble_hid.c \
		$(ROOT)/src/services/solar_os_ble_hid_report_map.c \
		$(ROOT)/src/services/solar_os_ble_nimble_server.inc $(ROOT)/src/services/solar_os_ble_hid_hosts.inc \
		$(ROOT)/src/services/solar_os_ble_hid_name.inc \
		$(wildcard $(ROOT)/src/services/solar_os_ble*.h) \
		$(wildcard ble_stubs/freertos/*.h) \
		$(wildcard nimble_stubs/*/*/*.h) $(wildcard nimble_stubs/*/*.h) \
		$(wildcard nimble_stubs/*.h)
	$(CC) $(CFLAGS) -Wno-unused-parameter -D_POSIX_C_SOURCE=200809L -pthread -include compat.h \
		-Inimble_stubs -Ible_stubs -I. -I$(ROOT)/src/services \
		$< nimble_test_support.c ble_test_support.c $(ROOT)/src/services/solar_os_ble_hid_report_map.c -o $@


espdl_workers_test espdl_workers_single_test: espdl_workers_test.cpp $(ROOT)/scripts/espdl/worker_runtime.inc
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Werror -pthread \
		$(if $(filter espdl_workers_single_test,$@),-DCONFIG_FREERTOS_NUMBER_OF_CORES=1,) $< -o $@

tensor_image_lua_test: tensor_image_lua_test.c tensor_image_test.c \
        $(ROOT)/src/apps/solar_os_lua_tensor_image.inc $(ROOT)/src/services/solar_os_tensor_image.c $(LUA_TEST_SOURCES)
	$(CC) $(CFLAGS) -Iinference_stubs -Irtsp_stubs -I. -I$(ROOT)/src -I$(ROOT)/src/services \
		-I$(ROOT)/src/apps -I$(LUA_DIR) $(filter-out tensor_image_test.c %.inc,$^) -lm -o $@

inference_lua_json_test: inference_lua_bindings_test.c inference_test.c \
        $(ROOT)/src/apps/solar_os_lua_inference.inc $(ROOT)/src/services/solar_os_inference.c $(LUA_TEST_SOURCES)
	$(CC) $(CFLAGS) -Wno-unused-function -D_POSIX_C_SOURCE=200809L -pthread -DSOLAR_OS_PACKAGE_SERVICE_JSON=1 \
		-Iinference_stubs -Irtsp_stubs -I. -I$(ROOT)/src -I$(ROOT)/src/services \
		-I$(ROOT)/src/apps -I$(LUA_DIR) -I$(CJSON_DIR) \
		$(filter-out inference_test.c %.inc,$^) $(CJSON_DIR)/cJSON.c -lm -o $@

inference_test: inference_test.c $(ROOT)/src/services/solar_os_inference.c $(ROOT)/src/services/solar_os_inference.h
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -pthread \
		-Iinference_stubs -Irtsp_stubs -I. -I$(ROOT)/src -I$(ROOT)/src/services \
		$(filter %.c,$^) -o $@

inference_lua_bindings_test: inference_lua_bindings_test.c inference_test.c \
		$(ROOT)/src/apps/solar_os_lua_inference.inc $(ROOT)/src/services/solar_os_inference.c $(LUA_TEST_SOURCES)
	$(CC) $(CFLAGS) -Wno-unused-function -D_POSIX_C_SOURCE=200809L -pthread \
		-Iinference_stubs -Irtsp_stubs -I. -I$(ROOT)/src -I$(ROOT)/src/services \
		-I$(ROOT)/src/apps -I$(LUA_DIR) $(filter-out inference_test.c %.inc,$^) -lm -o $@

inference_schema_test: inference_schema_test.cpp $(ROOT)/components/espdl_schema/solar_os_espdl_validate.cpp
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Werror -idirafter $(ROOT)/include -I. -I$(ROOT)/src/services \
		-I$(ROOT)/components/espdl_schema/include $^ -o $@

generated/vision/solar_os_lua_vision.inc: $(WRITER_DIR)/solar_os_lua_media.inc
	@mkdir -p generated/vision
	python3 -c 'from pathlib import Path; s = Path("$<").read_text(); Path("$@").write_text(s[s.index("#if SOLAR_OS_PACKAGE_SERVICE_VISION"):])'

vision_lua_bindings_test: vision_lua_bindings_test.c vision_test.c \
		$(ROOT)/src/apps/solar_os_lua_image_rgb.inc \
		$(ROOT)/src/services/solar_os_vision.c $(ROOT)/src/services/solar_os_raster_image.c \
		generated/vision/solar_os_lua_vision.inc $(QUIRC_HOST_OBJECTS) $(LUA_TEST_SOURCES)
	$(CC) $(CFLAGS) -Wno-unused-function -D_POSIX_C_SOURCE=200809L -pthread \
		-Igenerated/vision -I$(ROOT)/src -I$(ROOT)/src/apps -Irtsp_stubs -I. -I$(ROOT)/src/services -I$(QUIRC_DIR) -I$(LUA_DIR) \
		-I$(ROOT)/components/stb_image/include -I$(ROOT)/components/webp_decoder/include \
		$(filter-out vision_test.c %.inc,$^) -lm -o $@

media_lua_bindings_test: media_lua_bindings_test.c $(SCRIPT_MEDIA_TEST_SOURCES) \
		$(WRITER_DIR)/solar_os_lua_media.inc $(LUA_TEST_SOURCES)
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h -pthread \
		-DSOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT=1 -DSOLAR_OS_PACKAGE_SERVICE_CAMERA=1 \
		-Iscript_media_stubs -Irtsp_stubs -I. -I$(ROOT)/src -I$(ROOT)/src/services \
		-I$(WRITER_DIR) -I$(LUA_DIR) media_lua_bindings_test.c $(filter-out script_media_test.c,$(SCRIPT_MEDIA_TEST_SOURCES)) \
		$(LUA_TEST_SOURCES) -lm -o $@

ble_lua_bindings_test: ble_lua_bindings_test.c ble_service_test.c ble_test_support.c \
		$(WRITER_DIR)/solar_os_lua_ble.inc $(ROOT)/src/services/solar_os_ble.c \
		$(ROOT)/src/services/solar_os_ble.h $(ROOT)/src/services/solar_os_ble_backend.h \
		ble_stubs/freertos/semphr.h ble_stubs/freertos/task.h $(LUA_TEST_SOURCES)
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -pthread -include compat.h \
		-Ible_stubs -I. -I$(ROOT)/src/services -I$(WRITER_DIR) -I$(LUA_DIR) \
		ble_lua_bindings_test.c ble_test_support.c $(ROOT)/src/services/solar_os_ble.c \
		$(LUA_TEST_SOURCES) -lm -o $@

ble_service_test: ble_service_test.c ble_test_support.c $(ROOT)/src/services/solar_os_ble.c \
		$(ROOT)/src/services/solar_os_ble.h $(ROOT)/src/services/solar_os_ble_backend.h \
		ble_stubs/freertos/semphr.h ble_stubs/freertos/task.h
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -pthread -include compat.h -Ible_stubs -I. -I$(ROOT)/src/services \
		ble_service_test.c ble_test_support.c $(ROOT)/src/services/solar_os_ble.c -o $@

writer_buffer_test: writer_buffer_test.c $(WRITER_DIR)/solar_os_writer_buffer.c $(WRITER_DIR)/solar_os_writer_buffer.h esp_err.h
	$(CC) $(CFLAGS) -DSOLAR_OS_WRITER_HOST_TEST -I. -I$(WRITER_DIR) \
		writer_buffer_test.c $(WRITER_DIR)/solar_os_writer_buffer.c -o $@

doc_layout_test: doc_layout_test.c doc_test_support.c $(ROOT)/src/services/solar_os_doc.c $(ROOT)/src/services/solar_os_doc.h
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-variable \
		-D_GNU_SOURCE -ffunction-sections -fdata-sections -include compat.h \
		-I. -I$(ROOT)/src -I$(ROOT)/src/services \
		doc_layout_test.c doc_test_support.c $(ROOT)/src/services/solar_os_doc.c \
		-Wl,--gc-sections -o $@

writer_files_test: writer_files_test.c $(WRITER_DIR)/solar_os_writer_files.c $(WRITER_DIR)/solar_os_writer_files.h esp_err.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I. -I$(ROOT)/src/services -I$(WRITER_DIR) \
		writer_files_test.c $(WRITER_DIR)/solar_os_writer_files.c -o $@

file_shortcuts_test: file_shortcuts_test.c $(WRITER_DIR)/solar_os_file_shortcuts.h
	$(CC) $(CFLAGS) -I. -I$(ROOT)/src/services -I$(WRITER_DIR) \
		file_shortcuts_test.c -o $@

sketch_canvas_test: sketch_canvas_test.c $(WRITER_DIR)/solar_os_sketch_canvas.c $(WRITER_DIR)/solar_os_sketch_canvas.h esp_err.h
	$(CC) $(CFLAGS) -I. -I$(WRITER_DIR) \
		sketch_canvas_test.c $(WRITER_DIR)/solar_os_sketch_canvas.c -o $@

launcher_layout_test: launcher_layout_test.c $(WRITER_DIR)/solar_os_launcher_layout.c $(WRITER_DIR)/solar_os_launcher_layout.h
	$(CC) $(CFLAGS) -I$(WRITER_DIR) \
		launcher_layout_test.c $(WRITER_DIR)/solar_os_launcher_layout.c -o $@

display_layout_test: display_layout_test.c $(ROOT)/src/services/solar_os_display_layout_core.c $(ROOT)/src/services/solar_os_display_layout_core.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/services \
		display_layout_test.c $(ROOT)/src/services/solar_os_display_layout_core.c -o $@

gfx_icons_test: gfx_icons_test.c $(ROOT)/src/services/solar_os_gfx_icons.c \
		$(ROOT)/src/services/solar_os_gfx.h \
		$(ROOT)/src/services/solar_os_gfx_icons.h \
		$(ROOT)/src/services/solar_os_gfx_icon_table.inc esp_err.h
	$(CC) $(CFLAGS) -I. -I$(ROOT)/src/services \
		gfx_icons_test.c $(ROOT)/src/services/solar_os_gfx_icons.c -o $@

text_search_test: text_search_test.c $(ROOT)/src/services/solar_os_text_search.c $(ROOT)/src/services/solar_os_text_search.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/services \
		text_search_test.c $(ROOT)/src/services/solar_os_text_search.c -o $@

shell_parse_test: shell_parse_test.c $(ROOT)/src/shell/solar_os_shell_parse.c $(ROOT)/src/shell/solar_os_shell_parse.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/shell \
		shell_parse_test.c $(ROOT)/src/shell/solar_os_shell_parse.c -o $@

shell_launch_test: shell_launch_test.c $(ROOT)/src/shell/solar_os_shell_launch.c $(ROOT)/src/shell/solar_os_shell_launch.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/shell \
		shell_launch_test.c $(ROOT)/src/shell/solar_os_shell_launch.c -o $@

shell_line_test: shell_line_test.c $(ROOT)/src/shell/solar_os_shell_line.c $(ROOT)/src/shell/solar_os_shell_line.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/shell \
		shell_line_test.c $(ROOT)/src/shell/solar_os_shell_line.c -o $@

shell_completion_test: shell_completion_test.c $(ROOT)/src/shell/solar_os_shell_completion.c $(ROOT)/src/shell/solar_os_shell_completion.h $(ROOT)/src/shell/solar_os_shell_parse.c $(ROOT)/src/shell/solar_os_shell_parse.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/shell \
		shell_completion_test.c $(ROOT)/src/shell/solar_os_shell_completion.c \
		$(ROOT)/src/shell/solar_os_shell_parse.c -o $@

gesture_completion_test: gesture_completion_test.c $(ROOT)/src/shell/solar_os_shell_gesture_completion.c $(ROOT)/src/shell/solar_os_shell_gesture_completion.h
	$(CC) $(CFLAGS) -I. -I$(ROOT)/src/services -I$(ROOT)/src/shell \
		gesture_completion_test.c $(ROOT)/src/shell/solar_os_shell_gesture_completion.c -o $@

shell_diag_test: shell_diag_test.c $(ROOT)/src/shell/solar_os_shell_common.c $(ROOT)/src/shell/solar_os_shell_common.h $(ROOT)/src/shell/solar_os_shell_io.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -ffunction-sections -fdata-sections \
		-include compat.h -I. -I$(ROOT)/src -I$(ROOT)/src/services \
		-I$(ROOT)/src/shell \
		shell_diag_test.c $(ROOT)/src/shell/solar_os_shell_common.c \
		-Wl,--gc-sections -o $@

shell_startup_policy_test: shell_startup_policy_test.c $(ROOT)/src/apps/solar_os_shell_startup_policy.c $(ROOT)/src/apps/solar_os_shell_startup_policy.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/apps \
		shell_startup_policy_test.c $(ROOT)/src/apps/solar_os_shell_startup_policy.c -o $@

scheduler_test: scheduler_test.c $(ROOT)/src/solar_os_scheduler.c $(ROOT)/src/solar_os_scheduler.h esp_timer.h
	$(CC) $(CFLAGS) -I. -I$(ROOT)/src \
		scheduler_test.c $(ROOT)/src/solar_os_scheduler.c -o $@

script_lifecycle_test: script_lifecycle_test.c $(ROOT)/src/services/solar_os_script_lifecycle.c $(ROOT)/src/services/solar_os_script_lifecycle.h freertos/FreeRTOS.h freertos/task.h
	$(CC) $(CFLAGS) -I. -I$(ROOT)/src/services \
		script_lifecycle_test.c $(ROOT)/src/services/solar_os_script_lifecycle.c -o $@

app_file_types_test: app_file_types_test.c $(ROOT)/src/apps/solar_os_app_file_types.c $(ROOT)/src/apps/solar_os_app_file_types.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/apps \
		app_file_types_test.c $(ROOT)/src/apps/solar_os_app_file_types.c -o $@

gameboy_support_test: gameboy_support_test.c $(WRITER_DIR)/solar_os_gameboy_rom.c $(WRITER_DIR)/solar_os_gameboy_rom.h $(WRITER_DIR)/solar_os_gameboy_video.c $(WRITER_DIR)/solar_os_gameboy_video.h $(WRITER_DIR)/solar_os_gameboy_apu.c $(ROOT)/src/vendor/peanut_gb/peanut_gb.h $(ROOT)/src/vendor/peanut_gb/minigb_apu.c $(ROOT)/src/vendor/peanut_gb/minigb_apu.h
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-variable \
		-DSOLAR_OS_GAMEBOY_APU_HOST_TEST -I$(ROOT)/src -I$(WRITER_DIR) \
		gameboy_support_test.c $(WRITER_DIR)/solar_os_gameboy_rom.c \
		$(WRITER_DIR)/solar_os_gameboy_video.c \
		$(WRITER_DIR)/solar_os_gameboy_apu.c -o $@

storage_mounts_test: storage_mounts_test.c $(ROOT)/src/services/solar_os_storage.c $(ROOT)/src/services/solar_os_storage.h esp_log.h ff.h solar_os_board.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -ffunction-sections -fdata-sections -include compat.h \
		-I. -I$(ROOT)/src/services -I$(ROOT)/src/board -I$(ROOT)/src/drivers \
		storage_mounts_test.c $(ROOT)/src/services/solar_os_storage.c \
		-Wl,--gc-sections,--wrap=fwrite,--wrap=fsync,--wrap=fclose -o $@

json_scan_test: json_scan_test.c $(ROOT)/src/services/solar_os_json_scan.c $(ROOT)/src/services/solar_os_json_scan.h esp_err.h
	$(CC) $(CFLAGS) -I. -I$(ROOT)/src/services \
		json_scan_test.c $(ROOT)/src/services/solar_os_json_scan.c -o $@

native_elf_test: native_elf_test.c $(ROOT)/src/services/solar_os_native_elf.c $(ROOT)/src/services/solar_os_native_elf.h esp_err.h
	$(CC) $(CFLAGS) -I. -I$(ROOT)/src/services \
		native_elf_test.c $(ROOT)/src/services/solar_os_native_elf.c -o $@

keys_test: keys_test.c $(ROOT)/src/services/solar_os_keys.c $(ROOT)/src/services/solar_os_keys.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/services \
		keys_test.c $(ROOT)/src/services/solar_os_keys.c -o $@

input_test: input_test.c $(ROOT)/src/services/solar_os_input.c $(ROOT)/src/services/solar_os_input.h $(ROOT)/src/services/solar_os_keys.c $(ROOT)/src/services/solar_os_keys.h $(ROOT)/src/services/solar_os_memory.h esp_timer.h nvs.h freertos/FreeRTOS.h
	$(CC) $(CFLAGS) -include compat.h -I. -I$(ROOT)/src -I$(ROOT)/src/services \
		input_test.c $(ROOT)/src/services/solar_os_input.c \
		$(ROOT)/src/services/solar_os_keys.c -o $@

input_action_match_test: input_action_match_test.c $(ROOT)/src/services/solar_os_input_action_match.c $(ROOT)/src/services/solar_os_input_action_match.h $(ROOT)/src/services/solar_os_input_actions.h
	$(CC) $(CFLAGS) -I. -I$(ROOT)/src/services \
		input_action_match_test.c $(ROOT)/src/services/solar_os_input_action_match.c -o $@

unistroke_test: unistroke_test.c $(ROOT)/src/services/solar_os_unistroke.c $(ROOT)/src/services/solar_os_unistroke.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/services \
		unistroke_test.c $(ROOT)/src/services/solar_os_unistroke.c -lm -o $@

graffiti_test: graffiti_test.c $(ROOT)/src/services/solar_os_graffiti.c $(ROOT)/src/services/solar_os_graffiti.h $(ROOT)/src/services/solar_os_unistroke.c $(ROOT)/src/services/solar_os_unistroke.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/services \
		graffiti_test.c $(ROOT)/src/services/solar_os_graffiti.c \
		$(ROOT)/src/services/solar_os_unistroke.c -lm -o $@

board_caps_test: board_caps_test.c $(ROOT)/src/services/solar_os_board_caps.c $(ROOT)/src/services/solar_os_board_caps.h solar_os_board.h
	$(CC) $(CFLAGS) -I. -I$(ROOT)/src/services \
		-DSOLAR_OS_BOARD_HAS_PSRAM=1 -DSOLAR_OS_BOARD_HAS_DISPLAY=1 \
		-DSOLAR_OS_BOARD_HAS_GFX=1 -DSOLAR_OS_BOARD_HAS_CDC=1 \
		-DSOLAR_OS_BOARD_HAS_UART=1 -DSOLAR_OS_BOARD_HAS_I2C=1 \
		-DSOLAR_OS_BOARD_HAS_SPI=1 -DSOLAR_OS_BOARD_HAS_RTC=1 \
		-DSOLAR_OS_BOARD_HAS_BATTERY=1 -DSOLAR_OS_BOARD_HAS_AUDIO=1 \
		-DSOLAR_OS_BOARD_HAS_AUDIO_INPUT=1 -DSOLAR_OS_BOARD_HAS_WIFI=1 \
		-DSOLAR_OS_BOARD_HAS_BLE=1 -DSOLAR_OS_BOARD_HAS_GPIO=1 \
		-DSOLAR_OS_BOARD_HAS_POINTER=1 \
		-DSOLAR_OS_BOARD_HAS_STREAMING_DISPLAY=1 \
		-DSOLAR_OS_BOARD_HAS_ADC=1 -DSOLAR_OS_BOARD_HAS_PWM=1 \
		-DSOLAR_OS_BOARD_HAS_EXPANSION_GPIO=1 \
		-DSOLAR_OS_BOARD_HAS_EXPANSION_I2C=1 \
		-DSOLAR_OS_BOARD_HAS_EXPANSION_SPI=1 \
		-DSOLAR_OS_BOARD_HAS_EXPANSION_UART=1 \
		-DSOLAR_OS_BOARD_HAS_EXPANSION_ADC=1 \
		-DSOLAR_OS_BOARD_HAS_EXPANSION_PWM=1 \
		-DSOLAR_OS_BOARD_HAS_EXPANSION_I2S=1 -DSOLAR_OS_BOARD_HAS_KEY=1 \
		-DSOLAR_OS_BOARD_HAS_TEMPERATURE=1 \
		-DSOLAR_OS_BOARD_HAS_HUMIDITY=1 -DSOLAR_OS_BOARD_HAS_SIMD=1 \
		board_caps_test.c $(ROOT)/src/services/solar_os_board_caps.c -o $@

hid_keyboard_report_test: hid_keyboard_report_test.c $(ROOT)/src/services/solar_os_hid_keyboard_report.c $(ROOT)/src/services/solar_os_hid_keyboard_report.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/services \
		hid_keyboard_report_test.c $(ROOT)/src/services/solar_os_hid_keyboard_report.c -o $@

ble_keyboard_scan_policy_test: ble_keyboard_scan_policy_test.c $(ROOT)/src/services/solar_os_ble_keyboard_scan_policy.c $(ROOT)/src/services/solar_os_ble_keyboard_scan_policy.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/services \
		ble_keyboard_scan_policy_test.c $(ROOT)/src/services/solar_os_ble_keyboard_scan_policy.c -o $@

ps2_keyboard_test: ps2_keyboard_test.c $(ROOT)/src/services/solar_os_ps2_keyboard.c $(ROOT)/src/services/solar_os_ps2_keyboard.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/services \
		ps2_keyboard_test.c $(ROOT)/src/services/solar_os_ps2_keyboard.c -o $@

ps2_mouse_test: ps2_mouse_test.c $(ROOT)/src/services/solar_os_ps2_mouse.c $(ROOT)/src/services/solar_os_ps2_mouse.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/services \
		ps2_mouse_test.c $(ROOT)/src/services/solar_os_ps2_mouse.c -o $@

cardkb_codec_test: cardkb_codec_test.c $(ROOT)/src/services/solar_os_cardkb_codec.c $(ROOT)/src/services/solar_os_cardkb_codec.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/services \
		cardkb_codec_test.c $(ROOT)/src/services/solar_os_cardkb_codec.c -o $@

$(KEYMAP_GENERATED_HEADERS) &: $(KEYMAP_INPUTS)
	python3 $(ROOT)/scripts/generate_keymaps.py --output-dir $(MANUAL_GENERATED_DIR)

inputronic_keyboard_test matrix_keyboard_test: $(KEYMAP_GENERATED_HEADERS)

inputronic_keyboard_test: inputronic_keyboard_test.c $(wildcard $(ROOT)/src/services/solar_os_inputronic_keyboard*) $(wildcard $(ROOT)/src/services/solar_os_lilygo_pager_keyboard*) $(wildcard $(ROOT)/src/services/solar_os_tca8418*) $(wildcard $(ROOT)/src/services/solar_os_matrix_keyboard*) $(wildcard $(ROOT)/src/services/solar_os_input_keymap*) $(wildcard inputronic_stubs/*.h inputronic_stubs/freertos/*.h inputronic_stubs/driver/*.h)
	$(CC) $(CFLAGS) -include compat.h -I$(MANUAL_GENERATED_DIR) -Iinputronic_stubs -Iexpansion_stubs -I. -I$(ROOT)/src -I$(ROOT)/src/services -I$(CJSON_DIR) \
		inputronic_keyboard_test.c $(ROOT)/src/services/solar_os_input_keymap.c \
		$(ROOT)/src/services/solar_os_input_keymap_mapper.c \
		$(ROOT)/src/services/solar_os_input_keymap_json.c $(CJSON_DIR)/cJSON.c -lm \
		$(ROOT)/src/services/solar_os_matrix_keyboard.c \
		$(ROOT)/src/services/solar_os_inputronic_keyboard_profile.c -o $@

matrix_keyboard_test: matrix_keyboard_test.c keymap_memory_stub.c $(wildcard $(ROOT)/src/services/solar_os_matrix_keyboard*) $(wildcard $(ROOT)/src/services/solar_os_input_keymap*) $(wildcard $(ROOT)/src/services/solar_os_inputronic_keyboard_profile*) $(wildcard $(ROOT)/src/services/solar_os_lilygo_pager_keyboard*) $(wildcard inputronic_stubs/*.h inputronic_stubs/freertos/*.h)
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections -include compat.h \
		-I$(MANUAL_GENERATED_DIR) -Iinputronic_stubs -Iexpansion_stubs -I. -I$(ROOT)/src -I$(ROOT)/src/services -I$(CJSON_DIR) \
		matrix_keyboard_test.c keymap_memory_stub.c $(ROOT)/src/services/solar_os_input_keymap_mapper.c $(ROOT)/src/services/solar_os_matrix_keyboard.c \
		$(ROOT)/src/services/solar_os_input_keymap_json.c \
		$(ROOT)/src/services/solar_os_inputronic_keyboard_profile.c \
		$(ROOT)/src/services/solar_os_lilygo_pager_keyboard.c $(CJSON_DIR)/cJSON.c \
		-Wl,--gc-sections -lm -o $@

input_keymap_service_test: input_keymap_service_test.c $(wildcard $(ROOT)/src/services/solar_os_input_keymap*) $(wildcard inputronic_stubs/freertos/*.h)
	$(CC) $(CFLAGS) -include compat.h -Iinputronic_stubs -Iexpansion_stubs -I. \
		-I$(ROOT)/src -I$(ROOT)/src/services -I$(CJSON_DIR) input_keymap_service_test.c \
		$(ROOT)/src/services/solar_os_input_keymap.c $(ROOT)/src/services/solar_os_input_keymap_mapper.c \
		$(ROOT)/src/services/solar_os_input_keymap_json.c $(CJSON_DIR)/cJSON.c -lm -o $@

ubx_test: ubx_test.c $(ROOT)/src/drivers/ubx.c $(ROOT)/src/drivers/ubx.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/drivers \
		ubx_test.c $(ROOT)/src/drivers/ubx.c -o $@

nmea_test: nmea_test.c $(ROOT)/src/drivers/nmea.c $(ROOT)/src/drivers/nmea.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/drivers \
		nmea_test.c $(ROOT)/src/drivers/nmea.c -o $@

nmea_gnss_test: nmea_gnss_test.c $(ROOT)/src/drivers/nmea.c $(ROOT)/src/drivers/nmea.h \
        $(ROOT)/src/services/solar_os_nmea_gnss.c $(ROOT)/src/services/solar_os_nmea_gnss.h \
        $(ROOT)/src/services/solar_os_gnss.c $(ROOT)/src/services/solar_os_gnss.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h \
		-Iexpansion_stubs -I. -I$(ROOT)/include -I$(ROOT)/src/services -I$(ROOT)/src/drivers \
		nmea_gnss_test.c $(ROOT)/src/drivers/nmea.c $(ROOT)/src/services/solar_os_gnss.c -o $@

gnss_service_test: gnss_service_test.c $(ROOT)/src/services/solar_os_gnss.c $(ROOT)/src/services/solar_os_gnss.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h -I. -I$(ROOT)/src/services \
		gnss_service_test.c $(ROOT)/src/services/solar_os_gnss.c -o $@

haptic_service_test: haptic_service_test.c $(ROOT)/src/services/solar_os_haptic.c $(ROOT)/src/services/solar_os_haptic.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h -I. -I$(ROOT)/src/services \
		haptic_service_test.c $(ROOT)/src/services/solar_os_haptic.c -o $@

charger_service_test: charger_service_test.c $(ROOT)/src/services/solar_os_charger.c $(ROOT)/src/services/solar_os_charger.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h -I. -I$(ROOT)/src/services \
		charger_service_test.c $(ROOT)/src/services/solar_os_charger.c -o $@

CAMERA_EXPANSION_SOURCES := camera_expansion_test.c $(ROOT)/src/drivers/solar_os_camera_esp32.c \
	$(ROOT)/src/services/solar_os_camera.c $(ROOT)/src/services/solar_os_camera_stream.c \
	$(ROOT)/src/services/solar_os_stream.c $(ROOT)/src/services/solar_os_resources.c
camera_expansion_test: $(CAMERA_EXPANSION_SOURCES) $(wildcard camera_stubs/*.h camera_stubs/*/*.h) \
	$(ROOT)/src/services/solar_os_camera.h $(ROOT)/src/services/solar_os_stream.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h -Icamera_stubs -Iexpansion_stubs -I. \
		-I$(ROOT)/include -I$(ROOT)/src -I$(ROOT)/src/services -I$(ROOT)/src/drivers \
		$(CAMERA_EXPANSION_SOURCES) -o $@

camera_service_test: camera_service_test.c $(ROOT)/src/services/solar_os_camera.c $(ROOT)/src/services/solar_os_camera.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h -I. -I$(ROOT)/src/services \
		camera_service_test.c $(ROOT)/src/services/solar_os_camera.c -o $@

mjpeg_test: mjpeg_test.c $(ROOT)/src/services/solar_os_mjpeg.c $(ROOT)/src/services/solar_os_mjpeg.h
	$(CC) $(CFLAGS) -I. -I$(ROOT)/src/services \
		mjpeg_test.c $(ROOT)/src/services/solar_os_mjpeg.c -o $@

media_test: media_test.c $(ROOT)/src/services/solar_os_media.c \
		$(ROOT)/src/services/solar_os_rtp.c \
		$(ROOT)/src/services/solar_os_rtp_jpeg.c \
		$(ROOT)/src/services/solar_os_rtsp.c \
		$(ROOT)/src/services/solar_os_media.h $(ROOT)/src/services/solar_os_rtp.h \
		$(ROOT)/src/services/solar_os_rtp_jpeg.h \
		$(ROOT)/src/services/solar_os_rtsp.h
	$(CC) $(CFLAGS) -I. -I$(ROOT)/src/services \
		media_test.c $(ROOT)/src/services/solar_os_media.c \
		$(ROOT)/src/services/solar_os_rtp.c \
		$(ROOT)/src/services/solar_os_rtp_jpeg.c \
		$(ROOT)/src/services/solar_os_rtsp.c -o $@

http_server_test: http_server_test.c $(ROOT)/src/services/solar_os_http_server.c $(ROOT)/src/services/solar_os_http_server.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -pthread -include compat.h \
		-Ihttp_server_stubs -I. -I$(ROOT)/src/services \
		http_server_test.c $(ROOT)/src/services/solar_os_http_server.c -o $@

drv2605_test: drv2605_test.c $(ROOT)/src/drivers/drv2605.c $(ROOT)/src/drivers/drv2605.h
	$(CC) $(CFLAGS) -I. -I$(ROOT)/src/drivers \
		drv2605_test.c $(ROOT)/src/drivers/drv2605.c -o $@

bq25896_test: bq25896_test.c $(ROOT)/src/drivers/bq25896.c $(ROOT)/src/drivers/bq25896.h
	$(CC) $(CFLAGS) -I. -I$(ROOT)/src/drivers \
		bq25896_test.c $(ROOT)/src/drivers/bq25896.c -o $@

axp2101_test: axp2101_test.c $(ROOT)/src/drivers/axp2101.c $(ROOT)/src/drivers/axp2101.h
	$(CC) $(CFLAGS) -I. -I$(ROOT)/src/drivers \
		axp2101_test.c $(ROOT)/src/drivers/axp2101.c -o $@

qmi8658_test: qmi8658_test.c $(ROOT)/src/drivers/qmi8658.c $(ROOT)/src/drivers/qmi8658.h
	$(CC) $(CFLAGS) -I. -I$(ROOT)/src/drivers \
		qmi8658_test.c $(ROOT)/src/drivers/qmi8658.c -lm -o $@

max17048_test: max17048_test.c $(ROOT)/src/drivers/max17048.c $(ROOT)/src/drivers/max17048.h
	$(CC) $(CFLAGS) -I. -I$(ROOT)/src/drivers \
		max17048_test.c $(ROOT)/src/drivers/max17048.c -o $@

mgc3130_test: mgc3130_test.c $(ROOT)/src/drivers/mgc3130.c $(ROOT)/src/drivers/mgc3130.h
	$(CC) $(CFLAGS) -I. -I$(ROOT)/src/drivers \
		mgc3130_test.c $(ROOT)/src/drivers/mgc3130.c -o $@

sim7670_test: sim7670_test.c $(ROOT)/src/drivers/sim7670.c $(ROOT)/src/drivers/sim7670.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I. -I$(ROOT)/src/drivers \
		sim7670_test.c $(ROOT)/src/drivers/sim7670.c -o $@

modem_service_test: modem_service_test.c $(ROOT)/src/services/solar_os_modem.c $(ROOT)/src/services/solar_os_modem.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h -I. -I$(ROOT)/src/services \
		modem_service_test.c $(ROOT)/src/services/solar_os_modem.c -o $@

imu_service_test: imu_service_test.c $(ROOT)/src/services/solar_os_imu.c $(ROOT)/src/services/solar_os_imu.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h -I. -I$(ROOT)/src/services \
		imu_service_test.c $(ROOT)/src/services/solar_os_imu.c -lm -o $@

bhi260ap_codec_test: bhi260ap_codec_test.c $(ROOT)/src/drivers/bhi260ap_codec.c \
		$(ROOT)/src/drivers/bhi260ap_codec.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/drivers \
		bhi260ap_codec_test.c $(ROOT)/src/drivers/bhi260ap_codec.c -lm -o $@

xl9555_test: xl9555_test.c $(ROOT)/src/drivers/xl9555.c \
		$(ROOT)/src/drivers/xl9555.h esp_err.h
	$(CC) $(CFLAGS) -I. -I$(ROOT)/src/drivers \
		xl9555_test.c $(ROOT)/src/drivers/xl9555.c -o $@

st25r3916_test: st25r3916_test.c $(ROOT)/src/drivers/st25r3916.c \
		$(ROOT)/src/drivers/st25r3916.h esp_err.h
	$(CC) $(CFLAGS) -I. -I$(ROOT)/src/drivers \
		st25r3916_test.c $(ROOT)/src/drivers/st25r3916.c -o $@

nfc_service_test: nfc_service_test.c $(ROOT)/src/services/solar_os_nfc.c $(ROOT)/src/services/solar_os_nfc.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h -I. -I$(ROOT)/src/services \
		nfc_service_test.c $(ROOT)/src/services/solar_os_nfc.c -o $@

cl32_keyboard_test: cl32_keyboard_test.c $(ROOT)/src/services/solar_os_cl32_keyboard.c $(ROOT)/src/services/solar_os_cl32_keyboard.h
	$(CC) $(CFLAGS) -I. -I$(ROOT)/src/services \
		cl32_keyboard_test.c $(ROOT)/src/services/solar_os_cl32_keyboard.c -o $@

battery_service_test: battery_service_test.c $(ROOT)/src/services/solar_os_battery.c $(ROOT)/src/services/solar_os_battery.h $(ROOT)/src/services/solar_os_battery_trend.c $(ROOT)/src/services/solar_os_battery_trend.h nvs.h freertos/FreeRTOS.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h -I. -I$(ROOT)/src/services \
		battery_service_test.c $(ROOT)/src/services/solar_os_battery.c \
		$(ROOT)/src/services/solar_os_battery_trend.c -o $@

battery_trend_test: battery_trend_test.c $(ROOT)/src/services/solar_os_battery_trend.c $(ROOT)/src/services/solar_os_battery_trend.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/services \
		battery_trend_test.c $(ROOT)/src/services/solar_os_battery_trend.c -o $@

sensors_service_test: sensors_service_test.c $(ROOT)/src/services/solar_os_sensors.c $(ROOT)/src/services/solar_os_sensors.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h -I. -I$(ROOT)/src/services \
		sensors_service_test.c $(ROOT)/src/services/solar_os_sensors.c -o $@

midi_codec_test: midi_codec_test.c $(ROOT)/src/services/solar_os_midi_codec.c $(ROOT)/src/services/solar_os_midi_codec.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/services \
		midi_codec_test.c $(ROOT)/src/services/solar_os_midi_codec.c -o $@

midi_service_test: midi_service_test.c $(ROOT)/src/services/solar_os_midi.c $(ROOT)/src/services/solar_os_midi.h $(ROOT)/src/services/solar_os_midi_codec.c $(ROOT)/src/services/solar_os_stream.c $(ROOT)/src/services/solar_os_stream.h esp_err.h freertos/FreeRTOS.h freertos/semphr.h freertos/portmacro.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h -I. -I$(ROOT)/include \
		-I$(ROOT)/src/services \
		midi_service_test.c $(ROOT)/src/services/solar_os_midi.c \
		$(ROOT)/src/services/solar_os_midi_codec.c \
		$(ROOT)/src/services/solar_os_stream.c -o $@

parameters_test: parameters_test.c $(ROOT)/src/services/solar_os_parameters.c $(ROOT)/src/services/solar_os_parameters.h esp_attr.h esp_err.h freertos/FreeRTOS.h freertos/semphr.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h -I. -I$(ROOT)/src/services \
		parameters_test.c $(ROOT)/src/services/solar_os_parameters.c -lm -o $@

controls_test: controls_test.c $(ROOT)/src/services/solar_os_controls.c $(ROOT)/src/services/solar_os_controls.h esp_attr.h esp_err.h freertos/FreeRTOS.h freertos/semphr.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h -I. -I$(ROOT)/src/services \
		controls_test.c $(ROOT)/src/services/solar_os_controls.c -lm -o $@

osc_test: osc_test.c $(ROOT)/src/services/solar_os_osc.c $(ROOT)/src/services/solar_os_osc.h $(ROOT)/src/services/solar_os_parameters.c $(ROOT)/src/services/solar_os_parameters.h esp_attr.h esp_err.h freertos/FreeRTOS.h freertos/semphr.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h -I. -I$(ROOT)/src/services \
		osc_test.c $(ROOT)/src/services/solar_os_osc.c \
		$(ROOT)/src/services/solar_os_parameters.c -lm -o $@

dsp_test: dsp_test.c $(ROOT)/src/services/solar_os_dsp.c $(ROOT)/src/services/solar_os_dsp.h esp_err.h
	$(CC) $(CFLAGS) -DSOLAR_OS_DSP_HOST_TEST -I. -I$(ROOT)/src/services \
		dsp_test.c $(ROOT)/src/services/solar_os_dsp.c -lm -o $@

stream_test: stream_test.c $(ROOT)/src/services/solar_os_stream.c $(ROOT)/src/services/solar_os_stream.h esp_err.h freertos/FreeRTOS.h freertos/semphr.h freertos/portmacro.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h -I. -I$(ROOT)/src/services \
		stream_test.c $(ROOT)/src/services/solar_os_stream.c -o $@

resources_test: resources_test.c $(ROOT)/src/services/solar_os_resources.c $(ROOT)/src/services/solar_os_resources.h esp_attr.h esp_err.h freertos/FreeRTOS.h freertos/semphr.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h -I. -I$(ROOT)/src/services \
		resources_test.c $(ROOT)/src/services/solar_os_resources.c -o $@

gpio_controller_test: gpio_controller_test.c \
		$(ROOT)/src/drivers/gpio_port.c $(ROOT)/src/drivers/gpio_port.h \
		$(ROOT)/src/services/solar_os_gpio_controller.c \
		$(ROOT)/src/services/solar_os_gpio_controller.h \
		$(ROOT)/src/services/solar_os_resources.c $(ROOT)/src/services/solar_os_resources.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h -Igpio_controller_stubs \
		-I. -I$(ROOT)/src/drivers -I$(ROOT)/src/services \
		gpio_controller_test.c $(ROOT)/src/services/solar_os_gpio_controller.c \
		$(ROOT)/src/drivers/gpio_port.c \
		$(ROOT)/src/services/solar_os_resources.c -o $@

expansion_registry_test: expansion_registry_test.c \
		$(ROOT)/src/services/solar_os_ssd1677_driver.c \
		$(ROOT)/src/services/solar_os_expansion.c \
		$(ROOT)/src/services/solar_os_expansion.h \
		$(ROOT)/src/services/solar_os_memory.h \
		expansion_stubs/solar_os_board.h expansion_stubs/solar_os_config.h \
		expansion_stubs/driver/i2c_master.h \
		expansion_stubs/driver/spi_master.h expansion_stubs/soc/soc_caps.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -ffunction-sections -fdata-sections \
		-include compat.h -Iexpansion_stubs -I. -I$(ROOT)/include \
		-I$(ROOT)/src -I$(ROOT)/src/services expansion_registry_test.c \
		$(ROOT)/src/services/solar_os_expansion.c \
		$(ROOT)/src/services/solar_os_ssd1677_driver.c -Wl,--gc-sections -o $@

expansion_command_test: expansion_command_test.c \
		$(ROOT)/src/shell/solar_os_shell_expansion_command.c \
		$(ROOT)/src/shell/solar_os_shell_expansion_internal.h \
		$(ROOT)/src/services/solar_os_expansion.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h -I. \
		-I$(ROOT)/include -I$(ROOT)/src -I$(ROOT)/src/services \
		-I$(ROOT)/src/shell expansion_command_test.c \
		$(ROOT)/src/shell/solar_os_shell_expansion_command.c -o $@

audio_codec_test: audio_codec_test.c $(ROOT)/src/services/solar_os_audio_codec.c $(ROOT)/src/services/solar_os_audio_codec.h $(ROOT)/src/services/solar_os_audio_pcm.c $(ROOT)/src/services/solar_os_audio_pcm.h esp_err.h
	$(CC) $(CFLAGS) -DSOLAR_OS_AUDIO_CODEC_HOST_TEST -I. -I$(ROOT)/src/services -I$(ROOT)/components/minimp3/include \
		audio_codec_test.c $(ROOT)/src/services/solar_os_audio_codec.c \
		$(ROOT)/src/services/solar_os_audio_pcm.c \
		$(ROOT)/components/minimp3/minimp3_impl.c -o $@

webradio_catalog_test: webradio_catalog_test.c $(ROOT)/src/services/solar_os_webradio_catalog.c $(ROOT)/src/services/solar_os_webradio_catalog.h esp_attr.h esp_check.h esp_err.h nvs.h freertos/FreeRTOS.h freertos/portmacro.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h -I. -I$(ROOT)/src -I$(ROOT)/src/services \
		webradio_catalog_test.c $(ROOT)/src/services/solar_os_webradio_catalog.c -o $@

gif_decode_stack_test: gif_decode_stack_test.c deep_animation.gif.b64 esp_heap_caps.h esp_err.h $(ROOT)/components/stb_image/stb_image_port.c $(ROOT)/components/stb_image/jpeg_fast.c $(ROOT)/components/stb_image/jpeg_simd.c $(ROOT)/components/stb_image/include/solar_os_stb_image.h $(ROOT)/components/stb_image/include/stb_image.h
	$(CC) $(CFLAGS) -pthread -I. -I$(ROOT)/components/stb_image/include \
		gif_decode_stack_test.c $(ROOT)/components/stb_image/stb_image_port.c $(ROOT)/components/stb_image/jpeg_fast.c $(ROOT)/components/stb_image/jpeg_simd.c -o $@

nvs_backup_test: nvs_backup_test.c $(ROOT)/src/services/solar_os_nvs_backup.c $(ROOT)/src/services/solar_os_nvs_backup.h esp_partition.h nvs_flash.h esp_err.h freertos/FreeRTOS.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h -I. -I$(ROOT)/src/services \
		nvs_backup_test.c $(ROOT)/src/services/solar_os_nvs_backup.c -o $@

$(MANUAL_GENERATED_HEADER): $(ROOT)/scripts/generate_manual.py $(ROOT)/packages/solar_os_packages.toml $(wildcard $(ROOT)/doc/manual/*.md)
	mkdir -p $(MANUAL_GENERATED_DIR)
	python3 $(ROOT)/scripts/generate_manual.py --input $(ROOT)/doc/manual \
		--packages $(ROOT)/packages/solar_os_packages.toml --output $@

agent_reference_test: agent_reference_test.c $(MANUAL_GENERATED_HEADER) $(ROOT)/src/services/solar_os_agent_reference.c $(ROOT)/src/services/solar_os_agent_reference.h $(ROOT)/src/services/solar_os_manual.c $(ROOT)/src/services/solar_os_manual.h
	$(CC) $(CFLAGS) -Wno-comment -D_GNU_SOURCE -ffunction-sections -fdata-sections \
		-I. -I$(MANUAL_GENERATED_DIR) -I$(ROOT)/src -I$(ROOT)/src/services \
		agent_reference_test.c $(ROOT)/src/services/solar_os_agent_reference.c \
		$(ROOT)/src/services/solar_os_manual.c -Wl,--gc-sections -o $@

manual_external_metadata_test: manual_external_metadata_test.c $(MANUAL_GENERATED_HEADER) $(ROOT)/src/services/solar_os_manual.c $(ROOT)/src/services/solar_os_manual.h $(ROOT)/src/services/solar_os_docs.h
	$(CC) $(CFLAGS) -Wno-comment -D_GNU_SOURCE -DSOLAR_OS_PACKAGE_SERVICE_DOCS=1 \
		-ffunction-sections -fdata-sections -I. -I$(MANUAL_GENERATED_DIR) \
		-I$(ROOT)/src -I$(ROOT)/src/services manual_external_metadata_test.c \
		$(ROOT)/src/services/solar_os_manual.c -Wl,--gc-sections -o $@

terminal_geometry_test: terminal_geometry_test.c $(ROOT)/src/services/solar_os_terminal_geometry.c $(ROOT)/src/services/solar_os_terminal_geometry.h
	$(CC) $(CFLAGS) -I. -I$(ROOT)/src -I$(ROOT)/src/services terminal_geometry_test.c \
		$(ROOT)/src/services/solar_os_terminal_geometry.c -o $@

app_exit_result_test: app_exit_result_test.c $(ROOT)/src/solar_os.c $(ROOT)/src/solar_os.h $(ROOT)/src/shell/solar_os_shell_io.c $(ROOT)/src/shell/solar_os_shell_io.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -ffunction-sections -fdata-sections \
		-include compat.h -I. -I$(ROOT)/src -I$(ROOT)/src/services \
		-I$(ROOT)/src/shell \
		-I$(ROOT)/components/u8g2/src/clib app_exit_result_test.c \
		$(ROOT)/src/solar_os.c $(ROOT)/src/shell/solar_os_shell_io.c \
		-Wl,--gc-sections -o $@

terminal_preferences_test: terminal_preferences_test.c $(ROOT)/src/services/solar_os_terminal_preferences.c $(ROOT)/src/services/solar_os_terminal_preferences.h
	$(CC) $(CFLAGS) -I. -I$(ROOT)/src -I$(ROOT)/src/services \
		terminal_preferences_test.c $(ROOT)/src/services/solar_os_terminal_preferences.c -o $@

tui_widgets_test: tui_widgets_test.c $(ROOT)/src/services/solar_os_tui_widgets.c $(ROOT)/src/services/solar_os_tui_widgets.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -ffunction-sections -fdata-sections -I. \
		-I$(ROOT)/src -I$(ROOT)/src/services -I$(ROOT)/src/shell \
		tui_widgets_test.c $(ROOT)/src/services/solar_os_tui_widgets.c \
		-Wl,--gc-sections -o $@

wireguard_crypto_test: wireguard_crypto_test.c $(ROOT)/components/wireguard_lwip/src/crypto.c $(ROOT)/components/wireguard_lwip/src/crypto/refc/x25519.c
	$(CC) $(CFLAGS) -I$(ROOT)/components/wireguard_lwip/src \
		wireguard_crypto_test.c $(ROOT)/components/wireguard_lwip/src/crypto.c \
		$(ROOT)/components/wireguard_lwip/src/crypto/refc/x25519.c -o $@

meshcore_channel_key_test: meshcore_channel_key_test.cpp \
		$(ROOT)/src/services/solar_os_meshcore_channel_key.cpp \
		$(ROOT)/src/services/solar_os_meshcore_channel_key.h \
		$(ROOT)/src/vendor/meshcore/crypto/SHA256.cpp \
		$(ROOT)/src/vendor/meshcore/crypto/SHA256.h \
		$(ROOT)/src/vendor/meshcore/crypto/Hash.cpp \
		$(ROOT)/src/vendor/meshcore/crypto/Hash.h \
		$(ROOT)/src/vendor/meshcore/crypto/Crypto.cpp
	$(CXX) $(CXXFLAGS) -std=c++17 -Wall -Wextra -Werror \
		-I$(ROOT)/src/services -I$(ROOT)/src/vendor/meshcore/crypto \
		meshcore_channel_key_test.cpp \
		$(ROOT)/src/services/solar_os_meshcore_channel_key.cpp \
		$(ROOT)/src/vendor/meshcore/crypto/SHA256.cpp \
		$(ROOT)/src/vendor/meshcore/crypto/Hash.cpp \
		$(ROOT)/src/vendor/meshcore/crypto/Crypto.cpp -o $@

meshcore_stream_codec_test: meshcore_stream_codec_test.c \
		$(ROOT)/src/services/solar_os_meshcore_stream_codec.c \
		$(ROOT)/src/services/solar_os_meshcore_stream_codec.h
	$(CC) $(CFLAGS) -I. -I$(ROOT)/src/services \
		meshcore_stream_codec_test.c \
		$(ROOT)/src/services/solar_os_meshcore_stream_codec.c -o $@

meshcore_ble_protocol_test: meshcore_ble_protocol_test.c \
		$(ROOT)/src/services/solar_os_meshcore_ble_protocol.c \
		$(ROOT)/src/services/solar_os_meshcore_ble_protocol.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/services \
		meshcore_ble_protocol_test.c \
		$(ROOT)/src/services/solar_os_meshcore_ble_protocol.c -o $@

link_repeater_test: link_repeater_test.c \
		$(ROOT)/src/services/solar_os_link_repeater.c \
		$(ROOT)/src/services/solar_os_link_repeater.h \
		$(ROOT)/src/services/solar_os_link.h esp_err.h
	$(CC) $(CFLAGS) -I. -I$(ROOT)/src/services \
		link_repeater_test.c \
		$(ROOT)/src/services/solar_os_link_repeater.c -o $@

time_timezone_test: time_timezone_test.c \
		$(ROOT)/src/services/solar_os_timezone.c \
		$(ROOT)/src/services/solar_os_timezone.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I$(ROOT)/src/services \
		time_timezone_test.c $(ROOT)/src/services/solar_os_timezone.c -o $@

rtc_service_test: rtc_service_test.c \
		$(ROOT)/src/services/solar_os_rtc.c \
		$(ROOT)/src/services/solar_os_rtc.h \
		$(ROOT)/src/services/solar_os_time.h esp_err.h freertos/FreeRTOS.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I. -I$(ROOT)/src/services \
		rtc_service_test.c $(ROOT)/src/services/solar_os_rtc.c -o $@

schedule_test: schedule_test.c \
		$(ROOT)/src/services/solar_os_schedule.c \
		$(ROOT)/src/services/solar_os_schedule.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h \
		-Ischedule_stubs -I. -I$(ROOT)/src -I$(ROOT)/src/services \
		schedule_test.c $(ROOT)/src/services/solar_os_schedule.c -o $@

rtc_pcf85063_test: rtc_pcf85063_test.c \
		$(ROOT)/src/drivers/rtc_pcf85063.c \
		$(ROOT)/src/drivers/rtc_pcf85063.h \
		rtc_stubs/solar_os_buses.h esp_err.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -Irtc_stubs -I. -I$(ROOT)/src/drivers \
		rtc_pcf85063_test.c $(ROOT)/src/drivers/rtc_pcf85063.c -o $@

test: $(TARGETS)
	./i2c_buses_test
	./cw2017_test
	./rtc_pcf8563_test
	./axp2101_service_test
	./messaging_service_test
	./ble_service_test
	./ble_multi_peer_test
	./ble_nimble_test
	./ble_hid_test
	./ble_lua_bindings_test
	./writer_buffer_test
	./writer_files_test
	./sketch_canvas_test
	./launcher_layout_test
	./display_layout_test
	./gfx_icons_test
	./text_search_test
	./doc_layout_test
	./shell_parse_test
	./shell_launch_test
	./shell_line_test
	./shell_completion_test
	./shell_diag_test
	./scheduler_test
	./schedule_test
	./script_lifecycle_test
	./app_file_types_test
	./gameboy_support_test
	./storage_mounts_test
	./json_scan_test
	./keys_test
	./input_test
	./unistroke_test
	./graffiti_test
	./board_caps_test
	./hid_keyboard_report_test
	./ble_keyboard_scan_policy_test
	./ps2_keyboard_test
	./ps2_mouse_test
	./cardkb_codec_test
	./inputronic_keyboard_test
	./tab5_keyboard_test
	./matrix_keyboard_test
	./input_keymap_service_test
	./cl32_keyboard_test
	./battery_service_test
	./camera_service_test
	./script_media_test
	./script_media_rtsp_test
	./raster_image_test
	./vision_test
	./imlib_test
	./imlib_lua_test
	./inference_test
	./espdl_workers_test
	./espdl_workers_single_test
	./tensor_image_test
	./tensor_image_lua_test
	./inference_lua_bindings_test
	./inference_schema_test
	./vision_lua_bindings_test
	./media_lua_bindings_test
	./camera_expansion_test
	./media_test
	./rtspd_job_test
	./rtspd_audio_only_test
	./rtsp_receiver_test
	./rtsp_auth_test
	./rtsp_client_test
	./gfx_mono_raster_test
	./rgb565_raster_test
	./rtspd_completion_test
	./mjpeg_test
	./http_server_test
	./sensors_service_test
	./midi_codec_test
	./midi_service_test
	./parameters_test
	./controls_test
	./osc_test
	./dsp_test
	./stream_test
	./resources_test
	./expansion_registry_test
	./audio_codec_test
	./webradio_catalog_test
	./gif_decode_stack_test
	./nvs_backup_test
	./agent_reference_test
	./manual_external_metadata_test
	./terminal_geometry_test
	./app_exit_result_test
	./terminal_preferences_test
	./tui_widgets_test
	./wireguard_crypto_test
	./meshcore_channel_key_test
	./meshcore_stream_codec_test
	./meshcore_ble_protocol_test
	./link_repeater_test
	./time_timezone_test
	./rtc_service_test
	./rtc_pcf85063_test
	./ubx_test
	./nmea_test
	./nmea_gnss_test
	./gnss_service_test
	./modem_service_test
	./haptic_service_test
	./charger_service_test
	./qmi8658_test
	./drv2605_test
	./bq25896_test
	./axp2101_test
	./epd_ssd1677_test
	./frontlight_pwm_test
	./epd_xteink_identify_test
	./x4pro_expansion_test
	./gt911_test
	./sdmmc_expansion_test
	./epd_uc8179_test
	./uc8179_expansion_test
	./epd_uc8279_test
	./uc8279_expansion_test
	./max17048_test
	./mgc3130_test
	./sim7670_test
	./st25r3916_test
	./nfc_service_test

clean:
	$(RM) $(TARGETS) model_bundle_test inference_lua_json_test zoo_test
	$(RM) -r generated
