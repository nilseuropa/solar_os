#ifndef NCR_H
#define NCR_H
#include <stdint.h>

typedef struct {
	uint8_t cmd[256];
	uint8_t data[32*1024];
	uint8_t msg[128];
	int cmdlen;
	int datalen;
	int msglen;
} SCSITransferData;

typedef struct {
	int (*scsiCmd)(SCSITransferData *data, unsigned int cmd, unsigned int len, unsigned int lba, void *arg);
	void *arg;
} SCSIDevice;


void ncrInit();
void ncrRegisterDevice(int id, SCSIDevice* dev);
unsigned int ncrRead(unsigned int addr, unsigned int dack);
void ncrWrite(unsigned int addr,unsigned int dack, unsigned int val);

#endif