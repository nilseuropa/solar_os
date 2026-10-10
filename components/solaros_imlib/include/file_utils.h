#pragma once
/* Only forward declarations in imlib.h need file_t. File I/O is disabled;
 * SolarOS owns decoding and storage access. */
typedef struct { void *unused; } file_t;
