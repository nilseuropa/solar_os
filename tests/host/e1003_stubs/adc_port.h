#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"
#define ADC_ATTEN_DB_12 12
#define ADC_BITWIDTH_12 12
typedef int adc_atten_t;
typedef int adc_bitwidth_t;
typedef struct { uint16_t voltage_mv; bool calibrated; } adc_port_sample_t;
esp_err_t adc_port_configure_pin(gpio_num_t pin, adc_atten_t atten, adc_bitwidth_t bits);
esp_err_t adc_port_read(gpio_num_t pin, adc_port_sample_t *sample);
