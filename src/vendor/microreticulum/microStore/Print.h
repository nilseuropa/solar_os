#pragma once

/*
 Print.h - Base class that provides print() and println()
 Copyright (c) 2008 David A. Mellis.  All right reserved.

 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Lesser General Public
 License as published by the Free Software Foundation; either
 version 2.1 of the License, or (at your option) any later version.

 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Lesser General Public License for more details.

 You should have received a copy of the GNU Lesser General Public
 License along with this library; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef ARDUINO

#ifndef Print_h
#define Print_h

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

#include "Print.h"
extern "C" {
    #include "time.h"
}

#include <string>

//#define DEC 10
//#define HEX 16
//#define OCT 8
//#define BIN 2
static constexpr uint8_t DEC {10};
static constexpr uint8_t HEX {16};
static constexpr uint8_t OCT {8};
static constexpr uint8_t BIN {2};

class Print
{
private:
    int write_error;
    size_t printNumber(unsigned long n, uint8_t base)
    {
        char buf[8 * sizeof(n) + 1]; // Assumes 8-bit chars plus zero byte.
        char *str = &buf[sizeof(buf) - 1];

        *str = '\0';

        // prevent crash if called with base == 1
        if(base < 2) {
            base = 10;
        }

        do {
            char c = n % base;
            n /= base;

            *--str = c < 10 ? c + '0' : c + 'A' - 10;
        } while (n);

        return write(str);
    }
    size_t printNumber(unsigned long long n, uint8_t base)
    {
        char buf[8 * sizeof(n) + 1]; // Assumes 8-bit chars plus zero byte.
        char* str = &buf[sizeof(buf) - 1];

        *str = '\0';

        // prevent crash if called with base == 1
        if (base < 2) {
            base = 10;
        }

        do {
            auto m = n;
            n /= base;
            char c = m - base * n;

            *--str = c < 10 ? c + '0' : c + 'A' - 10;
        } while (n);

        return write(str);
    }
    size_t printFloat(double number, uint8_t digits)
    {
        size_t n = 0;

        if(isnan(number)) {
            return print("nan");
        }
        if(isinf(number)) {
            return print("inf");
        }
        if(number > 4294967040.0) {
            return print("ovf");    // constant determined empirically
        }
        if(number < -4294967040.0) {
            return print("ovf");    // constant determined empirically
        }

        // Handle negative numbers
        if(number < 0.0) {
            n += print('-');
            number = -number;
        }

        // Round correctly so that print(1.999, 2) prints as "2.00"
        double rounding = 0.5;
        for(uint8_t i = 0; i < digits; ++i) {
            rounding /= 10.0;
        }

        number += rounding;

        // Extract the integer part of the number and print it
        unsigned long int_part = (unsigned long) number;
        double remainder = number - (double) int_part;
        n += print(int_part);

        // Print the decimal point, but only if there are digits beyond
        if(digits > 0) {
            n += print(".");
        }

        // Extract digits from the remainder one at a time
        while(digits-- > 0) {
            remainder *= 10.0;
            int toPrint = int(remainder);
            n += print(toPrint);
            remainder -= toPrint;
        }

        return n;
    }
protected:
    void setWriteError(int err = 1)
    {
        write_error = err;
    }
public:
    Print() :
        write_error(0)
    {
    }
    virtual ~Print() {}
    int getWriteError()
    {
        return write_error;
    }
    void clearWriteError()
    {
        setWriteError(0);
    }

    virtual size_t write(uint8_t) = 0;
    size_t write(const char *str)
    {
        if(str == NULL) {
            return 0;
        }
        return write((const uint8_t *) str, strlen(str));
    }
    virtual size_t write(const uint8_t *buffer, size_t size)
    {
        size_t n = 0;
        while(size--) {
            n += write(*buffer++);
        }
        return n;
    }
    size_t write(const char *buffer, size_t size)
    {
        return write((const uint8_t *) buffer, size);
    }

    size_t printf(const char * format, ...)  __attribute__ ((format (printf, 2, 3)))
    {
        char loc_buf[64];
        char * temp = loc_buf;
        va_list arg;
        va_list copy;
        va_start(arg, format);
        va_copy(copy, arg);
        int len = vsnprintf(temp, sizeof(loc_buf), format, copy);
        va_end(copy);
        if(len < 0) {
            va_end(arg);
            return 0;
        }
        if(len >= (int)sizeof(loc_buf)){  // comparation of same sign type for the compiler
            temp = (char*) malloc(len+1);
            if(temp == NULL) {
                va_end(arg);
                return 0;
            }
            len = vsnprintf(temp, len+1, format, arg);
        }
        va_end(arg);
        len = write((uint8_t*)temp, len);
        if(temp != loc_buf){
            free(temp);
        }
        return len;
    }

    // add availableForWrite to make compatible with Arduino Print.h
    // default to zero, meaning "a single write may block"
    // should be overriden by subclasses with buffering
    virtual int availableForWrite() { return 0; }

    size_t print(const std::string &s)
    {
        return write(s.c_str(), s.length());
    }
    size_t print(const char str[])
    {
        return write(str);
    }
    size_t print(char c)
    {
        return write(c);
    }
    size_t print(unsigned char b, int base = DEC)
    {
        return print((unsigned long) b, base);
    }
    size_t print(int n, int base = DEC)
    {
        return print((long) n, base);
    }
    size_t print(unsigned int n, int base = DEC)
    {
        return print((unsigned long) n, base);
    }
    size_t print(long n, int base = DEC)
    {
        int t = 0;
        if (base == 10 && n < 0) {
            t = print('-');
            n = -n;
        }
        return printNumber(static_cast<unsigned long>(n), base) + t;
    }
    size_t print(unsigned long n, int base = DEC)
    {
        if(base == 0) {
            return write(n);
        } else {
            return printNumber(n, base);
        }
    }
    size_t print(long long n, int base = DEC)
    {
        int t = 0;
        if (base == 10 && n < 0) {
            t = print('-');
            n = -n;
        }
        return printNumber(static_cast<unsigned long long>(n), base) + t;
    }
    size_t print(unsigned long long n, int base = DEC)
    {
        if (base == 0) {
            return write(n);
        } else {
            return printNumber(n, base);
        }
    }
    size_t print(double n, int digits = 2)
    {
        return printFloat(n, digits);
    }
    size_t print(struct tm * timeinfo, const char * format = NULL)
    {
        const char * f = format;
        if(!f){
            f = "%c";
        }
        char buf[64];
        size_t written = strftime(buf, 64, f, timeinfo);
        if(written == 0){
            return written;
        }
        return print(buf);
    }

    size_t println(void)
    {
        return print("\r\n");
    }
    size_t println(const std::string &s)
    {
        size_t n = print(s);
        n += println();
        return n;
    }
    size_t println(const char c[])
    {
        size_t n = print(c);
        n += println();
        return n;
    }
    size_t println(char c)
    {
        size_t n = print(c);
        n += println();
        return n;
    }
    size_t println(unsigned char b, int base = DEC)
    {
        size_t n = print(b, base);
        n += println();
        return n;
    }
    size_t println(int num, int base = DEC)
    {
        size_t n = print(num, base);
        n += println();
        return n;
    }
    size_t println(unsigned int num, int base = DEC)
    {
        size_t n = print(num, base);
        n += println();
        return n;
    }
    size_t println(long num, int base = DEC)
    {
        size_t n = print(num, base);
        n += println();
        return n;
    }
    size_t println(unsigned long num, int base = DEC)
    {
        size_t n = print(num, base);
        n += println();
        return n;
    }
    size_t println(long long num, int base = DEC)
    {
        size_t n = print(num, base);
        n += println();
        return n;
    }
    size_t println(unsigned long long num, int base = DEC)
    {
        size_t n = print(num, base);
        n += println();
        return n;
    }
    size_t println(double num, int digits = 2)
    {
        size_t n = print(num, digits);
        n += println();
        return n;
    }
    size_t println(struct tm * timeinfo, const char * format = NULL)
    {
        size_t n = print(timeinfo, format);
        n += println();
        return n;
    }
    
    virtual void flush() { /* Empty implementation for backward compatibility */ }
    
};

#endif

#endif
