#define MOUSE_QXA (1<<0)
#define MOUSE_QXB (1<<1)
#define MOUSE_QYA (1<<2)
#define MOUSE_QYB (1<<3)
#define MOUSE_BTN (1<<4)


void mouseMove(int dx, int dy, int btn);
void mouseRelease(void);
int mouseTick();
