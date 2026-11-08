/* Minimal JSON parser (from the LCD Smartie Jellyfin plugin). */
#include "jf.h"
#include <ctype.h>

void jfree(J *j)
{
    while (j) {
        J *nx = j->next;
        jfree(j->kids);
        free(j->s);
        free(j->key);
        free(j);
        j = nx;
    }
}

static void jws(const char **p) { while (**p && isspace((unsigned char)**p)) (*p)++; }

static void put_utf8(char **o, unsigned cp)
{
    char *d = *o;
    if (cp < 0x80) *d++ = (char)cp;
    else if (cp < 0x800) { *d++ = 0xC0 | (cp >> 6); *d++ = 0x80 | (cp & 0x3F); }
    else if (cp < 0x10000) { *d++ = 0xE0 | (cp >> 12); *d++ = 0x80 | ((cp >> 6) & 0x3F); *d++ = 0x80 | (cp & 0x3F); }
    else { *d++ = 0xF0 | (cp >> 18); *d++ = 0x80 | ((cp >> 12) & 0x3F); *d++ = 0x80 | ((cp >> 6) & 0x3F); *d++ = 0x80 | (cp & 0x3F); }
    *o = d;
}

static unsigned hex4(const char *p)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return 0xFFFFFFFF;
    }
    return v;
}

static char *jparse_str(const char **p)
{
    const char *s = *p + 1;     /* skip opening quote */
    const char *e = s;
    while (*e && *e != '"') { if (*e == '\\' && e[1]) e++; e++; }
    if (*e != '"') return NULL;
    char *out = malloc((e - s) + 1), *o = out;   /* decoded is never longer */
    if (!out) return NULL;
    while (s < e) {
        if (*s != '\\') { *o++ = *s++; continue; }
        s++;
        switch (*s) {
        case 'n': *o++ = '\n'; s++; break;
        case 't': *o++ = '\t'; s++; break;
        case 'r': *o++ = '\r'; s++; break;
        case 'b': *o++ = '\b'; s++; break;
        case 'f': *o++ = '\f'; s++; break;
        case 'u': {
            if (e - s < 5) { s = e; break; }
            unsigned cp = hex4(s + 1);
            s += 5;
            if (cp == 0xFFFFFFFF) break;
            if (cp >= 0xD800 && cp <= 0xDBFF && e - s >= 6 && s[0] == '\\' && s[1] == 'u') {
                unsigned lo = hex4(s + 2);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    s += 6;
                }
            }
            put_utf8(&o, cp);
            break;
        }
        default: *o++ = *s++; break;   /* \" \\ \/ */
        }
    }
    *o = 0;
    *p = e + 1;
    return out;
}

static J *jparse_val(const char **p, int depth)
{
    if (depth > 64) return NULL;
    jws(p);
    J *j = calloc(1, sizeof(J));
    if (!j) return NULL;
    char c = **p;
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        j->t = c == '{' ? J_OBJ : J_ARR;
        (*p)++;
        J **tail = &j->kids;
        jws(p);
        if (**p == close) { (*p)++; return j; }
        for (;;) {
            char *key = NULL;
            jws(p);
            if (j->t == J_OBJ) {
                if (**p != '"' || !(key = jparse_str(p))) goto fail;
                jws(p);
                if (**p != ':') { free(key); goto fail; }
                (*p)++;
            }
            J *v = jparse_val(p, depth + 1);
            if (!v) { free(key); goto fail; }
            v->key = key;
            *tail = v;
            tail = &v->next;
            jws(p);
            if (**p == ',') { (*p)++; continue; }
            if (**p == close) { (*p)++; return j; }
            goto fail;
        }
    } else if (c == '"') {
        j->t = J_STR;
        if (!(j->s = jparse_str(p))) goto fail;
    } else if (!strncmp(*p, "true", 4)) { j->t = J_BOOL; j->n = 1; *p += 4; }
    else if (!strncmp(*p, "false", 5)) { j->t = J_BOOL; *p += 5; }
    else if (!strncmp(*p, "null", 4)) { j->t = J_NULL; *p += 4; }
    else {
        char *end;
        j->t = J_NUM;
        j->n = strtod(*p, &end);
        if (end == *p) goto fail;
        *p = end;
    }
    return j;
fail:
    jfree(j);
    return NULL;
}

J *jparse(const char *text)
{
    const char *p = text;
    return jparse_val(&p, 0);
}

J *jget(J *o, const char *key)
{
    if (!o || o->t != J_OBJ) return NULL;
    for (J *k = o->kids; k; k = k->next)
        if (k->key && !strcmp(k->key, key)) return k;
    return NULL;
}

const char *jstr(J *o, const char *key)
{
    J *v = jget(o, key);
    return (v && v->t == J_STR) ? v->s : "";
}

double jnum(J *o, const char *key)
{
    J *v = jget(o, key);
    return (v && (v->t == J_NUM || v->t == J_BOOL)) ? v->n : 0;
}

void jescape(char *out, const char *s)
{
    char *o = out;
    *o++ = '"';
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { *o++ = '\\'; *o++ = c; }
        else if (c == '\n') { *o++ = '\\'; *o++ = 'n'; }
        else if (c == '\r') { *o++ = '\\'; *o++ = 'r'; }
        else if (c == '\t') { *o++ = '\\'; *o++ = 't'; }
        else if (c < 0x20) o += sprintf(o, "\\u%04x", c);
        else *o++ = c;
    }
    *o++ = '"';
    *o = 0;
}
