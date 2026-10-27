/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Jeroen Domburg <jeroen@spritesmods.com> wrote this file. As long as you retain 
 * this notice you can do whatever you want with this stuff. If we meet some day, 
 * and you think this stuff is worth it, you can buy me a beer in return. 
 * ----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <stdio.h>
#include "ncr.h"
#include "m68k.h"
#include <string.h>

static const char* const regNamesR[]={
	"CURSCSIDATA","INITIATORCMD", "MODE", "TARGETCMD", "CURSCSISTATUS",
	"BUSANDSTATUS", "INPUTDATA", "RESETPARINT"
};

static const char* const regNamesW[]={
	"OUTDATA","INITIATORCMD", "MODE", "TARGETCMD", "SELECTENA",
	"STARTDMASEND", "STARTDMATARRECV", "STARTDMAINITRECV"
};


typedef struct {
	SCSIDevice *dev[8];
	uint8_t mode;
	uint8_t tcr;
	uint8_t dout;
	uint8_t din;
	uint8_t inicmd;
	int selected;
	int state;
	uint8_t tcrforbuf;
	SCSITransferData data;
	uint8_t *buf;
	int bufmax;
	int bufpos;
	int datalen;
} Ncr;

#define INIR_AIP (1<<6)
#define INIR_LA (1<<5)
#define INI_RST (1<<7)
#define INI_ACK (1<<4)
#define INI_BSY (1<<3)
#define INI_SEL (1<<2)
#define INI_ATN (1<<1)
#define INI_DBUS (1<<0)

#define SSR_RST (1<<7)
#define SSR_BSY (1<<6)
#define SSR_REQ (1<<5)
#define SSR_MSG (1<<4)
#define SSR_CD (1<<3)
#define SSR_IO (1<<2)
#define SSR_SEL (1<<1)
#define SSR_DBP (1<<0)

#define TCR_IO (1<<0)
#define TCR_CD (1<<1)
#define TCR_MSG (1<<2)
#define TCR_REQ (1<<3)

#define MODE_ARB (1<<0)
#define MODE_DMA (1<<1)
#define MODE_MONBSY (1<<2)
#define MODE_EIPINTEN (1<<3)
#define MODE_PARINTEN (1<<4)
#define MODE_PARCHK (1<<5)
#define MODE_TARGET (1<<6)
#define MODE_BDMA (1<<7)

#define BSR_ACK (1<<0)
#define BSR_ATN (1<<1)
#define BSR_BUSYERR (1<<2)
#define BSR_PHASEMATCH (1<<3)
#define BSR_IRQACT (1<<4)
#define BSR_PARERR (1<<5)
#define BSR_DMARQ (1<<6)
#define BSR_EODMA (1<<7)

#define ST_IDLE 0
#define ST_ARB 1
#define ST_ARBDONE 2
#define ST_SELECT 3
#define ST_SELDONE 4
#define ST_DATA 5

static const char* const stateNames[]={
	"IDLE", "ARB", "ARBDONE", "SELECT", "SELDONE", "DATA"
};

static Ncr ncr;

void ncrInit(void) {
    memset(&ncr, 0, sizeof(ncr));
    ncr.buf = ncr.data.cmd;
    ncr.bufmax = sizeof(ncr.data.cmd);
}

static void parseScsiCmd(int isRead) {
	uint8_t *buf=ncr.data.cmd;
	int cmd=buf[0];
	unsigned int lba, len, ctrl;
	int group=(cmd>>5);
	if (group==0) { //6-byte command
		lba=buf[3]|(buf[2]<<8)|((buf[1]&0x1F)<<16);
		len=buf[4];
		if (len==0) len=256;
		ctrl=buf[5];
//		for (int x=0; x<6; x++) printf("%02X ", buf[x]);
//		printf("\n");
	} else if (group==1 || group==2) { //10-byte command
		lba=buf[5]|(buf[4]<<8)|(buf[3]<<16)|((unsigned int)buf[2]<<24);
		len=buf[8]|(buf[7]<<8);
		ctrl=buf[9];
//		for (int x=0; x<10; x++) printf("%02X ", buf[x]);
//		printf("\n");
	} else {
		printf("SCSI: UNSUPPORTED CMD %x\n", cmd);
		return;
	}
//	printf("SCSI: CMD %x LBA %x LEN %x CTRL %x %s\n", cmd, lba, len, ctrl, isRead?"*READ*":"*WRITE*");
	if (ncr.dev[ncr.selected]) {
		ncr.datalen=ncr.dev[ncr.selected]->scsiCmd(&ncr.data, cmd, len, lba, ncr.dev[ncr.selected]->arg);
        if (ncr.datalen < 0 || ncr.datalen > (int)sizeof(ncr.data.data))
            solar_os_mac_fail("invalid SCSI transfer size");
	}
}

unsigned int ncrRead(unsigned int addr, unsigned int dack) {
	unsigned int pc=m68k_get_reg(NULL, M68K_REG_PC);
	unsigned int ret=0;
	if (ncr.mode&MODE_DMA && dack) {
		if (ncr.tcr&TCR_IO) {
			if (ncr.bufpos<ncr.bufmax) ncr.din=ncr.buf[ncr.bufpos++];
//			printf("Send next byte dma %d/%d\n", ncr.bufpos, ncr.datalen);
		}
	}
	if (addr==0) {
		ret=ncr.din;
//		printf("READ BYTE %02X dack=%d\n", ret, dack);
	} else if (addr==1) {
		// /rst s s /ack /bsy /sel /atn databus
		ret=ncr.inicmd;
		if (ncr.state==ST_ARB) {
			ret|=INIR_AIP;
			//We don't have a timer... just set arb to be done right now.
			if (ncr.dev[ncr.selected]) ncr.state=ST_ARBDONE;
		}
	} else if (addr==2) {
		ret=ncr.mode;
	} else if (addr==3) {
		ret=ncr.tcr;
	} else if (addr==4) {
		ret=0;
		if (ncr.inicmd&INI_RST) ret|=SSR_RST;
		if (ncr.inicmd&INI_BSY) ret|=SSR_BSY;
//		if (ncr.inicmd&INI_SEL) ret|=SSR_SEL;
		if (ncr.tcr&TCR_IO) ret|=SSR_IO;
		if (ncr.tcr&TCR_CD) ret|=SSR_CD;
		if (ncr.tcr&TCR_MSG) ret|=SSR_MSG;
		if (ncr.dev[ncr.selected] && (ncr.state==ST_SELDONE)) {
//			ret|=SSR_REQ;
			ret|=SSR_BSY;
		}
		if (ncr.state==ST_DATA) {
			if ((ncr.inicmd&INI_ACK)==0) {
				ret|=SSR_REQ;
			}
		}
		if (ncr.state==ST_ARB) return 0x40;
	} else if (addr==5) {
		ret=BSR_PHASEMATCH;
		if (ncr.mode&MODE_DMA) {
			ret|=BSR_DMARQ;
			if (ncr.bufpos>=ncr.datalen) {
//				printf("End of DMA reached: bufpos %d datalen %d\n", ncr.bufpos, ncr.datalen);
				ret|=BSR_EODMA;
			}
		}
	} else if (addr==6) {
		ret=ncr.din;
//		printf("READ BYTE (NCR addr6) %02X dack=%d\n", ret, dack);
	} else if (addr==7) {
		printf("Scsi: !UNIMPLEMENTED! (addr 7)\n");
	}
//	printf("%08X SCSI: (dack %d), cur st %s read %s (reg %d) = %x \n", 
//		pc, dack,  stateNames[ncr.state], regNamesR[addr], addr, ret);
	return ret;
}


void ncrWrite(unsigned int addr, unsigned int dack, unsigned int val) {
	unsigned int pc=m68k_get_reg(NULL, M68K_REG_PC);

	if (addr==0) {
		if (ncr.mode&MODE_DMA && dack) {
			if (ncr.bufpos<ncr.bufmax) ncr.buf[ncr.bufpos]=val;
			if ((ncr.tcr&TCR_IO)==0) {
				if (ncr.bufpos<ncr.bufmax) ncr.bufpos++;
			}
		}
		ncr.dout=val;
		ncr.din=val;
	} else if (addr==1) {
		if ((val&INI_SEL) && (val&INI_DBUS) && (val&INI_BSY) && (ncr.state==ST_ARBDONE || ncr.state==ST_ARB)) {
			ncr.state=ST_SELECT;
			if (ncr.dout==0x81) ncr.selected=0;
			if (ncr.dout==0x82) ncr.selected=1;
			if (ncr.dout==0x84) ncr.selected=2;
			if (ncr.dout==0x88) ncr.selected=3;
			if (ncr.dout==0x90) ncr.selected=4;
			if (ncr.dout==0xA0) ncr.selected=5;
			if (ncr.dout==0xC0) ncr.selected=6;
//			printf("Selected dev: %d (val %x)\n", ncr.selected, ncr.dout);
		}
		if (((val&INI_BSY)==0) && ncr.state==ST_SELECT) {
			ncr.state=ST_SELDONE;
		}
		if (((val&INI_SEL)==0) && ncr.state==ST_SELDONE) {
			if (ncr.dev[ncr.selected]) {
				ncr.state=ST_DATA;
			} else {
				ncr.state=ST_IDLE;
			}
		}
		if (ncr.state==ST_DATA && ((ncr.inicmd&INI_ACK)==0) && (val&INI_ACK)) {
			//We have an ack.
			if (!(ncr.tcr&TCR_IO)) {
				if (ncr.bufpos<ncr.bufmax) ncr.buf[ncr.bufpos++]=ncr.dout;
			}
		}
		if (ncr.state==ST_DATA && (ncr.inicmd&INI_ACK) && ((val&INI_ACK)==0)) {
			//Ack line goes low..
			if (ncr.tcr&TCR_IO) {
				if (ncr.bufpos<ncr.bufmax) ncr.din=ncr.buf[ncr.bufpos++];
//				printf("Send byte non-dma\n");
			}
		}
		if (val&INI_RST) {
			ncr.state=ST_IDLE;
		}
		ncr.inicmd&=~0x9F;
		ncr.inicmd|=val&0x9f;
	} else if (addr==2) {
		ncr.mode=val;
		if (((val&1)==0) && ncr.state==ST_ARB) ncr.state=ST_IDLE;
		if (val&1) ncr.state=ST_ARB;
	} else if (addr==3) {
		if (ncr.tcr!=(val&0xf)) {
			int oldtcr=(ncr.tcr&7);
			int newtcr=(val&7);
			if (oldtcr==0 && ncr.bufpos) {
				//End of data out phase
				parseScsiCmd(0);
			} else if ((oldtcr==TCR_CD) && (newtcr==TCR_IO)) {
				//Start of data in phase
				parseScsiCmd(1);
			}
			if ((ncr.tcr&0x7)==TCR_IO) {
//				printf("Data Out finished: Host read %d/%d bytes.\n", ncr.bufpos, ncr.datalen);
			}
			ncr.bufpos=0;
			int type=val&(TCR_MSG|TCR_CD);
			if (type==0) {
//				printf("Sel data buf %s.\n", (newtcr&TCR_IO)?"IN":"OUT");
				ncr.buf=ncr.data.data;
				ncr.bufmax=sizeof(ncr.data.data);
			} else if (type==TCR_CD) {
//				printf("Sel cmd/status buf %s.\n", (newtcr&TCR_IO)?"IN":"OUT");
				ncr.buf=ncr.data.cmd;
				ncr.bufmax=sizeof(ncr.data.cmd);
				ncr.datalen=1;
			} else if (type==(TCR_CD|TCR_MSG)) {
//				printf("Sel msg buf %s.\n", (newtcr&TCR_IO)?"IN":"OUT");
				ncr.buf=ncr.data.msg;
				ncr.bufmax=sizeof(ncr.data.msg);
				ncr.datalen=1;
			}
			ncr.din=ncr.buf[0];
		}
		ncr.tcr=val&0xf;
	} else if (addr==4) {
		if (val!=0) printf("!UNIMPLEMENTED! selenable (val %x), todo\n", val);
	} else if (addr==5) {
		printf("!UNIMPLEMENTED!\n");
	} else if (addr==6) {
		printf("!UNIMPLEMENTED!\n");
	} else if (addr==7) {
		//Start DMA. We already do this using the mode bit.
	}
//	printf("%08X SCSI: (dack %d), cur state %s %02x to %s (reg %d)\n", pc, dack, stateNames[ncr.state], val, regNamesW[addr], addr);
}

void ncrRegisterDevice(int id, SCSIDevice* dev){
	if (id >= 0 && id < 8) ncr.dev[id]=dev;
}
