CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror

ROOT := ../..
WRITER_DIR := $(ROOT)/src/apps
TARGETS := writer_buffer_test writer_files_test doc_layout_test shell_parse_test shell_launch_test shell_line_test shell_completion_test shell_diag_test scheduler_test script_lifecycle_test app_file_types_test gameboy_support_test storage_mounts_test json_scan_test keys_test input_test board_caps_test hid_keyboard_report_test ble_keyboard_scan_policy_test ps2_keyboard_test cardkb_codec_test midi_codec_test midi_service_test parameters_test controls_test osc_test dsp_test stream_test audio_codec_test webradio_catalog_test nvs_backup_test agent_reference_test terminal_geometry_test terminal_preferences_test tui_widgets_test wireguard_crypto_test meshcore_channel_key_test time_timezone_test

MANUAL_GENERATED_DIR := generated/solar_os
MANUAL_GENERATED_HEADER := $(MANUAL_GENERATED_DIR)/solar_os_manual_data.h

.PHONY: all test clean

all: $(TARGETS)

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

shell_parse_test: shell_parse_test.c $(ROOT)/src/shell/solar_os_shell_parse.c $(ROOT)/src/shell/solar_os_shell_parse.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/shell \
		shell_parse_test.c $(ROOT)/src/shell/solar_os_shell_parse.c -o $@

shell_launch_test: shell_launch_test.c $(ROOT)/src/shell/solar_os_shell_launch.c $(ROOT)/src/shell/solar_os_shell_launch.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/shell \
		shell_launch_test.c $(ROOT)/src/shell/solar_os_shell_launch.c -o $@

shell_line_test: shell_line_test.c $(ROOT)/src/shell/solar_os_shell_line.c $(ROOT)/src/shell/solar_os_shell_line.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/shell \
		shell_line_test.c $(ROOT)/src/shell/solar_os_shell_line.c -o $@

shell_completion_test: shell_completion_test.c $(ROOT)/src/shell/solar_os_shell_completion.c $(ROOT)/src/shell/solar_os_shell_completion.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/shell \
		shell_completion_test.c $(ROOT)/src/shell/solar_os_shell_completion.c -o $@

shell_diag_test: shell_diag_test.c $(ROOT)/src/shell/solar_os_shell_common.c $(ROOT)/src/shell/solar_os_shell_common.h $(ROOT)/src/shell/solar_os_shell_io.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -ffunction-sections -fdata-sections \
		-include compat.h -I. -I$(ROOT)/src -I$(ROOT)/src/services \
		-I$(ROOT)/src/shell \
		shell_diag_test.c $(ROOT)/src/shell/solar_os_shell_common.c \
		-Wl,--gc-sections -o $@

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
		-Wl,--gc-sections -o $@

json_scan_test: json_scan_test.c $(ROOT)/src/services/solar_os_json_scan.c $(ROOT)/src/services/solar_os_json_scan.h esp_err.h
	$(CC) $(CFLAGS) -I. -I$(ROOT)/src/services \
		json_scan_test.c $(ROOT)/src/services/solar_os_json_scan.c -o $@

keys_test: keys_test.c $(ROOT)/src/services/solar_os_keys.c $(ROOT)/src/services/solar_os_keys.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/services \
		keys_test.c $(ROOT)/src/services/solar_os_keys.c -o $@

input_test: input_test.c $(ROOT)/src/services/solar_os_input.c $(ROOT)/src/services/solar_os_input.h $(ROOT)/src/services/solar_os_memory.h esp_timer.h nvs.h freertos/FreeRTOS.h
	$(CC) $(CFLAGS) -include compat.h -I. -I$(ROOT)/src -I$(ROOT)/src/services \
		input_test.c $(ROOT)/src/services/solar_os_input.c -o $@

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

cardkb_codec_test: cardkb_codec_test.c $(ROOT)/src/services/solar_os_cardkb_codec.c $(ROOT)/src/services/solar_os_cardkb_codec.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/services \
		cardkb_codec_test.c $(ROOT)/src/services/solar_os_cardkb_codec.c -o $@

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

audio_codec_test: audio_codec_test.c $(ROOT)/src/services/solar_os_audio_codec.c $(ROOT)/src/services/solar_os_audio_codec.h $(ROOT)/src/services/solar_os_audio_pcm.c $(ROOT)/src/services/solar_os_audio_pcm.h esp_err.h
	$(CC) $(CFLAGS) -DSOLAR_OS_AUDIO_CODEC_HOST_TEST -I. -I$(ROOT)/src/services -I$(ROOT)/components/minimp3/include \
		audio_codec_test.c $(ROOT)/src/services/solar_os_audio_codec.c \
		$(ROOT)/src/services/solar_os_audio_pcm.c \
		$(ROOT)/components/minimp3/minimp3_impl.c -o $@

webradio_catalog_test: webradio_catalog_test.c $(ROOT)/src/services/solar_os_webradio_catalog.c $(ROOT)/src/services/solar_os_webradio_catalog.h esp_attr.h esp_check.h esp_err.h nvs.h freertos/FreeRTOS.h freertos/portmacro.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -include compat.h -I. -I$(ROOT)/src -I$(ROOT)/src/services \
		webradio_catalog_test.c $(ROOT)/src/services/solar_os_webradio_catalog.c -o $@

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

terminal_geometry_test: terminal_geometry_test.c $(ROOT)/src/services/solar_os_terminal_geometry.c $(ROOT)/src/services/solar_os_terminal_geometry.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/services terminal_geometry_test.c \
		$(ROOT)/src/services/solar_os_terminal_geometry.c -o $@

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

time_timezone_test: time_timezone_test.c \
		$(ROOT)/src/services/solar_os_timezone.c \
		$(ROOT)/src/services/solar_os_timezone.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I$(ROOT)/src/services \
		time_timezone_test.c $(ROOT)/src/services/solar_os_timezone.c -o $@

test: $(TARGETS)
	./writer_buffer_test
	./writer_files_test
	./doc_layout_test
	./shell_parse_test
	./shell_launch_test
	./shell_line_test
	./shell_completion_test
	./shell_diag_test
	./scheduler_test
	./script_lifecycle_test
	./app_file_types_test
	./gameboy_support_test
	./storage_mounts_test
	./json_scan_test
	./keys_test
	./input_test
	./board_caps_test
	./hid_keyboard_report_test
	./ble_keyboard_scan_policy_test
	./ps2_keyboard_test
	./cardkb_codec_test
	./midi_codec_test
	./midi_service_test
	./parameters_test
	./controls_test
	./osc_test
	./dsp_test
	./stream_test
	./audio_codec_test
	./webradio_catalog_test
	./nvs_backup_test
	./agent_reference_test
	./terminal_geometry_test
	./terminal_preferences_test
	./tui_widgets_test
	./wireguard_crypto_test
	./meshcore_channel_key_test
	./time_timezone_test

clean:
	$(RM) $(TARGETS)
	$(RM) -r generated
