
#define VIA_CA1 0
#define VIA_CA2 1
#define VIA_CB1 2
#define VIA_CB2 3

#define VIA_PORTA 0
#define VIA_PORTB 1

void viaWrite(unsigned int addr, unsigned int val);
unsigned int viaRead(unsigned int addr);
void viaControlWrite(int no, int val);
void viaStep(int clockcycles);
void viaSet(int no, int mask);
void viaClear(int no, int mask);
void kbdPushKey(uint8_t scancode, int isRelease);
void kbdClearQueue(void);
