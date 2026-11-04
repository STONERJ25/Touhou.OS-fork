/* Driver for the damage tests of tests/host/test_dmi.py: reads records from stdin, each a 32-bit version, a 32-bit length and that many
 * bytes of SMBIOS table; walks and formats every one of them. Built with the address and undefined-behaviour sanitizers, and the table
 * handed to the walker is a heap block of exactly the stated length, so a read past the end of a table is an error the sanitizer reports. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmi.h"

int main(void) {
    static DmiInfo info;
    static char text[16384];
    unsigned long cases = 0, bytes = 0;
    for (;;) {
        unsigned char hdr[8];
        if (fread(hdr, 1, 8, stdin) != 8) break;
        unsigned ver = hdr[0] | hdr[1] << 8 | hdr[2] << 16 | (unsigned)hdr[3] << 24;
        unsigned len = hdr[4] | hdr[5] << 8 | hdr[6] << 16 | (unsigned)hdr[7] << 24;
        unsigned char *t = malloc(len ? len : 1);
        if (!t || fread(t, 1, len, stdin) != len) return 2;
        dmi_parse(&info, len ? t : NULL, len, (unsigned short)ver);
        info.kind = 3;
        size_t n = dmi_format(&info, text, sizeof text);
        if (n >= sizeof text) return 3;
        free(t);
        cases++;
        bytes += len;
    }
    printf("%lu cases, %lu bytes\n", cases, bytes);
    return 0;
}
