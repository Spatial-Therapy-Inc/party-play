#include "pp_launch.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "pp_record.h"
#include "pp_util.h"

static const char *ws(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

/** Reads a JSON string into out (escapes other than \" and \\ are dropped). Returns the char after it or NULL. */
static const char *str(const char *p, char *out, size_t size) {
    if (*p != '"') return NULL;
    p++;
    size_t n = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) c = *p++;
        if (n + 1 < size) out[n++] = c;
    }
    if (*p != '"') return NULL;
    if (size) out[n] = 0;
    return p + 1;
}

/** Skips any JSON value (strings, numbers, literals, nested objects and arrays). */
static const char *skip(const char *p) {
    p = ws(p);
    if (*p == '"') {
        char tmp[2];
        return str(p, tmp, sizeof(tmp));
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                char tmp[2];
                p = str(p, tmp, sizeof(tmp));
                if (!p) return NULL;
                continue;
            }
            if (*p == '{' || *p == '[') depth++;
            if (*p == '}' || *p == ']') {
                depth--;
                if (depth == 0) return p + 1;
            }
            p++;
        }
        return NULL;
    }
    while (*p && *p != ',' && *p != '}') p++;
    return p;
}

void pp_launch_parse(const char *json, pp_launch_params_t *out) {
    out->game = -1;
    out->players = 0;
    out->dice = 0;
    if (!json) return;
    const char *p = ws(json);
    if (*p != '{') return;
    p++;
    while (p && *(p = ws(p)) && *p != '}') {
        char key[16];
        p = str(p, key, sizeof(key));
        if (!p) return;
        p = ws(p);
        if (*p != ':') return;
        p = ws(p + 1);
        if (!strcmp(key, "game") && *p == '"') {
            char val[16];
            const char *q = str(p, val, sizeof(val));
            if (q) out->game = pp_game_from_key(val);
            p = q;
        } else if ((!strcmp(key, "players") || !strcmp(key, "dice")) && (*p == '-' || isdigit((unsigned char)*p))) {
            char *end;
            const double v = strtod(p, &end);
            const int iv = (int)(v + (v >= 0 ? 0.5 : -0.5));
            if (!strcmp(key, "players")) out->players = pp_clampi(iv, 2, 8);
            else out->dice = pp_clampi(iv, 1, 3);
            p = end;
        } else {
            p = skip(p);
        }
        if (!p) return;
        p = ws(p);
        if (*p == ',') p++;
    }
}
