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
#include "scc.h"
#include "m68k.h"
#include "hexdump.h"
#include "network/localtalk.h"
#include <string.h>
#include <stdlib.h>
#include <assert.h>
/*
Emulation of the Zilog 8530 SCC.

Supports basic mouse pins plus hacked in LocalTalk
*/

//#define SCC_DBG


/* Serial/LocalTalk is disconnected. Guest initialization may still program
 * modes we do not emulate; leave those modes inert. Bounds remain checked. */
#define exit_when_strict(...) ((void)0)

void sccIrq(int ena);

#define BUFLEN 8192
#define NO_RXBUF 4

typedef struct {
	int delay; //-1 if buffer is free
	int len;
	uint8_t data[BUFLEN];
} RxBuf;

typedef struct {
	int dcd;
	int cts;
	int wr1, wr15;
	int sdlcaddr;
	int hunting;
	int txTimer;
	uint8_t txData[BUFLEN];
	RxBuf rx[NO_RXBUF];
	int txPos;
	int rxPos;
	int rxBufCur;
	int rxDelay;
	int eofDelay;
	int eofIntPending;
	int rr0Latched;
	int rr0Prev;
	int rxAbrtTimer;
	int rxEom;
	int intOnNextRxChar;
} SccChan;

typedef struct {
	int guard;
	unsigned int regptr;
	int intpending;
	int intpendingOld;
	SccChan chan[2];
	int wr2, wr9;
} Scc;

static Scc scc;


/*
For SCC, Special conditions are:
- Receive overrun
- Framing Error (async)
- End of Frame
- Optional: Parity error

For the emu (where we atm only do sldc for appletalk) only End Of Frame is important.
*/

static void triggerRx(int chan);


static int rxHasByte(int chan) {
	return (scc.chan[chan].rx[scc.chan[chan].rxBufCur].delay==0)?1:0;
}


static void rxBufIgnoreRest(int chan) {
	if (scc.chan[chan].rxPos==0) return; //already at new buff
	scc.chan[chan].rx[scc.chan[chan].rxBufCur].delay=-1;
	scc.chan[chan].rxBufCur++;
#ifdef SCC_DBG
	printf("RxBuff: Skipping to next buff %d, which has delay %d.\n",
			scc.chan[chan].rxBufCur, scc.chan[chan].rx[scc.chan[chan].rxBufCur].delay);
#endif
	if (scc.chan[chan].rxBufCur>=NO_RXBUF) scc.chan[chan].rxBufCur=0;
	scc.chan[chan].rxPos=0;
}

static int rxByte(int chan, int *bytesLeftInBuf) {
	int ret;
	int curbuf=scc.chan[chan].rxBufCur;
#ifdef SCC_DBG
	printf("RxBuf: bufid %d byte %d/%d\n", curbuf, scc.chan[chan].rxPos, scc.chan[chan].rx[curbuf].len);
#endif
	if (scc.chan[chan].rx[curbuf].delay!=0) return 0;
	if (bytesLeftInBuf) *bytesLeftInBuf=scc.chan[chan].rx[curbuf].len-scc.chan[chan].rxPos-1;
	ret=scc.chan[chan].rx[curbuf].data[scc.chan[chan].rxPos++];
	if (scc.chan[chan].rxPos==scc.chan[chan].rx[curbuf].len) {
		rxBufIgnoreRest(chan);
	}
	return ret;
}

static int rxBytesLeft(int chan) {
	if (scc.chan[chan].rx[scc.chan[chan].rxBufCur].delay!=0) return 0;
	return scc.chan[chan].rx[scc.chan[chan].rxBufCur].len-scc.chan[chan].rxPos;
}

static int rxBufTick(int chan, int noTicks) {
	if (scc.chan[chan].rx[scc.chan[chan].rxBufCur].delay > 0) {
		scc.chan[chan].rx[scc.chan[chan].rxBufCur].delay-=noTicks;
		if (scc.chan[chan].rx[scc.chan[chan].rxBufCur].delay<=0) {
			scc.chan[chan].rx[scc.chan[chan].rxBufCur].delay=0;
#ifdef SCC_DBG
			printf("Feeding buffer %d into SCC\n", scc.chan[chan].rxBufCur);
#endif
			return 1;
		}
	}
	return 0;
}


//WR15 is the External/Status Interrupt Control and has the interrupt enable bits.
#define SCC_WR15_BREAK  (1<<7)
#define SCC_WR15_TXU    (1<<6)
#define SCC_WR15_CTS    (1<<5)
#define SCC_WR15_SYNC   (1<<4)
#define SCC_WR15_DCD    (1<<3)
#define SCC_WR15_ZCOUNT (1<<1)

//RR3, when read, gives the Interrupt Pending status.
//This is reflected in scc.intpending.
#define SCC_RR3_CHB_EXT (1<<0)
#define SCC_RR3_CHB_TX  (1<<1)
#define SCC_RR3_CHB_RX  (1<<2)
#define SCC_RR3_CHA_EXT (1<<3)
#define SCC_RR3_CHA_TX  (1<<4)
#define SCC_RR3_CHA_RX  (1<<5)

#define SCC_R0_RX (1<<0)
#define SCC_R0_ZEROCOUNT (1<<1)
#define SCC_R0_TXE (1<<2)
#define SCC_R0_DCD (1<<3)
#define SCC_R0_SYNCHUNT (1<<4)
#define SCC_R0_CTS (1<<5)
#define SCC_R0_EOM (1<<6)
#define SCC_R0_BREAKABRT (1<<7)

static void explainWrite(int reg, int chan, int val) {
	const static char *cmdLo[]={"null", "point_high", "reset_ext_status_int", "send_ABORT", 
			"ena_int_on_next_char", "reset_tx_pending", "error_reset", "reset_highest_ius"};
	const static char *cmdHi[]={"null", "reset_rx_crc", "reset_tx_crc", "reset_tx_underrun_EOM_latch"};
	const static char *intEna[]={"RxIntDisabled", "RxInt1stCharOrSpecial", "RxIntAllCharOrSpecial", "RxIntSpecial"};
	const static char *rstDesc[]={"NoReset", "ResetChB", "ResetChA", "HwReset"};
	if (reg==0) {
		if (((val&0xF8)!=0) && ((val&0xF8)!=0x08)) {
			printf("Write reg 0; CmdHi=%s CmdLo=%s\n", cmdHi[(val>>6)&3], cmdLo[(val>>3)&7]);
		}
	} else if (reg==1) {
		printf("Write reg 1 for chan %d: ", chan);
		if (val&0x80) printf("WaitDmaReqEn ");
		if (val&0x40) printf("WaitDmaReqFn ");
		if (val&0x20) printf("WaitDmaOnRxTx ");
		if (val&0x04) printf("ParityIsSpecial ");
		if (val&0x02) printf("TxIntEna ");
		if (val&0x01) printf("RxIntEna ");
		printf("%s\n", intEna[(val>>3)&3]);
	} else if (reg==9) {
		printf("Write reg 9: cmd=%s ", rstDesc[(val>>6)&3]);
		if (val&0x01) printf("VIS ");
		if (val&0x02) printf("NV ");
		if (val&0x04) printf("DLC ");
		if (val&0x08) printf("MIE ");
		if (val&0x10) printf("StatusHi ");
		if (val&0x20) printf("RESVD ");
		printf("\n");
	} else if (reg==15) {
		printf("Write reg 15: ");
		if (val&0x02) printf("ZeroCountIE ");
		if (val&0x08) printf("DcdIE ");
		if (val&0x10) printf("SyncHuntIE ");
		if (val&0x20) printf("CtsIE ");
		if (val&0x40) printf("TxUnderrunIE ");
		if (val&0x80) printf("BreakAbortIE ");
		printf("\n");
	} else {
		printf("Write chan %d reg %d val 0x%02X\n", chan, reg, val);
	}
}

void explainRead(int reg, int chan, int val) {
	const static char *intRsn[]={
			"ChBTxBufEmpty", "ChBExtOrStatusChange", "ChBRecvCharAvail", "ChBSpecRecvCond",
			"ChATxBufEmpty", "ChAExtOrStatusChange", "ChARecvCharAvail", "ChASpecRecvCond",
		};
	if (reg==0) {
		printf("Read chan %d reg 0:", chan);
		if (val&0x01) printf("RxCharAvailable ");
		if (val&0x02) printf("ZeroCount ");
		if (val&0x04) printf("TxBufEmpty ");
		if (val&0x08) printf("DCD ");
		if (val&0x10) printf("SyncHunt ");
		if (val&0x20) printf("CTS ");
		if (val&0x40) printf("TxUnderrunEOM ");
		if (val&0x80) printf("BreakAbort ");
		printf("\n");
	} else if (reg==2) {
		printf("Read reg 2: ");
		printf("IntRsn=%s\n", intRsn[(val>>1)&7]);
	} else {
		printf("Read chan %d reg %d val 0x%X\n", chan, reg, val);
	}
}

//Raises an interrupt if needed, specifically when intpending indicates so. Need to 
//change intpending beforehand.
static void raiseInt(int chan) {
    (void)chan;
    unsigned enabled = 0;
    for (unsigned i = 0; i < 2; ++i) {
        unsigned bits = ((scc.chan[i].wr1 & 1) ? 1U : 0U) |
            ((scc.chan[i].wr1 & 2) ? 2U : 0U) |
            ((scc.chan[i].wr1 & 0x18) ? 4U : 0U);
        enabled |= bits << (i == SCC_CHANA ? 3U : 0U);
    }
    sccIrq((scc.wr9 & 8) && (scc.intpending & enabled));
}


static int calcRr0(int chan) {
	int val=0;
	if (rxHasByte(chan)) val|=SCC_R0_RX;
	//Bit 1 is zero count - never set
	val|=SCC_R0_TXE; //tx buffer always empty
	val|=SCC_R0_CTS;
	if (scc.chan[chan].dcd) val|=SCC_R0_DCD;
	if (scc.chan[chan].hunting) val|=SCC_R0_SYNCHUNT;
	if (scc.chan[chan].cts) val|=SCC_R0_CTS;
	if (scc.chan[chan].txTimer==0) val|=SCC_R0_EOM;
	if (scc.chan[chan].rxAbrtTimer>0 && scc.chan[chan].rxAbrtTimer<20)  val|=SCC_R0_BREAKABRT; //abort
	//if (rxBytesLeft(chan)==1) val|=SCC_R0_BREAKABRT; //abort
	//if (scc.chan[chan].rxEom) val|=SCC_R0_BREAKABRT; //abort
	return val;
}

static void checkExtInt(int chan) {
	int rr0=calcRr0(chan);
	int dif=(rr0^scc.chan[chan].rr0Prev);
	int wr15=scc.chan[chan].wr15;
	int triggered=0;
	if ((dif&SCC_R0_BREAKABRT) && (wr15&SCC_WR15_BREAK)) triggered=1;
	if ((dif&SCC_R0_CTS) && (wr15&SCC_WR15_CTS)) triggered=1;
	if ((dif&SCC_R0_DCD) && (wr15&SCC_WR15_DCD)) triggered=1;
	if ((dif&SCC_R0_SYNCHUNT) && (wr15&SCC_WR15_SYNC)) triggered=1;
	if ((dif&SCC_R0_EOM) && (wr15&SCC_WR15_TXU)) triggered=1;
	if (triggered) {
		if (chan==0 && ((scc.intpending&SCC_RR3_CHA_EXT)==0)) {
			scc.chan[chan].rr0Latched=rr0;
			scc.intpending|=SCC_RR3_CHA_EXT;
			raiseInt(chan);
		}
		if (chan==1 && ((scc.intpending&SCC_RR3_CHB_EXT)==0)) {
			scc.chan[chan].rr0Latched=rr0;
			scc.intpending|=SCC_RR3_CHB_EXT;
			raiseInt(chan);
		}
	}
	scc.chan[chan].rr0Prev=rr0;
}


void sccSetDcd(int chan, int val) {
	val=val?1:0;
	scc.chan[chan].dcd=val;
	checkExtInt(chan);
}

void sccRecv(int chan, uint8_t *data, int len, int delay) {
    if (chan < 0 || chan > 1 || len < 0 || len > BUFLEN - 2) solar_os_mac_fail("SCC receive overflow");
	int bufid=scc.chan[chan].rxBufCur;
	int n=0;
	do {
		if (scc.chan[chan].rx[bufid].delay==-1) break;
		bufid++;
		if (bufid>=NO_RXBUF) bufid=0;
		n++;
	} while(bufid!=scc.chan[chan].rxBufCur);

	//check if all buffers are full
	if (scc.chan[chan].rx[bufid].delay!=-1) {
		printf("Eek! Can't queue buffer: full!\n");
		return;
	}

	printf("Serial transmit for chan %d queued; bufidx %d, len=%d delay=%d, %d other buffers in queue\n", chan, bufid, len, delay, n);
	memcpy(scc.chan[chan].rx[bufid].data, data, len);
	scc.chan[chan].rx[bufid].data[len]=0xA5; //crc1
	scc.chan[chan].rx[bufid].data[len+1]=0xA5; //crc2
//	scc.chan[chan].rx[bufid].data[len+2]=0; //end flag
//	scc.chan[chan].rx[bufid].data[len+3]=0; //dummy
	scc.chan[chan].rx[bufid].len=len+2;
	scc.chan[chan].rx[bufid].delay=delay;
}

void sccTxFinished(int chan) {
	hexdump(scc.chan[chan].txData, scc.chan[chan].txPos);
	localtalkSend(scc.chan[chan].txData, scc.chan[chan].txPos);
	//Echo back data over Rx of same channel
//	sccRecv(chan, scc.chan[chan].txData, scc.chan[chan].txPos, 0);
//	triggerRx(chan);

	scc.chan[chan].txPos=0;
	//scc.chan[chan].hunting=1;
}

static void checkRxIntPending(int chan) {
	int doInt=0;
	int rxIntMode=(scc.chan[chan].wr1>>3)&0x3;
#ifdef SCC_DBG
	printf("Int check: chan %d rx has byte %d, int mode %d, intOnNextRxChar: %d\n", 
			chan, rxHasByte(chan), rxIntMode, scc.chan[chan].intOnNextRxChar);
#endif
	if (scc.chan[chan].intOnNextRxChar && rxHasByte(chan)) {
		doInt=1;
		scc.chan[chan].intOnNextRxChar=0;
	}
	if (rxIntMode>=1) {
		//special condition int... we handle only eof here
		if (rxHasByte(chan) && scc.chan[chan].rxEom) doInt=1;
	} 
	if (rxIntMode==1) {
		//int on 1st char
		if (rxHasByte(chan) && scc.chan[chan].rxPos==0) doInt=1;
	} else if (rxIntMode==2) {
		//int on all char
		if (rxHasByte(chan)) doInt=1;
	}

	//check global int ena
	int rxintena=scc.chan[chan].wr1&0x18;
	if (!rxintena) doInt=0;

	if (doInt) {
		scc.intpending|=((chan==0)?SCC_RR3_CHA_RX:SCC_RR3_CHB_RX);
	} else {
#ifdef SCC_DBG
		printf("Resetting int pending for channel %d\n", chan);
#endif
		scc.intpending&=~((chan==0)?SCC_RR3_CHA_RX:SCC_RR3_CHB_RX);
	}
	raiseInt(chan);
}

static void triggerRx(int chan) {
//	if (!scc.chan[chan].hunting) return;
	int bufid=scc.chan[chan].rxBufCur;
	printf("SCC: Receiving bufid %d:\n", bufid);
	hexdump(scc.chan[chan].rx[bufid].data, scc.chan[chan].rx[bufid].len);
	if (scc.chan[chan].rx[bufid].data[0]==0xFF || scc.chan[chan].rx[bufid].data[0]==scc.chan[chan].sdlcaddr) {
		scc.chan[chan].rxPos=0;
		printf("WR15: 0x%X WR1: %X\n", scc.chan[chan].wr15, scc.chan[chan].wr1);
		//Sync int
		if (scc.chan[chan].wr15&SCC_WR15_SYNC) {
			scc.intpending|=((chan==0)?SCC_RR3_CHA_EXT:SCC_RR3_CHB_EXT);
			raiseInt(chan);
		}
		//RxD int
		checkRxIntPending(chan);
		scc.chan[chan].hunting=0;
		scc.chan[chan].eofDelay=scc.chan[chan].rx[bufid].len*3;
	} else {
		printf("...Not for us, ignoring.\n");
		rxBufIgnoreRest(chan);
	}
}

void sccWrite(unsigned int addr, unsigned int val) {
	int chan, reg;
	if (addr & (1<<1)) chan=SCC_CHANA; else chan=SCC_CHANB;
	assert(scc.regptr<32);
	if (addr & (1<<2)) {
		//Data
		reg=8;
	} else {
		//Control
		reg=scc.regptr;
		scc.regptr=0;
	}
#ifdef SCC_DBG
	explainWrite(reg, chan, val);
#endif
	if (reg==0) {
		scc.regptr=val&0x7;
		if ((val&0x38)==0) {
			//nop
		} else if ((val&0x38)==0x8) {
			//point hi
			scc.regptr|=8;
		} else if ((val&0x38)==0x10) {
			//Reset ext/status int latch
			scc.chan[chan].rr0Latched=-1;
            scc.intpending &= ~(chan == SCC_CHANA ? SCC_RR3_CHA_EXT : SCC_RR3_CHB_EXT);
            raiseInt(chan);
		} else if ((val&0x38)==0x18) {
			//SCC abort: parse whatever we sent
//			printf("SCC ABORT: Sent data\n");
			sccTxFinished(chan);
			checkRxIntPending(chan);
		} else if ((val&0x38)==0x20) {
			//Int on next char
			scc.chan[chan].intOnNextRxChar=1;
			checkRxIntPending(chan);
		} else if ((val&0x38)==0x30) {
			//Error Reset: kills special condition bytes from fifo
			rxBufIgnoreRest(chan);
			checkRxIntPending(chan);
			printf("Error Reset Finished, pending=%x\n", scc.intpending);
		} else if ((val&0x38)==0x38) {
			//Reset Higher IUS
		} else {
			printf("Unknown command to reg 0: %X\n", val);
			exit_when_strict();
		}
		if ((val&0xc0)==0) {
			//Nop
		} else if ((val&0xc0)==0x80) {
			//Reset TX CRC gen
		} else if ((val&0xc0)==0xC0) {
			//reset tx underrun latch
//			sccTxFinished(chan);
			scc.chan[chan].txTimer=10;
			//if (scc.chan[chan].txTimer==0) scc.chan[chan].txTimer=-1;
		} else {
			exit_when_strict();
		}
	} else if (reg==1) {
		scc.chan[chan].wr1=val;
	} else if (reg==2) {
		scc.wr2=val;
	} else if (reg==3) {
		//bitsperchar1, bitsperchar0, autoenables, enterhuntmode, rxcrcen, addresssearch, synccharloadinh, rxena
		//autoenable: cts = tx ena, dcd = rx ena
		if (val&0x10) scc.chan[chan].hunting=1;
	} else if (reg==4) {
		//Parity, sync etc
	} else if (reg==5) {
		//Transmit parameters and controls
	} else if (reg==6) {
		scc.chan[chan].sdlcaddr=val;
	} else if (reg==7) {
		//Cync char /SDLC flag
	} else if (reg==8) {
		if (scc.chan[chan].txPos >= BUFLEN) solar_os_mac_fail("SCC transmit overflow");
        scc.chan[chan].txData[scc.chan[chan].txPos++]=val;
		scc.chan[chan].txTimer+=30;
#ifdef SCC_DBG
		printf("TX! Pos %d timer set to %d\n", scc.chan[chan].txPos, scc.chan[chan].txTimer);
#endif
	} else if (reg==9) {
		scc.wr9=val;
        if (val & 0x80) scc.intpending &= ~0x38;
        if (val & 0x40) scc.intpending &= ~0x07;
        raiseInt(chan);
	} else if (reg==10) {
		//Misc reg
	} else if (reg==11) {
		//PLL stuff
	} else if (reg==12) {
		//Baud lo
	} else if (reg==13) {
		//Baud hi
	} else if (reg==14) {
		//Baud generator stuff
		if ((val&0xe0)==0) {
			//null command
		} else if ((val&0xe0)==0x20) {
			//enter search mode
		} else if ((val&0xe0)==0xc0) {
			//set FM mode
		} else if ((val&0xe0)==0x40) {
			//reset missing clock
		} else {
			printf("Scc write undefined cmd to reg14: %x\n", val);
			exit_when_strict();
		}
	} else if (reg==15) {
		scc.chan[chan].wr15=val;
		raiseInt(chan);
	} else {
		printf("Scc write to undefined reg: %x\n", reg);
		exit_when_strict();
	}
	checkExtInt(chan);
//	printf("SCC: write to addr %x chan %d reg %d val %x\n", addr, chan, reg, val);
}


unsigned int sccRead(unsigned int addr) {
	int chan, reg, val=0xff;
	if (addr & (1<<1)) chan=SCC_CHANA; else chan=SCC_CHANB;
	assert(scc.regptr<32);
	if (addr & (1<<2)) {
		//Data
		reg=8;
	} else {
		//Control
		reg=scc.regptr;
		scc.regptr=0;
	}
	if (reg==0) {
		if (scc.chan[chan].rr0Latched==-1) {
			val=calcRr0(chan);
		} else {
			val=scc.chan[chan].rr0Latched;
			scc.chan[chan].rr0Latched=-1;
		}
	} else if (reg==1) {
		//Actually, these come out of the same fifo as the data, so this status should be for the fifo
		//val available.
		//EndOfFrame, CRCErr, RXOverrun, ParityErr, Residue0, Residue1, Residue2, AllSent
		val=0x7; //residue code 011, all sent
		//if (rxBytesLeft(chan)==1) val|=(1<<7); //end of frame
		if (scc.chan[chan].rxEom) val|=(1<<7); //end of frame
	} else if (reg==2 && chan==SCC_CHANB) {
		//We assume this also does an intack.
		int rsn=0;

#ifdef SCC_DBG
		printf("Pending: 0x%X\n", scc.intpending);
#endif
		if (scc.intpending & SCC_RR3_CHB_EXT) {
			rsn=1;
			scc.intpending&=~SCC_RR3_CHB_EXT;
		} else if (scc.intpending & SCC_RR3_CHA_EXT) {
			rsn=5;
			scc.intpending&=~SCC_RR3_CHA_EXT;
		} else if (scc.intpending & SCC_RR3_CHA_RX) {
			rsn=6;
			checkRxIntPending(0);
		} else if (scc.intpending & SCC_RR3_CHB_RX) {
			rsn=2;
			checkRxIntPending(1);
		}
#ifdef SCC_DBG
		printf("Rsn: %d\n", rsn);
#endif
		val=scc.wr2;
		if (scc.wr9&0x10) { //hi/lo
			val=(scc.wr2&~0x70)|rsn<<4;
		} else {
			val=(scc.wr2&~0xE)|rsn<<1;
		}
		if (scc.intpending&0x38) raiseInt(SCC_CHANA);
		if (scc.intpending&0x07) raiseInt(SCC_CHANB);
        raiseInt(chan);
	} else if (reg==3) {
		if (chan==SCC_CHANA) val=scc.intpending; else val=0;
	} else if (reg==8) {
		//rx buffer

		if (rxHasByte(chan)) {
			int left=0;
			val=rxByte(chan, &left);
#ifdef SCC_DBG
			printf("SCC READ DATA val %x, %d bytes left\n", val, left);
#endif
			if (left==1) { //because status goes with byte *to be read*, the last byte here is the EOM byte
				scc.chan[chan].rxEom=1;
				scc.chan[chan].rxAbrtTimer=40;
			} else {
				scc.chan[chan].rxEom=0;
			}
//			if (left==1) scc.chan[chan].hunting=1;
		} else {
			printf("SCC READ but no data?\n");
			scc.chan[chan].rxEom=0;
			val=0;
		}
		checkRxIntPending(chan);
	} else if (reg==10) {
		//Misc status (mostly SDLC)
		val=0;
	} else if (reg==15) {
		val=scc.chan[chan].wr15;
	} else {
		printf("Scc read from undefined reg: %x\n", reg);
		exit_when_strict();
	}
	checkExtInt(chan);
#ifdef SCC_DBG
	explainRead(reg, chan, val);
#endif
	return val;
}

//Called at about 800KHz
void sccTick(int noTicks) {
	for (int n=0; n<2; n++) {
		int needCheck=0;
		if (scc.chan[n].txTimer>0) {
			scc.chan[n].txTimer-=noTicks;
			if (scc.chan[n].txTimer<=0) {
				scc.chan[n].txTimer=0;
//				printf("Tx buffer empty: Sent data\n");
				sccTxFinished(n);
				needCheck=1;
			}
		}
		if (rxBufTick(n, noTicks)) {
			triggerRx(n);
			needCheck=1;
		}
		if (scc.chan[n].eofDelay>0) {
			scc.chan[n].eofDelay-=noTicks;
			if (scc.chan[n].eofDelay<0) scc.chan[n].eofDelay=0;
			if (scc.chan[n].eofDelay==0 && (scc.chan[n].wr1&0x10)!=0) {
				//Int mode is recv char or special / special only
				printf("Raise EOF int for channel %d\n", n);
				scc.chan[n].eofIntPending=1;
				scc.intpending|=((n==0)?SCC_RR3_CHA_RX:SCC_RR3_CHB_RX);
				raiseInt(n);
				needCheck=1;
			}
		}
		if (scc.chan[n].rxAbrtTimer>0) {
			scc.chan[n].rxAbrtTimer-=noTicks;
			if (scc.chan[n].rxAbrtTimer<0) scc.chan[n].rxAbrtTimer=0;
			needCheck=1;
		}
		if (needCheck) checkExtInt(n);
	}
    raiseInt(0);
}

void sccInit() {
    memset(&scc, 0, sizeof(scc));
	sccSetDcd(0, 1);
	sccSetDcd(1, 1);
//	scc.chan[0].txTimer=-1;
//	scc.chan[1].txTimer=-1;
	scc.chan[0].rr0Latched=-1;
	scc.chan[1].rr0Latched=-1;
	for (int x=0; x<NO_RXBUF; x++) {
		scc.chan[0].rx[x].delay=-1;
		scc.chan[1].rx[x].delay=-1;
	}
	scc.guard=0x1234;
}

