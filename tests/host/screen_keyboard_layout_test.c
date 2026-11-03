#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_input.h"
#include "solar_os_keys.h"
#include "solar_os_screen_keyboard_layout.h"

static solar_os_screen_keyboard_rect_t glyphs[SOLAR_OS_SCREEN_KEYBOARD_GLYPH_DOWN+1];
static bool collect_deck;
static unsigned deck_count;
static char deck_labels[14];
static solar_os_screen_keyboard_rect_t deck_rects[14];
static void box(void *context,solar_os_screen_keyboard_rect_t r,bool filled)
{(void)context;(void)r;(void)filled;}
static void text(void *context,solar_os_screen_keyboard_rect_t r,const char *label,bool inverse)
{
    (void)context;(void)inverse;
    if (collect_deck) {
        assert(deck_count<14 && label[0] && !label[1]);
        deck_labels[deck_count]=label[0];deck_rects[deck_count++]=r;
    }
}
static void glyph(void *context,solar_os_screen_keyboard_rect_t r,solar_os_screen_keyboard_glyph_t key,bool inverse)
{(void)context;(void)inverse;glyphs[key]=r;}
static solar_os_screen_keyboard_action_t tap_glyph(const solar_os_screen_keyboard_layout_t *layout,
    void *state,solar_os_screen_keyboard_glyph_t glyph)
{
    solar_os_screen_keyboard_rect_t r=glyphs[glyph];assert(r.width && r.height);
    (void)layout->pointer(state,SOLAR_OS_INPUT_POINTER_PRESS,r.x+r.width/2,r.y+r.height/2);
    return layout->pointer(state,SOLAR_OS_INPUT_POINTER_RELEASE,r.x+r.width/2,r.y+r.height/2);
}

int main(void)
{
    const solar_os_screen_keyboard_layout_t *layout=solar_os_screen_keyboard_layout_find("grid");
    assert(layout && !solar_os_screen_keyboard_layout_find("missing"));
    void *state=calloc(1,layout->state_size);
    layout->init(state,true);
    solar_os_screen_keyboard_rect_t panel=layout->resize(state,400,240);
    assert(panel.x==0 && panel.width==400 && panel.y+panel.height==240);
    assert(layout->key(state,'\r').key=='=');
    char punctuation[33]={0};
    for (unsigned i=0;i<32;i++) {
        solar_os_screen_keyboard_action_t a=layout->key(state,'\r');
        assert(a.type==SOLAR_OS_SCREEN_KEYBOARD_EMIT && a.usage);punctuation[i]=a.key;
        (void)layout->key(state,SOLAR_OS_KEY_RIGHT);
    }
    for (const char *p="=\\|[]{}~`\"'!?<>+-*/%&^$#@;:,.()_";*p;p++) assert(strchr(punctuation,*p));
    assert(layout->key(state,'\r').type==SOLAR_OS_SCREEN_KEYBOARD_MODIFIER);
    layout->init(state,true);(void)layout->resize(state,400,240);
    int x=25,y=panel.y+panel.height/10;
    assert(layout->pointer(state,SOLAR_OS_INPUT_POINTER_PRESS,x,y).type==SOLAR_OS_SCREEN_KEYBOARD_NONE);
    assert(layout->pointer(state,SOLAR_OS_INPUT_POINTER_RELEASE,x,y).key=='=');
    (void)layout->pointer(state,SOLAR_OS_INPUT_POINTER_PRESS,x,y);
    (void)layout->pointer(state,SOLAR_OS_INPUT_POINTER_MOVE,x+30,y);
    assert(layout->pointer(state,SOLAR_OS_INPUT_POINTER_RELEASE,x,y).type==SOLAR_OS_SCREEN_KEYBOARD_NONE);
    (void)layout->pointer(state,SOLAR_OS_INPUT_POINTER_PRESS,x,y);layout->cancel(state);
    assert(layout->pointer(state,SOLAR_OS_INPUT_POINTER_RELEASE,x,y).type==SOLAR_OS_SCREEN_KEYBOARD_NONE);
    layout->init(state,false);(void)layout->resize(state,400,240);
    assert(layout->key(state,'\r').key=='1');
    (void)layout->key(state,'\t');assert(layout->key(state,'\r').key=='=');
    (void)layout->key(state,'\t');assert(layout->key(state,'\r').key==27);
    (void)layout->key(state,SOLAR_OS_KEY_RIGHT);assert(layout->key(state,'\r').key==SOLAR_OS_KEY_HOME);
    (void)layout->key(state,SOLAR_OS_KEY_DOWN);assert(layout->key(state,'\r').key==SOLAR_OS_KEY_END);
    (void)layout->key(state,SOLAR_OS_KEY_UP);assert(layout->key(state,'\r').key==SOLAR_OS_KEY_HOME);
    const solar_os_screen_keyboard_painter_t painter={.box=box,.text=text,.glyph=glyph};
    layout->draw(state,&painter,0,false);
    solar_os_screen_keyboard_rect_t up=glyphs[SOLAR_OS_SCREEN_KEYBOARD_GLYPH_UP];
    solar_os_screen_keyboard_rect_t down=glyphs[SOLAR_OS_SCREEN_KEYBOARD_GLYPH_DOWN];
    solar_os_screen_keyboard_rect_t left=glyphs[SOLAR_OS_SCREEN_KEYBOARD_GLYPH_LEFT];
    solar_os_screen_keyboard_rect_t right=glyphs[SOLAR_OS_SCREEN_KEYBOARD_GLYPH_RIGHT];
    assert(up.x==down.x && up.y<down.y);
    assert(left.y==down.y && right.y==down.y && left.x<down.x && down.x<right.x);
    assert(tap_glyph(layout,state,SOLAR_OS_SCREEN_KEYBOARD_GLYPH_UP).key==SOLAR_OS_KEY_UP);
    assert(tap_glyph(layout,state,SOLAR_OS_SCREEN_KEYBOARD_GLYPH_LEFT).key==SOLAR_OS_KEY_LEFT);
    assert(tap_glyph(layout,state,SOLAR_OS_SCREEN_KEYBOARD_GLYPH_RIGHT).key==SOLAR_OS_KEY_RIGHT);
    assert(tap_glyph(layout,state,SOLAR_OS_SCREEN_KEYBOARD_GLYPH_DOWN).key==SOLAR_OS_KEY_DOWN);
    (void)layout->key(state,SOLAR_OS_KEY_UP);assert(layout->key(state,'\r').key==SOLAR_OS_KEY_UP);
    (void)layout->key(state,SOLAR_OS_KEY_DOWN);assert(layout->key(state,'\r').key==SOLAR_OS_KEY_DOWN);
    assert(tap_glyph(layout,state,SOLAR_OS_SCREEN_KEYBOARD_GLYPH_BACKSPACE).key==8);
    assert(tap_glyph(layout,state,SOLAR_OS_SCREEN_KEYBOARD_GLYPH_ENTER).key=='\r');
    assert(tap_glyph(layout,state,SOLAR_OS_SCREEN_KEYBOARD_GLYPH_SPACE).key==' ');
    (void)layout->key(state,'\t');assert(layout->key(state,'\r').key=='1');
    (void)layout->key(state,SOLAR_OS_KEY_UP);
    assert(layout->key(state,'\r').type==SOLAR_OS_SCREEN_KEYBOARD_MODIFIER);
    /* Every navigation direction remains valid on very small displays. */
    (void)layout->resize(state,80,64);
    for (unsigned i=0;i<100;i++) {
        (void)layout->key(state,SOLAR_OS_KEY_DOWN);(void)layout->key(state,SOLAR_OS_KEY_RIGHT);
        (void)layout->key(state,SOLAR_OS_KEY_UP);(void)layout->key(state,SOLAR_OS_KEY_LEFT);
    }
    solar_os_screen_keyboard_mode_t mode;
    assert(solar_os_screen_keyboard_mode_parse("deck",&mode) && mode==SOLAR_OS_SCREEN_KEYBOARD_MODE_DECK);
    assert(!strcmp(solar_os_screen_keyboard_mode_name(mode),"deck"));
    assert(!solar_os_screen_keyboard_mode_parse("invalid",&mode));
    layout->init(state,SOLAR_OS_SCREEN_KEYBOARD_MODE_DECK);
    panel=layout->resize(state,320,240);
    assert(panel.height<=64 && panel.y+panel.height==240);
    collect_deck=true;layout->draw(state,&painter,0,false);collect_deck=false;
    assert(deck_count==14 && deck_rects[0].y<deck_rects[7].y);
    for (unsigned i=0;i<14;i++) {
        assert(deck_rects[i].y==deck_rects[i<7?0:7].y);
        assert(layout->key(state,'\r').key==(uint8_t)deck_labels[i]);
        (void)layout->key(state,SOLAR_OS_KEY_RIGHT);
    }
    assert(layout->key(state,'\r').key=='=');
    (void)layout->key(state,'\t');assert(layout->key(state,'\r').key=='=');
    (void)layout->key(state,SOLAR_OS_KEY_DOWN);assert(layout->key(state,'\r').key=='%');
    for (unsigned i=0;i<14;i++) {
        solar_os_screen_keyboard_rect_t r=deck_rects[i];
        (void)layout->pointer(state,SOLAR_OS_INPUT_POINTER_PRESS,r.x+r.width/2,r.y+r.height/2);
        solar_os_screen_keyboard_action_t action=layout->pointer(state,SOLAR_OS_INPUT_POINTER_RELEASE,r.x+r.width/2,r.y+r.height/2);
        assert(action.type==SOLAR_OS_SCREEN_KEYBOARD_EMIT && action.key==(uint8_t)deck_labels[i] && action.usage);
    }
    free(state);puts("screen keyboard layout tests: ok");
}
