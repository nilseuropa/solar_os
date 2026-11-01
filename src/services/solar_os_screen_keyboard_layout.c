#include "solar_os_screen_keyboard_layout.h"

#include <string.h>
#include "solar_os_input.h"
#include "solar_os_keys.h"

typedef struct {
    const char *label;
    solar_os_screen_keyboard_action_t action;
} grid_key_t;
#define EMIT(k, s, u) {NULL, {SOLAR_OS_SCREEN_KEYBOARD_EMIT, k, s, u, 0}}
#define SYMBOL(k, u) {NULL, {SOLAR_OS_SCREEN_KEYBOARD_EMIT, k, k, u, SOLAR_OS_INPUT_MOD_LEFT_SHIFT}}
#define BUTTON(label, action, mod) {label, {action, 0, 0, 0, mod}}
#define PAGE 255U
static const grid_key_t full_keys[] = {
    EMIT('1','!',0x1e),EMIT('2','@',0x1f),EMIT('3','#',0x20),EMIT('4','$',0x21),EMIT('5','%',0x22),
    EMIT('6','^',0x23),EMIT('7','&',0x24),EMIT('8','*',0x25),EMIT('9','(',0x26),EMIT('0',')',0x27),
    EMIT('q','Q',0x14),EMIT('w','W',0x1a),EMIT('e','E',8),EMIT('r','R',0x15),EMIT('t','T',0x17),
    EMIT('y','Y',0x1c),EMIT('u','U',0x18),EMIT('i','I',0x0c),EMIT('o','O',0x12),EMIT('p','P',0x13),
    EMIT('a','A',4),EMIT('s','S',0x16),EMIT('d','D',7),EMIT('f','F',9),EMIT('g','G',0x0a),
    EMIT('h','H',0x0b),EMIT('j','J',0x0d),EMIT('k','K',0x0e),EMIT('l','L',0x0f),EMIT(8,8,0x2a),
    BUTTON(NULL,SOLAR_OS_SCREEN_KEYBOARD_MODIFIER,SOLAR_OS_INPUT_MOD_LEFT_SHIFT),
    EMIT('z','Z',0x1d),EMIT('x','X',0x1b),EMIT('c','C',6),EMIT('v','V',0x19),EMIT('b','B',5),
    EMIT('n','N',0x11),EMIT('m','M',0x10),EMIT('\r','\r',0x28),
    BUTTON("Ctrl",SOLAR_OS_SCREEN_KEYBOARD_MODIFIER,SOLAR_OS_INPUT_MOD_LEFT_CTRL),
    BUTTON("Alt",SOLAR_OS_SCREEN_KEYBOARD_MODIFIER,SOLAR_OS_INPUT_MOD_LEFT_ALT),
    EMIT(' ',' ',0x2c),
    BUTTON("Sym",PAGE,1),BUTTON("Nav",PAGE,2),BUTTON("Caps",SOLAR_OS_SCREEN_KEYBOARD_CAPS,0),
    BUTTON("Hide",SOLAR_OS_SCREEN_KEYBOARD_HIDE,0),
};
static const grid_key_t symbol_keys[] = {
    EMIT('=','=',0x2e),SYMBOL('+',0x2e),EMIT('-','-',0x2d),SYMBOL('*',0x25),EMIT('/','/',0x38),SYMBOL('%',0x22),SYMBOL('<',0x36),SYMBOL('>',0x37),
    SYMBOL('!',0x1e),SYMBOL('?',0x38),SYMBOL('"',0x34),EMIT('\'','\'',0x34),SYMBOL('(',0x26),SYMBOL(')',0x27),EMIT('[','[',0x2f),EMIT(']',']',0x30),
    SYMBOL('{',0x2f),SYMBOL('}',0x30),EMIT('\\','\\',0x31),SYMBOL('|',0x31),SYMBOL('_',0x2d),SYMBOL('~',0x35),SYMBOL('^',0x23),SYMBOL('&',0x24),
    SYMBOL('$',0x21),SYMBOL('#',0x20),SYMBOL('@',0x1f),EMIT(';',';',0x33),SYMBOL(':',0x33),EMIT(',',',',0x36),EMIT('.','.',0x37),EMIT('`','`',0x35),
    BUTTON("Ctrl",SOLAR_OS_SCREEN_KEYBOARD_MODIFIER,SOLAR_OS_INPUT_MOD_LEFT_CTRL),
    BUTTON("Alt",SOLAR_OS_SCREEN_KEYBOARD_MODIFIER,SOLAR_OS_INPUT_MOD_LEFT_ALT),
    BUTTON(NULL,SOLAR_OS_SCREEN_KEYBOARD_MODIFIER,SOLAR_OS_INPUT_MOD_LEFT_SHIFT),
    EMIT(' ',' ',0x2c),EMIT(8,8,0x2a),EMIT('\r','\r',0x28),
    BUTTON("ABC",PAGE,0),BUTTON("Hide",SOLAR_OS_SCREEN_KEYBOARD_HIDE,0),
};
/* Printable punctuation absent from T-Deck's normal and Sym key maps. */
static const grid_key_t deck_keys[] = {
    EMIT('=','=',0x2e),SYMBOL('<',0x36),SYMBOL('>',0x37),EMIT('[','[',0x2f),EMIT(']',']',0x30),SYMBOL('{',0x2f),SYMBOL('}',0x30),
    SYMBOL('%',0x22),SYMBOL('&',0x24),EMIT('\\','\\',0x31),SYMBOL('|',0x31),SYMBOL('^',0x23),SYMBOL('~',0x35),EMIT('`','`',0x35),
};
static const grid_key_t navigation_keys[] = {
    {"Esc",{SOLAR_OS_SCREEN_KEYBOARD_EMIT,27,27,0x29,0}},
    {"Home",{SOLAR_OS_SCREEN_KEYBOARD_EMIT,SOLAR_OS_KEY_HOME,SOLAR_OS_KEY_HOME,0x4a,0}},
    {"PgUp",{SOLAR_OS_SCREEN_KEYBOARD_EMIT,SOLAR_OS_KEY_PAGE_UP,SOLAR_OS_KEY_PAGE_UP,0x4b,0}},
    EMIT(SOLAR_OS_KEY_UP,SOLAR_OS_KEY_UP,0x52),EMIT(8,8,0x2a),
    EMIT('\t','\t',0x2b),
    {"End",{SOLAR_OS_SCREEN_KEYBOARD_EMIT,SOLAR_OS_KEY_END,SOLAR_OS_KEY_END,0x4d,0}},
    {"PgDn",{SOLAR_OS_SCREEN_KEYBOARD_EMIT,SOLAR_OS_KEY_PAGE_DOWN,SOLAR_OS_KEY_PAGE_DOWN,0x4e,0}},
    {"Del",{SOLAR_OS_SCREEN_KEYBOARD_EMIT,SOLAR_OS_KEY_DELETE,SOLAR_OS_KEY_DELETE,0x4c,0}},
    EMIT(SOLAR_OS_KEY_LEFT,SOLAR_OS_KEY_LEFT,0x50),
    EMIT(SOLAR_OS_KEY_DOWN,SOLAR_OS_KEY_DOWN,0x51),
    EMIT(SOLAR_OS_KEY_RIGHT,SOLAR_OS_KEY_RIGHT,0x4f),EMIT('\r','\r',0x28),
    BUTTON("Ctrl",SOLAR_OS_SCREEN_KEYBOARD_MODIFIER,SOLAR_OS_INPUT_MOD_LEFT_CTRL),
    BUTTON("Alt",SOLAR_OS_SCREEN_KEYBOARD_MODIFIER,SOLAR_OS_INPUT_MOD_LEFT_ALT),
    BUTTON(NULL,SOLAR_OS_SCREEN_KEYBOARD_MODIFIER,SOLAR_OS_INPUT_MOD_LEFT_SHIFT),
    EMIT(' ',' ',0x2c),
    BUTTON("ABC",PAGE,0),BUTTON("Sym",PAGE,1),BUTTON("Hide",SOLAR_OS_SCREEN_KEYBOARD_HIDE,0),
};
typedef struct {
    bool symbols_only, symbols, nav, moved, deck;
    int focus, pressed, press_x, press_y;
    solar_os_screen_keyboard_rect_t panel;
} grid_t;

static unsigned row_count(const grid_t *g, unsigned row)
{ return g->deck ? 7U : g->nav ? (row==0 ? 5U : row==1 ? 8U : 7U) : g->symbols ? 8U : row < 3 ? 10U : row == 3 ? 9U : 7U; }
static unsigned rows(const grid_t *g) { return g->deck ? 2U : g->nav ? 3U : 5U; }
static unsigned first(const grid_t *g, unsigned row)
{ unsigned n=0; for (unsigned r=0;r<row;r++) n+=row_count(g,r); return n; }
static unsigned count(const grid_t *g) { return first(g,rows(g)); }
static const grid_key_t *key_at(const grid_t *g, unsigned n)
{ return (g->deck ? deck_keys : g->nav ? navigation_keys : g->symbols ? symbol_keys : full_keys)+n; }
static solar_os_screen_keyboard_rect_t rect(const grid_t *g, unsigned n)
{
    unsigned row=0;
    while (row+1<rows(g) && n>=first(g,row+1)) row++;
    unsigned column=n-first(g,row), columns=row_count(g,row);
    if (g->nav && row==0) {
        static const unsigned positions[]={0,1,2,5,7};
        column=positions[column];columns=8;
    }
    int top=g->panel.y, height=g->panel.height;
    int x0=g->panel.width*column/columns, x1=g->panel.width*(column+1)/columns;
    int y0=top+height*row/rows(g), y1=top+height*(row+1)/rows(g);
    return (solar_os_screen_keyboard_rect_t){x0,y0,x1-x0,y1-y0};
}
static int hit(const grid_t *g,int x,int y)
{
    for (unsigned i=0;i<count(g);i++) {
        solar_os_screen_keyboard_rect_t r=rect(g,i);
        if (x>=r.x && x<r.x+r.width && y>=r.y && y<r.y+r.height) return i;
    }
    return -1;
}
static void cancel(void *state) { ((grid_t *)state)->pressed=-1; }
static void init(void *state,solar_os_screen_keyboard_mode_t mode)
{
    grid_t *g=state; memset(g,0,sizeof(*g));
    g->symbols_only=mode!=SOLAR_OS_SCREEN_KEYBOARD_MODE_FULL;
    g->symbols=mode==SOLAR_OS_SCREEN_KEYBOARD_MODE_SYMBOLS;
    g->deck=mode==SOLAR_OS_SCREEN_KEYBOARD_MODE_DECK;cancel(g);
}
static solar_os_screen_keyboard_rect_t resize(void *state,int width,int height)
{
    grid_t *g=state;
    int h=height<128 ? height : ((height*3/5+7)/8)*8;
    if (g->deck) h=((h*2/5+7)/8)*8;
    if (h>height) h=height;
    g->panel=(solar_os_screen_keyboard_rect_t){0,height-h,width,h};
    return g->panel;
}
static solar_os_screen_keyboard_action_t activate(grid_t *g)
{
    solar_os_screen_keyboard_action_t a=key_at(g,g->focus)->action;
    if ((unsigned)a.type==PAGE) {
        if (!g->symbols_only) { g->symbols=a.modifier==1;g->nav=a.modifier==2;g->focus=0; }
        a.type=SOLAR_OS_SCREEN_KEYBOARD_NONE;
    }
    return a;
}
static solar_os_screen_keyboard_action_t pointer(void *state,unsigned action,int x,int y)
{
    grid_t *g=state;
    solar_os_screen_keyboard_action_t a={0};
    if (action==SOLAR_OS_INPUT_POINTER_PRESS) {
        g->pressed=hit(g,x,y); g->press_x=x; g->press_y=y; g->moved=false;
        if (g->pressed>=0) g->focus=g->pressed;
    } else {
        int dx=x-g->press_x,dy=y-g->press_y;
        if (dx>8 || dx<-8 || dy>8 || dy<-8) g->moved=true;
        if (action==SOLAR_OS_INPUT_POINTER_RELEASE) {
            if (!g->moved && g->pressed>=0 && hit(g,x,y)==g->pressed) a=activate(g);
            cancel(g);
        }
    }
    return a;
}
static solar_os_screen_keyboard_action_t key(void *state,uint8_t k)
{
    grid_t *g=state;
    solar_os_screen_keyboard_action_t a={0};
    if (k=='\r' || k=='\n') return activate(g);
    if (k=='\t') {
        if (!g->symbols_only) {
            if (g->nav) g->nav=g->symbols=false;
            else if (g->symbols) {g->symbols=false;g->nav=true;}
            else g->symbols=true;
            g->focus=0;
        }
        return a;
    }
    if (k==SOLAR_OS_KEY_LEFT) g->focus=(g->focus+(int)count(g)-1)%count(g);
    else if (k==SOLAR_OS_KEY_RIGHT) g->focus=(g->focus+1)%count(g);
    else if (k==SOLAR_OS_KEY_UP || k==SOLAR_OS_KEY_DOWN) {
        unsigned row=0; while (row+1<rows(g) && g->focus>=(int)first(g,row+1)) row++;
        solar_os_screen_keyboard_rect_t current=rect(g,g->focus);
        row=(row+(k==SOLAR_OS_KEY_UP ? rows(g)-1 : 1))%rows(g);
        unsigned closest=first(g,row);
        int distance=g->panel.width*2;
        for (unsigned i=first(g,row);i<first(g,row+1);i++) {
            solar_os_screen_keyboard_rect_t candidate=rect(g,i);
            int dx=(candidate.x*2+candidate.width)-(current.x*2+current.width);
            if (dx<0) dx=-dx;
            if (dx<distance) {distance=dx;closest=i;}
        }
        g->focus=closest;
    }
    return a;
}
static solar_os_screen_keyboard_glyph_t glyph(const grid_key_t *k)
{
    if (k->action.type==SOLAR_OS_SCREEN_KEYBOARD_MODIFIER && k->action.modifier==SOLAR_OS_INPUT_MOD_LEFT_SHIFT)
        return SOLAR_OS_SCREEN_KEYBOARD_GLYPH_SHIFT;
    if (k->action.type!=SOLAR_OS_SCREEN_KEYBOARD_EMIT) return SOLAR_OS_SCREEN_KEYBOARD_GLYPH_NONE;
    switch (k->action.key) {
    case ' ': return SOLAR_OS_SCREEN_KEYBOARD_GLYPH_SPACE;
    case '\r': return SOLAR_OS_SCREEN_KEYBOARD_GLYPH_ENTER;
    case 8: return SOLAR_OS_SCREEN_KEYBOARD_GLYPH_BACKSPACE;
    case '\t': return SOLAR_OS_SCREEN_KEYBOARD_GLYPH_TAB;
    case SOLAR_OS_KEY_LEFT: return SOLAR_OS_SCREEN_KEYBOARD_GLYPH_LEFT;
    case SOLAR_OS_KEY_RIGHT: return SOLAR_OS_SCREEN_KEYBOARD_GLYPH_RIGHT;
    case SOLAR_OS_KEY_UP: return SOLAR_OS_SCREEN_KEYBOARD_GLYPH_UP;
    case SOLAR_OS_KEY_DOWN: return SOLAR_OS_SCREEN_KEYBOARD_GLYPH_DOWN;
    default: return SOLAR_OS_SCREEN_KEYBOARD_GLYPH_NONE;
    }
}
static void draw(void *state,const solar_os_screen_keyboard_painter_t *p,uint8_t mods,bool caps)
{
    grid_t *g=state;
    p->box(p->context,g->panel,false);
    for (unsigned i=0;i<count(g);i++) {
        solar_os_screen_keyboard_rect_t r=rect(g,i);
        r.x++;r.y++;r.width-=2;r.height-=2;
        const grid_key_t *k=key_at(g,i);
        bool active=(int)i==g->focus || (k->action.type==SOLAR_OS_SCREEN_KEYBOARD_MODIFIER && (mods&k->action.modifier)) ||
            (k->action.type==SOLAR_OS_SCREEN_KEYBOARD_CAPS && caps);
        p->box(p->context,r,active);
        char label[2]={k->action.key,0};
        bool shift=(mods&SOLAR_OS_INPUT_MOD_SHIFT)!=0;
        if (k->action.key>='a' && k->action.key<='z') shift^=caps;
        if (shift && k->action.shifted) label[0]=k->action.shifted;
        const char *text=k->label ? k->label : label;
        if (g->symbols_only && (unsigned)k->action.type==PAGE) text="Sym";
        solar_os_screen_keyboard_glyph_t icon=glyph(k);
        if (icon!=SOLAR_OS_SCREEN_KEYBOARD_GLYPH_NONE) p->glyph(p->context,r,icon,active);
        else p->text(p->context,r,text,active);
    }
}
static const solar_os_screen_keyboard_layout_t grid={
    .name="grid",.state_size=sizeof(grid_t),.init=init,.resize=resize,.draw=draw,
    .pointer=pointer,.key=key,.cancel=cancel,
};
const solar_os_screen_keyboard_layout_t *solar_os_screen_keyboard_layout_find(const char *name)
{
    static const solar_os_screen_keyboard_layout_t *const layouts[]={&grid};
    for (size_t i=0;i<sizeof(layouts)/sizeof(layouts[0]);i++)
        if (!strcmp(name,layouts[i]->name)) return layouts[i];
    return NULL;
}

const char *solar_os_screen_keyboard_mode_name(solar_os_screen_keyboard_mode_t mode)
{
    static const char *const names[]={"full","symbols","deck"};
    return (unsigned)mode<sizeof(names)/sizeof(names[0]) ? names[mode] : "unknown";
}
bool solar_os_screen_keyboard_mode_parse(const char *name,solar_os_screen_keyboard_mode_t *mode)
{
    if (!name || !mode) return false;
    for (unsigned i=SOLAR_OS_SCREEN_KEYBOARD_MODE_FULL;i<=SOLAR_OS_SCREEN_KEYBOARD_MODE_DECK;i++)
        if (!strcmp(name,solar_os_screen_keyboard_mode_name(i))) {*mode=i;return true;}
    return false;
}
