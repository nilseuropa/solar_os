/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Jeroen Domburg <jeroen@spritesmods.com> wrote this file. As long as you retain 
 * this notice you can do whatever you want with this stuff. If we meet some day, 
 * and you think this stuff is worth it, you can buy me a beer in return. 
 * ----------------------------------------------------------------------------
 */
#include <stdio.h>

/*
At the moment, this only emulates enough of the IWM to make the Mac boot; it basically always reports that
there's no floppy in the drive.
*/

#define IWM_CA0		(1<<0)
#define IWM_CA1		(1<<1)
#define IWM_CA2		(1<<2)
#define IWM_LSTRB	(1<<3)
#define IWM_ENABLE	(1<<4)
#define IWM_SELECT	(1<<5)
#define IWM_Q6		(1<<6)
#define IWM_Q7		(1<<7)

int iwmLines, iwmModeReg, iwmHeadSel;

void iwmInit(void) { iwmLines = iwmModeReg = iwmHeadSel = 0; }

void iwmAccess(unsigned int addr) {
	if (addr&1) {
		iwmLines|=(1<<(addr>>1));
	} else {
		iwmLines&=~(1<<(addr>>1));
	}
}

void iwmWrite(unsigned int addr, unsigned int val) {
	iwmAccess(addr);
	int reg=iwmLines&(IWM_Q7|IWM_Q6);
	if (reg==0xC0) iwmModeReg=val;
//	printf("IWM write %x (iwm reg %x) val %x\n", addr, reg, val);
}

void iwmSetHeadSel(int s) {
	iwmHeadSel=s;
}

unsigned int iwmRead(unsigned int addr) {
	unsigned int val=0;
	iwmAccess(addr);
	int reg=iwmLines&(IWM_Q7|IWM_Q6);
	if (reg==0) {
		//Data register
		val=0;
	} else if (reg==IWM_Q6) {
		//Status register
		int iwmSel=iwmLines&(IWM_CA0|IWM_CA1|IWM_CA2);
		if (iwmHeadSel) iwmSel|=IWM_SELECT; //Abusing this bit for the separate VIA-controlled SEL line. This is NOT the drive select bit!
		if (iwmSel==IWM_SELECT) val|=0x80; //Report: No disk in drive.
		if (iwmLines&IWM_ENABLE) val|=0x20; //Report: enabled
		val|=iwmModeReg&0x1F;
//		printf("Read disk status %x\n", iwmLines);
	} else if (reg==IWM_Q7) {
		//Read handshake register
		val=0xC0;
	} else {
		//Mode reg?
		if (iwmLines&IWM_ENABLE) val|=0x20; //enable
		val|=iwmModeReg&0x1F;
	}

//	printf("IWM read %x (iwm reg %x) val %x\n", addr, reg, val);
	return val;
}
