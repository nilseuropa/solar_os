#include "mpu6050.h"
#include <string.h>

/* TDK InvenSense RM-MPU-6000A-00 rev 4.2. */
#define REG_POWER 0x6bU
#define REG_INT_ENABLE 0x38U

static esp_err_t write_u8(mpu6050_t *d, uint8_t reg, uint8_t value)
{ return d->io.write(d->io.ctx, reg, &value, 1); }

esp_err_t mpu6050_init(mpu6050_t *d, const mpu6050_io_t *io)
{
    if (!d || !io || !io->read || !io->write || !io->delay_ms) return ESP_ERR_INVALID_ARG;
    memset(d, 0, sizeof(*d)); d->io = *io;
    uint8_t identity;
    esp_err_t e = io->read(io->ctx, 0x75, &identity, 1);
    if (e != ESP_OK) return e;
    if (identity != 0x68) return ESP_ERR_NOT_FOUND;
    e = write_u8(d, REG_POWER, 0x80); /* Device reset. */
    if (e == ESP_OK) {
        io->delay_ms(io->ctx, 100);
        e = io->read(io->ctx, REG_POWER, &identity, 1);
        if (e == ESP_OK && (identity & 0x80)) e = ESP_ERR_TIMEOUT;
    }
    const uint8_t settings[][2] = {
        {REG_POWER, 0x01}, /* Wake; X gyro PLL clock. */
        {0x6c, 0x00},      /* All six axes enabled. */
        {0x1a, 0x03}, {0x19, 9}, {0x1b, 0}, {0x1c, 0},
        {0x37, 0x20},      /* Latched, active-high push-pull INT. */
        /* DATA_RDY_EN also enables INT_STATUS for the polling path. */
        {REG_INT_ENABLE, 0x01},
    };
    for (unsigned i = 0; e == ESP_OK && i < sizeof(settings)/sizeof(settings[0]); ++i)
        e = write_u8(d, settings[i][0], settings[i][1]);
    if (e == ESP_OK) {
        io->delay_ms(io->ctx, 100); /* Gyro startup and filter settling. */
        d->initialized = true;
    } else {
        (void)write_u8(d, REG_INT_ENABLE, 0);
        (void)write_u8(d, REG_POWER, 0x40);
    }
    return e;
}

esp_err_t mpu6050_deinit(mpu6050_t *d)
{
    if (!d || !d->initialized) return ESP_ERR_INVALID_STATE;
    esp_err_t e = write_u8(d, REG_INT_ENABLE, 0);
    if (e == ESP_OK) e = write_u8(d, REG_POWER, 0x40);
    if (e == ESP_OK) memset(d, 0, sizeof(*d));
    return e;
}

esp_err_t mpu6050_data_ready(mpu6050_t *d, bool *ready)
{
    if (!d || !d->initialized || !ready) return ESP_ERR_INVALID_ARG;
    uint8_t status;
    esp_err_t e = d->io.read(d->io.ctx, 0x3a, &status, 1);
    if (e == ESP_OK) *ready = (status & 1) != 0;
    return e;
}

static int16_t be16(const uint8_t *p)
{ return (int16_t)(((uint16_t)p[0] << 8) | p[1]); }

esp_err_t mpu6050_read_sample(mpu6050_t *d, mpu6050_sample_t *sample)
{
    if (!d || !d->initialized || !sample) return ESP_ERR_INVALID_ARG;
    /* One burst gives a coherent accel/temp/gyro snapshot (register map 4.17).
     * Temperature is skipped; this six-axis device supplies no orientation. */
    uint8_t raw[14];
    esp_err_t e = d->io.read(d->io.ctx, 0x3b, raw, sizeof(raw));
    if (e != ESP_OK) return e;
    for (unsigned i = 0; i < 3; ++i) {
        sample->acceleration_m_s2[i] = be16(raw + 2*i) * (9.80665F / 16384.0F);
        sample->angular_velocity_rad_s[i] = be16(raw + 8 + 2*i) * (0.017453292519943295F / 131.0F);
    }
    return ESP_OK;
}
