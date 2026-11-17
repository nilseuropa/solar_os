#include "solar_os_native_abi.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)
static volatile int left = -73, right = 9;
static volatile unsigned bits = 0x87654321U, shift = 5;
static int counter;
static const char text[] = "SolarOS";
static int weight8(int a, int b, int c, int d, int e, int f, int g, int h)
{ return a + 2*b + 3*c + 4*d + 5*e + 6*f + 7*g + 8*h; }
static int subtract(int a, int b) { return a - b; }
static int (*function)(int, int) = subtract;
static int factorial(int n) { return n < 2 ? 1 : n * factorial(n - 1); }
static int bytes(signed char a, unsigned char b, short c, unsigned short d)
{ return a + b + c + d; }

int main(int argc, char **argv)
{
    int a = left, b = right, sum = 0, array[8];
    unsigned value = bits, amount = shift;
    signed char sc = -100;
    unsigned char uc = 200;
    short ss = -12345;
    unsigned short us = 54321;
    const solar_os_native_host_api_v1_t *host = solar_os_native_host_v1();
    (void)argc; (void)argv;
    CHECK(host && host->abi_version == 1);
    CHECK(a + b == -64 && a - b == -82 && a * b == -657);
    CHECK(a / b == -8 && a % b == -1);
    CHECK(value / b == 252395609U && value % b == 0);
    CHECK((value << amount) == 0xeca86420U && (value >> amount) == 0x043b2a19U);
    CHECK((a >> amount) == -3 && ((unsigned)a << 2) == 0xfffffedcU);
    CHECK((value & 0xff) == 0x21 && (value | 0xff) == 0x876543ffU);
    CHECK((value ^ 0xff) == 0x876543deU && ~value == 0x789abcdeU);
    CHECK(a < b && a <= b && b > a && b >= a && a != b && !(a == b));
    CHECK((unsigned)a > (unsigned)b && (unsigned)b < (unsigned)a);
    CHECK((a > b) == 0 && (a <= a) == 1 && (value >= value) == 1);
    CHECK((a ? b : 8) == 9 && (!a || b) && (a && b));
    for (int i = 0; i < 8; i++) { array[i] = i + 1; sum += array[i]; }
    CHECK(sum == 36 && array[7] == 8);
    CHECK(weight8(array[0], array[1], array[2], array[3], array[4], array[5], array[6], array[7]) == 204);
    CHECK(function(19, 23) == -4 && function(function(19, 23), 7) == -11);
    CHECK(factorial(7) == 5040);
    CHECK(sc == -100 && uc == 200 && ss == -12345 && us == 54321);
    CHECK(bytes(sc, uc, ss, us) == 42076);
    CHECK(text[0] == 'S' && text[6] == 'S' && text[7] == 0);
    switch (b) { case 2: return 90; case 9: counter++; break; default: return 91; }
    CHECK(counter == 1);
    host->write_utf8("TinyCC: scalar checks passed\n", 29);
    return 0;
}
