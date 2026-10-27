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
#include <string.h>
#include <time.h>

typedef struct {
	int lastClkVal;
	int pos;
	uint16_t cmd;
	uint8_t mem[32];
} Rtc;

static Rtc rtc;

void rtcTick() {
	int x;
	for (x=0; x<4; x++) {
		rtc.mem[x]++;
		if (rtc.mem[x]!=0) break;
	}
	for (int i=0; i<4; i++) rtc.mem[i+4]=rtc.mem[i]; //undocumented; mac needs it tho'.
}

extern void saveRtcMem(char *mem);

int rtcCom(int en, int dat, int clk) {
	int ret=0;
	clk=clk?1:0;
	if (en) {
		rtc.pos=0;
		rtc.cmd=0;
	} else {
		if (clk!=rtc.lastClkVal && clk && rtc.pos < 16) {
			if (rtc.pos<8 || (rtc.pos<16 && ((rtc.cmd&0x8000)==0)) ) {
				//First 8 bits, or all 16 bits if write: accumulate data
				if (dat) rtc.cmd|=(1<<(15-rtc.pos));
			}
//			printf("RTC: clocktick %d, dataline %d, cmd %x\n", rtc.pos, dat, rtc.cmd);
			if (rtc.cmd&0x8000) { //read
				if (rtc.pos==8) {
					rtc.cmd|=rtc.mem[(rtc.cmd&0x7C00)>>10];
//					printf("RTC: Read cmd %x val %x\n", rtc.cmd>>8, (rtc.cmd&0xff));
				}
				ret=((rtc.cmd&(1<<(15-rtc.pos)))?1:0);
			} else if (rtc.pos==15) {
//				printf("RTC: Write cmd %x\n", rtc.cmd>>8);
				rtc.mem[(rtc.cmd&0x7C00)>>10]=rtc.cmd&0xff;
				saveRtcMem((char*)rtc.mem);
			}
			rtc.pos++;
		}
	}
	rtc.lastClkVal=clk;
	return ret;
}

void rtcInit(char *mem) {
	memset(&rtc, 0, sizeof(rtc));
    memcpy(rtc.mem, mem, 32);
}
