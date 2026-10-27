
#define SCC_CHANA 0
#define SCC_CHANB 1

void sccWrite(unsigned int addr, unsigned int val);
unsigned int sccRead(unsigned int addr);
void sccSetDcd(int chan, int val);
void sccInit();
void sccTick(int cycles);
void sccRecv(int chan, uint8_t *data, int len, int delay);

