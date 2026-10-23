/* ~/.config/hakurei.conf: the desktop's small settings file (written by the Kappa Control Panel). Lines are key=value,
 * '#' starts a comment. Only clock24 is read here: 0 / no / false means a 12 hour display, anything else 24 hours. */
#include "watch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

void conf_reload(void) {
    bool clock24 = true;
    const char *home = getenv("HOME");
    char path[300];
    snprintf(path, sizeof path, "%s/.config/hakurei.conf", home && *home ? home : "");
    FILE *f = fopen(path, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char *p = line;
            while (isspace((unsigned char)*p)) p++;
            if (*p == '#' || !*p) continue;
            char *eq = strchr(p, '=');
            if (!eq) continue;
            *eq = 0;
            char *key = p, *val = eq + 1;
            for (char *e = key + strlen(key); e > key && isspace((unsigned char)e[-1]); ) *--e = 0;
            while (isspace((unsigned char)*val)) val++;
            for (char *e = val + strlen(val); e > val && isspace((unsigned char)e[-1]); ) *--e = 0;
            if (!strcmp(key, "clock24")) clock24 = !(!strcmp(val, "0") || !strcasecmp(val, "no") || !strcasecmp(val, "false") || !strcasecmp(val, "off"));
        }
        fclose(f);
    }
    W.clock24 = clock24;
}
