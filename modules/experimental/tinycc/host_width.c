#include "tcc.h"
_Static_assert(sizeof(void *) == 4, "compiler host must be 32 bit for this check");
_Static_assert(sizeof(long) == 4, "compiler host long must be 32 bit for this check");
_Static_assert(sizeof(((CValue *)0)->i) == 8, "integer constants need 64 bit storage");
_Static_assert(sizeof(((CValue *)0)->d) == 8, "double constants need 64 bit storage");
_Static_assert(PTR_SIZE == 4, "output target must have 32 bit pointers");
