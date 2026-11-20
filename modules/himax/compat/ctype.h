#pragma once
/* JSON is ASCII. Keep the external provider independent of newlib locale data. */
static inline int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static inline int isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int isxdigit(int c)
{ return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
