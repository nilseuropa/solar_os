#pragma once

/*
 Stream.h - base class for character-based streams.
 Copyright (c) 2010 David A. Mellis.  All right reserved.

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

 parsing functions based on TextFinder library by Michael Margolis
 */

#ifndef ARDUINO

#ifndef Stream_h
#define Stream_h

#include <inttypes.h>
#include "Print.h"

#define PARSE_TIMEOUT 1000  // default number of milli-seconds to wait
#define NO_SKIP_CHAR  1  // a magic char not found in a valid ASCII numeric field

// compatability macros for testing
/*
 #define   getInt()            parseInt()
 #define   getInt(skipChar)    parseInt(skipchar)
 #define   getFloat()          parseFloat()
 #define   getFloat(skipChar)  parseFloat(skipChar)
 #define   getString( pre_string, post_string, buffer, length)
 readBytesBetween( pre_string, terminator, buffer, length)
 */

class Stream: public Print
{
protected:
    unsigned long _timeout;      // number of milliseconds to wait for the next char before aborting timed read
    unsigned long _startMillis;  // used for timeout measurement

    static inline uint32_t millis()
    {
        return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // private method to read stream with timeout
    int timedRead()
    {
        int c;
        _startMillis = millis();
        do {
            c = read();
            if(c >= 0) {
                return c;
            }
        } while(millis() - _startMillis < _timeout);
        return -1;     // -1 indicates timeout
    }

    // private method to peek stream with timeout
    int timedPeek()
    {
        int c;
        _startMillis = millis();
        do {
            c = peek();
            if(c >= 0) {
                return c;
            }
        } while(millis() - _startMillis < _timeout);
        return -1;     // -1 indicates timeout
    }

    // returns peek of the next digit in the stream or -1 if timeout
    // discards non-numeric characters
    int peekNextDigit()
    {
        int c;
        while(1) {
            c = timedPeek();
            if(c < 0) {
                return c;    // timeout
            }
            if(c == '-') {
                return c;
            }
            if(c >= '0' && c <= '9') {
                return c;
            }
            read();  // discard non-numeric
        }
    }

public:
    virtual int available() = 0;
    virtual int read() = 0;
    virtual int peek() = 0;

    Stream():_startMillis(0)
    {
        _timeout = 1000;
    }
    virtual ~Stream() {}

// parsing methods

    // sets the maximum number of milliseconds to wait
    void setTimeout(unsigned long timeout)
    {
        _timeout = timeout;
    }
    unsigned long getTimeout(void) {
        return _timeout;
    }

    // find returns true if the target string is found
    bool  find(const char *target)
    {
        return findUntil(target, strlen(target), NULL, 0);
    }
    bool find(uint8_t *target)
    {
        return find((char *) target);
    }
    // returns true if target string is found, false if timed out (see setTimeout)

    // reads data from the stream until the target string of given length is found
    // returns true if target string is found, false if timed out
    bool find(const char *target, size_t length)
    {
        return findUntil(target, length, NULL, 0);
    }
    bool find(const uint8_t *target, size_t length)
    {
        return find((char *) target, length);
    }
    // returns true if target string is found, false if timed out

    bool find(char target)
    {
        return find (&target, 1);
    }

    // as find but search ends if the terminator string is found
    bool  findUntil(const char *target, const char *terminator)
    {
        return findUntil(target, strlen(target), terminator, strlen(terminator));
    }
    bool findUntil(const uint8_t *target, const char *terminator)
    {
        return findUntil((char *) target, terminator);
    }

    // reads data from the stream until the target string of the given length is found
    // search terminated if the terminator string is found
    // returns true if target string is found, false if terminated or timed out
    bool findUntil(const char *target, size_t targetLen, const char *terminator, size_t termLen)
    {
        if (terminator == NULL) {
            MultiTarget t[1] = {{target, targetLen, 0}};
            return findMulti(t, 1) == 0 ? true : false;
        } else {
            MultiTarget t[2] = {{target, targetLen, 0}, {terminator, termLen, 0}};
            return findMulti(t, 2) == 0 ? true : false;
        }
    }
    bool findUntil(const uint8_t *target, size_t targetLen, const char *terminate, size_t termLen)
    {
        return findUntil((char *) target, targetLen, terminate, termLen);
    }

    // returns the first valid (long) integer value from the current position.
    // initial characters that are not digits (or the minus sign) are skipped
    // function is terminated by the first character that is not a digit.
    long parseInt()
    {
        return parseInt(NO_SKIP_CHAR); // terminate on first non-digit character (or timeout)
    }
    // initial characters that are not digits (or the minus sign) are skipped
    // integer is terminated by the first character that is not a digit.

    // as parseInt but returns a floating point value
    float parseFloat()
    {
        return parseFloat(NO_SKIP_CHAR);
    }

    // read characters from stream into buffer
    // terminates if length characters have been read, or timeout (see setTimeout)
    // returns the number of characters placed in the buffer
    // the buffer is NOT null terminated.
    //
    size_t readBytes(char *buffer, size_t length)
    {
        size_t count = 0;
        while(count < length) {
            int c = timedRead();
            if(c < 0) {
                break;
            }
            *buffer++ = (char) c;
            count++;
        }
        return count;
    }
    virtual size_t readBytes(uint8_t *buffer, size_t length)
    {
        return readBytes((char *) buffer, length);
    }
    // terminates if length characters have been read or timeout (see setTimeout)
    // returns the number of characters placed in the buffer (0 means no valid data found)

    // as readBytes with terminator character
    // terminates if length characters have been read, timeout, or if the terminator character  detected
    // returns the number of characters placed in the buffer (0 means no valid data found)

    size_t readBytesUntil(char terminator, char *buffer, size_t length)
    {
        if(length < 1) {
            return 0;
        }
        size_t index = 0;
        while(index < length) {
            int c = timedRead();
            if(c < 0 || c == terminator) {
                break;
            }
            *buffer++ = (char) c;
            index++;
        }
        return index; // return number of characters, not including null terminator
    }
    size_t readBytesUntil(char terminator, uint8_t *buffer, size_t length)
    {
        return readBytesUntil(terminator, (char *) buffer, length);
    }
    // terminates if length characters have been read, timeout, or if the terminator character  detected
    // returns the number of characters placed in the buffer (0 means no valid data found)

    // Arduino String functions to be added here
    std::string readString()
    {
        std::string ret;
        int c = timedRead();
        while(c >= 0) {
            ret += (char) c;
            c = timedRead();
        }
        return ret;
    }
    std::string readStringUntil(char terminator)
    {
        std::string ret;
        int c = timedRead();
        while(c >= 0 && c != terminator) {
            ret += (char) c;
            c = timedRead();
        }
        return ret;
    }

protected:
    // as above but a given skipChar is ignored
    // this allows format characters (typically commas) in values to be ignored
    long parseInt(char skipChar)
    {
        bool isNegative = false;
        long value = 0;
        int c;

        c = peekNextDigit();
        // ignore non numeric leading characters
        if(c < 0) {
            return 0;    // zero returned if timeout
        }

        do {
            if(c == skipChar) {
            } // ignore this charactor
            else if(c == '-') {
                isNegative = true;
            } else if(c >= '0' && c <= '9') {    // is c a digit?
                value = value * 10 + c - '0';
            }
            read();  // consume the character we got with peek
            c = timedPeek();
        } while((c >= '0' && c <= '9') || c == skipChar);

        if(isNegative) {
            value = -value;
        }
        return value;
    }
    // as above but the given skipChar is ignored
    // this allows format characters (typically commas) in values to be ignored

    // as above but the given skipChar is ignored
    // this allows format characters (typically commas) in values to be ignored
    float parseFloat(char skipChar)
    {
        bool isNegative = false;
        bool isFraction = false;
        long value = 0;
        int c;
        float fraction = 1.0;

        c = peekNextDigit();
        // ignore non numeric leading characters
        if(c < 0) {
            return 0;    // zero returned if timeout
        }

        do {
            if(c == skipChar) {
            } // ignore
            else if(c == '-') {
                isNegative = true;
            } else if(c == '.') {
                isFraction = true;
            } else if(c >= '0' && c <= '9') {    // is c a digit?
                value = value * 10 + c - '0';
                if(isFraction) {
                    fraction *= 0.1f;
                }
            }
            read();  // consume the character we got with peek
            c = timedPeek();
        } while((c >= '0' && c <= '9') || c == '.' || c == skipChar);

        if(isNegative) {
            value = -value;
        }
        if(isFraction) {
            return value * fraction;
        } else {
            return value;
        }
    }
  
    struct MultiTarget {
      const char *str;  // string you're searching for
      size_t len;       // length of string you're searching for
      size_t index;     // index used by the search routine.
    };

  // This allows you to search for an arbitrary number of strings.
  // Returns index of the target that is found first or -1 if timeout occurs.
    int findMulti( struct MultiTarget *targets, int tCount) {
        // any zero length target string automatically matches and would make
        // a mess of the rest of the algorithm.
        for (struct MultiTarget *t = targets; t < targets+tCount; ++t) {
            if (t->len <= 0)
            return t - targets;
        }

        while (1) {
            int c = timedRead();
            if (c < 0)
            return -1;

            for (struct MultiTarget *t = targets; t < targets+tCount; ++t) {
            // the simple case is if we match, deal with that first.
            if (c == t->str[t->index]) {
                if (++t->index == t->len)
                return t - targets;
                else
                continue;
            }

            // if not we need to walk back and see if we could have matched further
            // down the stream (ie '1112' doesn't match the first position in '11112'
            // but it will match the second position so we can't just reset the current
            // index to 0 when we find a mismatch.
            if (t->index == 0)
                continue;

            int origIndex = t->index;
            do {
                --t->index;
                // first check if current char works against the new current index
                if (c != t->str[t->index])
                continue;

                // if it's the only char then we're good, nothing more to check
                if (t->index == 0) {
                t->index++;
                break;
                }

                // otherwise we need to check the rest of the found string
                int diff = origIndex - t->index;
                size_t i;
                for (i = 0; i < t->index; ++i) {
                if (t->str[i] != t->str[i + diff])
                    break;
                }

                // if we successfully got through the previous loop then our current
                // index is good.
                if (i == t->index) {
                t->index++;
                break;
                }

                // otherwise we just try the next index
            } while (t->index);
            }
        }
        // unreachable
        return -1;
    }

};

#endif

#endif
