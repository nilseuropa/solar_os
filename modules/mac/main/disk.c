#define SOLAR_OS_MAC_PORT_IMPLEMENTATION
#include "port.h"
#include "ncr.h"

static uint32_t disk_handle, disk_size;
static bool disk_opened;
static uint8_t sense_key, sense_code;

static void put32(uint8_t *p, uint32_t n)
{
    p[0] = n >> 24; p[1] = n >> 16; p[2] = n >> 8; p[3] = n;
}
static int check_condition(SCSITransferData *data, uint8_t key, uint8_t code)
{
    sense_key = key; sense_code = code;
    data->cmd[0] = 2; data->msg[0] = 0;
    return 0;
}

static int scsi_command(SCSITransferData *data, unsigned cmd, unsigned len, unsigned lba, void *arg)
{
    (void)arg;
    data->msg[0] = 0;
    /* Capture allocation length before overwriting cmd[0] with status. */
    unsigned allocation = data->cmd[4];
    data->cmd[0] = 0;
    if (cmd == 0x08U || cmd == 0x28U || cmd == 0x0aU || cmd == 0x2aU) {
        if (!disk_opened) return check_condition(data, 2, 0x3a);
        /* Check before multiplying: READ/WRITE(6)'s zero length is 256
         * sectors; the controller's transfer buffer holds only 64. */
        if (len > sizeof(data->data) / 512U) return check_condition(data, 5, 0x24);
        if (lba > disk_size / 512U || len > disk_size / 512U - lba)
            return check_condition(data, 5, 0x21);
        if (!len) return 0;
        size_t bytes = len * 512U;
        int result = (cmd == 0x08U || cmd == 0x28U) ?
            solar_os_mac_app->disk_read(disk_handle, lba * 512U, data->data, bytes) :
            solar_os_mac_app->disk_write(disk_handle, lba * 512U, data->data, bytes);
        if (result) solar_os_mac_fail("disk read, write or sync failed");
        return (cmd == 0x08U || cmd == 0x28U) ? (int)bytes : 0;
    }
    if (cmd == 0x12U) { /* INQUIRY */
        static const uint8_t inquiry[36] = {
            0,0,1,1,31,0,0,0, 'A','P','P','L','E',' ',' ',' ',
            '2','0','S','C',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
            '1','.','0',' '
        };
        unsigned n = allocation < sizeof(inquiry) ? allocation : sizeof(inquiry);
        memcpy(data->data, inquiry, n); return (int)n;
    }
    if (cmd == 0x03U) { /* REQUEST SENSE */
        memset(data->data, 0, 18);
        data->data[0] = 0x70; data->data[2] = sense_key;
        data->data[7] = 10; data->data[12] = sense_code;
        sense_key = sense_code = 0;
        return allocation < 18U ? (int)allocation : 18;
    }
    if (cmd == 0x25U) { /* READ CAPACITY: last LBA, then sector size */
        if (!disk_opened) return check_condition(data, 2, 0x3a);
        put32(data->data, disk_size / 512U - 1U);
        put32(data->data + 4, 512); return 8;
    }
    if (cmd == 0x00U || cmd == 0x01U || cmd == 0x1bU || cmd == 0x1eU)
        return disk_opened ? 0 : check_condition(data, 2, 0x3a);
    return check_condition(data, 5, 0x20);
}
static SCSIDevice device = { .scsiCmd = scsi_command };

int solar_os_mac_disk_open(const char *path)
{
    disk_opened = false;
    sense_key = sense_code = 0;
    ncrRegisterDevice(6, NULL);
    if (!path) return 0;
    int result = solar_os_mac_app->disk_open(path, &disk_handle, &disk_size);
    if (result) return result;
    disk_opened = true;
    if (disk_size < 512U || disk_size % 512U) {
        (void)solar_os_mac_disk_close(); return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    }
    ncrRegisterDevice(6, &device);
    return 0;
}
int solar_os_mac_disk_close(void)
{
    if (!disk_opened) return 0;
    disk_opened = false;
    ncrRegisterDevice(6, NULL);
    return solar_os_mac_app->disk_close(disk_handle);
}
