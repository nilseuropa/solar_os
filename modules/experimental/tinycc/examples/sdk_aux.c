#define AUX_ONLY 1
#include <stdlib.h>
static int private_value=23;
int sdk_aux(void) {return abs(-private_value);}
