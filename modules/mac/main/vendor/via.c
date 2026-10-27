/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Jeroen Domburg <jeroen@spritesmods.com> wrote this file. As long as you retain 
 * this notice you can do whatever you want with this stuff. If we meet some day, 
 * and you think this stuff is worth it, you can buy me a beer in return. 
 * ----------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "via.h"
#include "m68k.h"

// --- Mac Plus keyboard (M0110A) via VIA Shift Register ---
#define KBD_QUEUE_SIZE 128
static uint8_t kbdQueue[KBD_QUEUE_SIZE];
static volatile int kbdQueueHead = 0;
static volatile int kbdQueueTail = 0;
static uint8_t kbdResponse = 0x7B; // null key (no key pressed)
static uint8_t kbdLastCmd = 0;

void kbdPushKey(uint8_t scancode, int isRelease) {
    uint8_t val = (scancode << 1) | 1;
    if (isRelease) val |= 0x80;
    int next = (kbdQueueHead + 1) % KBD_QUEUE_SIZE;
    if (next != kbdQueueTail) {
        kbdQueue[kbdQueueHead] = val;
        kbdQueueHead = next;
    }
}

void kbdClearQueue(void) {
    kbdQueueHead = kbdQueueTail = 0;
    kbdResponse = 0x7B;
}

static uint8_t kbdDequeue(void) {
    if (kbdQueueHead == kbdQueueTail) return 0x7B; // null
    uint8_t val = kbdQueue[kbdQueueTail];
    kbdQueueTail = (kbdQueueTail + 1) % KBD_QUEUE_SIZE;
    return val;
}

void viaCbPortAWrite(unsigned int val);
void viaCbPortBWrite(unsigned int val);
void viaIrq(int req);

#define IFR_IRQ		(1<<7)
#define IFR_T1		(1<<6)
#define IFR_T2		(1<<5)
#define IFR_CB1		(1<<4)
#define IFR_CB2		(1<<3)
#define IFR_SR		(1<<2)
#define IFR_CA1		(1<<1)
#define IFR_CA2		(1<<0)

#define PCR_NEG			0
#define PCR_NEG_NOCLR	1
#define PCR_POS			2
#define PCR_POS_NOCLR	3
#define PCR_HANDSHAKE	4
#define PCR_PULSEOUT	5
#define PCR_MAN_LO		6
#define PCR_MAN_HI		7


typedef struct {
	uint8_t ddra, ddrb;
	uint8_t ina, inb;
	uint8_t outa, outb;
	int timer1, timer2;
	uint16_t latch1, latch2;
	uint8_t ifr, ier;
	uint8_t pcr, acr;
	uint8_t controlin[4];
	int srTicks;
} Via;

static Via via;
static int timer1_armed, timer2_armed;

void viaInit(void) {
    memset(&via, 0, sizeof(via));
    timer1_armed = timer2_armed = 0;
    memset(kbdQueue, 0, sizeof(kbdQueue));
    kbdQueueHead = kbdQueueTail = 0;
    kbdResponse = 0x7B;
    kbdLastCmd = 0;
}

static const char* const viaRegNames[]={
	"ORB", "ORA", "DDRB", "DDRA", "T1C-L", "T1C-H", "T1L-L", "T1L-H", "T2L-L",
	"T2C-H", "SR", "ACR", "PCR", "IFR", "IER", "ORA-NC"
};


void viaSet(int no, int mask) {
	if (no==VIA_PORTA) via.ina|=mask; else via.inb|=mask;
}

void viaClear(int no, int mask) {
	if (no==VIA_PORTA) via.ina&=~mask; else via.inb&=~mask;
}


static void viaCheckIrq() {
	static int oldmint=0;
	int mint=(via.ifr&via.ier)&0x7F;
	if (mint) {
		via.ifr|=IFR_IRQ;
		viaIrq(1);
//		printf("VIA: Raised IRQ because masked if is %x\n", mint);
	} else if (!mint) {
		if (via.ifr&IFR_IRQ) viaIrq(0);
		via.ifr&=~IFR_IRQ;
	}
}

void viaStep(int clockcycles) {
    if (timer1_armed && clockcycles > via.timer1) {
        via.ifr |= IFR_T1;
        if (via.acr & 0x40) {
            int period = (int)via.latch1 + 2;
            via.timer1 = period - (clockcycles - via.timer1) % period;
        } else {
            timer1_armed = 0;
            via.timer1 = (via.timer1 - clockcycles) & 0xffff;
        }
    } else via.timer1 = (via.timer1 - clockcycles) & 0xffff;
    if (timer2_armed && clockcycles > via.timer2) {
        via.ifr |= IFR_T2;
        timer2_armed = 0;
    }
    via.timer2 = (via.timer2 - clockcycles) & 0xffff;
	if (via.srTicks) {
		via.srTicks-=clockcycles;
		if (via.srTicks<=0) {
			via.ifr|=IFR_SR;
			via.srTicks=0;
		}
	}

	static int p=0;
	p++;
	if (p==200) {
		p=0;
		via.ifr|=0x4;
	}

	viaCheckIrq();
}

static int pcrFor(int no) {
	const int shift[]={0, 1, 4, 5};
	const int mask[]={1, 3, 1, 3};
	int pcr=(via.pcr>>shift[no])&mask[no];
	if (no&1) {
		if (pcr) return PCR_NEG; else return PCR_POS;
	} else {
		return pcr;
	}
}

//Clears CB1/CA1 bits in IFR
static void accessPort(int no) {
	via.ifr&=~(no?IFR_CB1:IFR_CA1);
	int pcrca2=pcrFor(no?VIA_CB2:VIA_CA2);
//	if (pcrca2==PCR_NEG || pcrca2==PCR_POS) {
		via.ifr&=~(no?IFR_CB2:IFR_CA2);
//	}
	viaCheckIrq();
}

void viaControlWrite(int no, int val) {
	const int ifbits[]={IFR_CA1, IFR_CA2, IFR_CB1, IFR_CB2}; 
	int pcr=pcrFor(no);
	if (val) val=1;
	if (via.controlin[no]!=val) {
		if ( ((pcr==PCR_NEG || pcr==PCR_NEG_NOCLR) && !val) ||
			 ((pcr==PCR_POS || pcr==PCR_POS_NOCLR) && val) ) {
			via.ifr|=ifbits[no];
			viaCheckIrq();
		}
	}
	via.controlin[no]=val;
}

void viaWrite(unsigned int addr, unsigned int val) {
	unsigned int pc=m68k_get_reg(NULL, M68K_REG_PC);
	if (addr==0x0) {
		//ORB
		viaCbPortBWrite(val);
		via.inb=(via.inb&~via.ddrb)|(val&via.ddrb);
		accessPort(1);
	} else if (addr==0x1) {
		//ORA
		viaCbPortAWrite(val);
		via.ina=(via.ina&~via.ddra)|(val&via.ddra);
		accessPort(0);
	} else if (addr==0x2) {
		//DDRB
		via.ddrb=val;
	} else if (addr==0x3) {
		//DDRA
		via.ddra=val;
	} else if (addr==0x4) {
		//T1L-L
		via.latch1=(via.latch1&0xff00)|val;
	} else if (addr==0x5) {
		//T1C-H
		via.latch1=(via.latch1&0x00ff)|(val<<8);
		via.timer1=via.latch1;
		timer1_armed=1;
		via.ifr&=~IFR_T1;
		viaCheckIrq();
		viaCheckIrq();
	} else if (addr==0x6) {
		//T1L-L
		via.latch1=(via.latch1&0xff00)|val;
	} else if (addr==0x7) {
		//T1L-H
		via.latch1=(via.latch1&0x00ff)|(val<<8);
		via.ifr&=~IFR_T1;
		viaCheckIrq();
	} else if (addr==0x8) {
		//T2L-l
		via.latch2=val;
	} else if (addr==0x9) {
		//T2C-H
		via.timer2=via.latch2|(val<<8);
		timer2_armed=1;
		via.ifr&=~IFR_T2;
		viaCheckIrq();
		via.ifr&=~IFR_T2;
		viaCheckIrq();
	} else if (addr==0xa) {
		//SR — keyboard command from Mac
		kbdLastCmd = val;
		if (val == 0x10) {
			// Inquiry: return next key or null
			kbdResponse = kbdDequeue();
		} else if (val == 0x14) {
			// Model Number: M0110A = 0x0B
			kbdResponse = 0x0B;
		} else if (val == 0x36) {
			// Test: respond with 0x7D (ACK)
			kbdResponse = 0x7D;
		} else {
			kbdResponse = 0x7B; // null
		}
		via.srTicks=8;
	} else if (addr==0xb) {
		//ACR
		via.acr=val;
	} else if (addr==0xc) {
		//PCR
		via.pcr=val;
	} else if (addr==0xd) {
		//IFR
		via.ifr&=~val;
		viaCheckIrq();
	} else if (addr==0xe) {
		//IER
		if (val&0x80) {
			via.ier|=val;
		} else {
			via.ier&=~val;
		}
		via.ier&=0x7f;
		if (via.ier&0x18) {
			printf("Unsupported int enabled. ier=%x\n", via.ier);
			abort();
		}
		viaCheckIrq();
	} else if (addr==0xf) {
		//ORA
		viaCbPortAWrite(val);
		via.ina=(via.ina&~via.ddra)|(val&via.ddra);
	}
//	printf("PC %x VIA write %s val %x\n", pc, viaRegNames[addr], val);
}


unsigned int viaRead(unsigned int addr) {
	unsigned int pc=m68k_get_reg(NULL, M68K_REG_PC);
	unsigned int val=0;
	if (addr==0x0) {
		//ORB
		val=via.inb;
		accessPort(1);
	} else if (addr==0x1) {
		//ORA
		val=via.ina;
		accessPort(0);
	} else if (addr==0x2) {
		//DDRB
		val=via.ddrb;
	} else if (addr==0x3) {
		//DDRA
		val=via.ddra;
	} else if (addr==0x4) {
		//T1L-L
		val=via.timer1&0xff;
		via.ifr&=~IFR_T1;
		viaCheckIrq();
	} else if (addr==0x5) {
		//T1C-H
		val=via.timer1>>8;
	} else if (addr==0x6) {
		//T1L-L
		val=via.latch1&0xff;
	} else if (addr==0x7) {
		//T1L-H
		val=via.latch1>>8;
	} else if (addr==0x8) {
		//T2L-l
		val=via.timer2&0xff;
		via.ifr&=~IFR_T2;
		viaCheckIrq();
	} else if (addr==0x9) {
		//T2C-H
		val=via.timer2>>8;
	} else if (addr==0xa) {
		//SR — keyboard response
		via.ifr&=~IFR_SR;
		val=kbdResponse;
	} else if (addr==0xb) {
		//ACR
		val=via.acr;
	} else if (addr==0xc) {
		//PCR
		val=via.pcr;
	} else if (addr==0xd) {
		//IFR
		val=via.ifr;
	} else if (addr==0xe) {
		//IER
		val=via.ier|0x80;
	} else if (addr==0xf) {
		//ORA
		val=via.ina;
	}
//	printf("PC %x VIA read %s val %x\n", pc, viaRegNames[addr], val);
	return val;
}
