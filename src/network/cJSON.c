/*
  Copyright (c) 2009-2017 Dave Gamble and cJSON contributors

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

#include "cJSON.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <limits.h>
#include <ctype.h>

static void *(*cJSON_malloc)(size_t sz) = malloc;
static void (*cJSON_free)(void *ptr) = free;

static char* cJSON_strdup(const char* str)
{
    size_t len;
    char* copy;

    if (!str) return NULL;
    len = strlen(str) + 1;
    copy = (char*)cJSON_malloc(len);
    if (!copy) return NULL;
    memcpy(copy, str, len);
    return copy;
}

void cJSON_InitHooks(cJSON_Hooks* hooks)
{
    if (!hooks) {
        cJSON_malloc = malloc;
        cJSON_free = free;
        return;
    }
    cJSON_malloc = (hooks->malloc_fn) ? hooks->malloc_fn : malloc;
    cJSON_free = (hooks->free_fn) ? hooks->free_fn : free;
}

static cJSON *cJSON_New_Item(void)
{
    cJSON* node = (cJSON*)cJSON_malloc(sizeof(cJSON));
    if (node) memset(node, 0, sizeof(cJSON));
    return node;
}

void cJSON_Delete(cJSON *c)
{
    cJSON *next;
    while (c) {
        next = c->next;
        if (!(c->type & cJSON_IsReference) && c->child) cJSON_Delete(c->child);
        if (!(c->type & cJSON_IsReference) && c->valuestring) cJSON_free(c->valuestring);
        if (!(c->type & cJSON_StringIsConst) && c->string) cJSON_free(c->string);
        cJSON_free(c);
        c = next;
    }
}

/* Skip whitespace */
static const char *skip(const char *in)
{
    while (in && *in && ((unsigned char)*in <= 32)) in++;
    return in;
}

/* Parse number */
static const char *parse_number(cJSON *item, const char *num)
{
    double n = 0;
    char *endptr;
    n = strtod(num, &endptr);
    if (endptr == num) return NULL;

    item->valuedouble = n;
    item->valueint = (int)n;
    item->type = cJSON_Number;
    return endptr;
}

/* Parse string */
static const char *parse_string(cJSON *item, const char *str)
{
    const char *ptr = str + 1;
    char *ptr2;
    char *out;
    int len = 0;

    if (*str != '\"') return NULL;

    while (*ptr != '\"' && *ptr) {
        if (*ptr++ == '\\') {
            if (*ptr == 0) return NULL;
            ptr++;
        }
        len++;
    }

    out = (char*)cJSON_malloc(len + 1);
    if (!out) return NULL;

    ptr = str + 1;
    ptr2 = out;
    while (*ptr != '\"' && *ptr) {
        if (*ptr != '\\') {
            *ptr2++ = *ptr++;
        } else {
            ptr++;
            switch (*ptr) {
                case 'b': *ptr2++ = '\b'; break;
                case 'f': *ptr2++ = '\f'; break;
                case 'n': *ptr2++ = '\n'; break;
                case 'r': *ptr2++ = '\r'; break;
                case 't': *ptr2++ = '\t'; break;
                case '\"': *ptr2++ = '\"'; break;
                case '\\': *ptr2++ = '\\'; break;
                case '/': *ptr2++ = '/'; break;
                default: *ptr2++ = *ptr; break;
            }
            ptr++;
        }
    }
    *ptr2 = 0;
    if (*ptr == '\"') ptr++;
    item->valuestring = out;
    item->type = cJSON_String;
    return ptr;
}

static const char *parse_value(cJSON *item, const char *value);
static const char *parse_array(cJSON *item, const char *value);
static const char *parse_object(cJSON *item, const char *value);

static const char *parse_array(cJSON *item, const char *value)
{
    cJSON *child;
    if (*value != '[') return NULL;

    item->type = cJSON_Array;
    value = skip(value + 1);
    if (*value == ']') return value + 1;

    item->child = child = cJSON_New_Item();
    if (!item->child) return NULL;
    value = skip(parse_value(child, skip(value)));
    if (!value) return NULL;

    while (*value == ',') {
        cJSON *new_item = cJSON_New_Item();
        if (!new_item) return NULL;
        child->next = new_item;
        new_item->prev = child;
        child = new_item;
        value = skip(parse_value(child, skip(value + 1)));
        if (!value) return NULL;
    }

    if (*value == ']') return value + 1;
    return NULL;
}

static const char *parse_object(cJSON *item, const char *value)
{
    cJSON *child;
    if (*value != '{') return NULL;

    item->type = cJSON_Object;
    value = skip(value + 1);
    if (*value == '}') return value + 1;

    item->child = child = cJSON_New_Item();
    if (!item->child) return NULL;
    value = skip(parse_string(child, skip(value)));
    if (!value) return NULL;
    child->string = child->valuestring;
    child->valuestring = NULL;
    if (*value != ':') return NULL;
    value = skip(parse_value(child, skip(value + 1)));
    if (!value) return NULL;

    while (*value == ',') {
        cJSON *new_item = cJSON_New_Item();
        if (!new_item) return NULL;
        child->next = new_item;
        new_item->prev = child;
        child = new_item;
        value = skip(parse_string(child, skip(value + 1)));
        if (!value) return NULL;
        child->string = child->valuestring;
        child->valuestring = NULL;
        if (*value != ':') return NULL;
        value = skip(parse_value(child, skip(value + 1)));
        if (!value) return NULL;
    }

    if (*value == '}') return value + 1;
    return NULL;
}

static const char *parse_value(cJSON *item, const char *value)
{
    if (!value) return NULL;
    if (!strncmp(value, "null", 4))  { item->type = cJSON_NULL;  return value + 4; }
    if (!strncmp(value, "false", 5)) { item->type = cJSON_False; return value + 5; }
    if (!strncmp(value, "true", 4))  { item->type = cJSON_True; item->valueint = 1; return value + 4; }
    if (*value == '\"')              { return parse_string(item, value); }
    if (*value == '-' || (*value >= '0' && *value <= '9')) { return parse_number(item, value); }
    if (*value == '[')              { return parse_array(item, value); }
    if (*value == '{')              { return parse_object(item, value); }
    return NULL;
}

cJSON *cJSON_Parse(const char *value)
{
    cJSON *c = cJSON_New_Item();
    if (!c) return NULL;
    if (!parse_value(c, skip(value))) {
        cJSON_Delete(c);
        return NULL;
    }
    return c;
}

cJSON *cJSON_GetObjectItem(const cJSON * const object, const char * const string)
{
    cJSON *c = object ? object->child : NULL;
    while (c && c->string && strcasecmp(c->string, string)) c = c->next;
    return c;
}

cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON * const object, const char * const string)
{
    cJSON *c = object ? object->child : NULL;
    while (c && c->string && strcmp(c->string, string)) c = c->next;
    return c;
}

int cJSON_HasObjectItem(const cJSON *object, const char *string)
{
    return cJSON_GetObjectItemCaseSensitive(object, string) ? 1 : 0;
}

int cJSON_GetArraySize(const cJSON *array)
{
    cJSON *c = array ? array->child : NULL;
    int i = 0;
    while (c) { i++; c = c->next; }
    return i;
}

cJSON *cJSON_GetArrayItem(const cJSON *array, int index)
{
    cJSON *c = array ? array->child : NULL;
    while (c && index > 0) { index--; c = c->next; }
    return c;
}

cJSON *cJSON_CreateNull(void)   { cJSON *item = cJSON_New_Item(); if(item) item->type = cJSON_NULL; return item; }
cJSON *cJSON_CreateTrue(void)   { cJSON *item = cJSON_New_Item(); if(item) { item->type = cJSON_True; item->valueint = 1; } return item; }
cJSON *cJSON_CreateFalse(void)  { cJSON *item = cJSON_New_Item(); if(item) item->type = cJSON_False; return item; }
cJSON *cJSON_CreateBool(int b)  { return b ? cJSON_CreateTrue() : cJSON_CreateFalse(); }
cJSON *cJSON_CreateNumber(double num)
{
    cJSON *item = cJSON_New_Item();
    if(item) {
        item->type = cJSON_Number;
        item->valuedouble = num;
        item->valueint = (int)num;
    }
    return item;
}

cJSON *cJSON_CreateString(const char *string)
{
    cJSON *item = cJSON_New_Item();
    if(item) {
        item->type = cJSON_String;
        item->valuestring = cJSON_strdup(string);
    }
    return item;
}

cJSON *cJSON_CreateArray(void)  { cJSON *item = cJSON_New_Item(); if(item) item->type = cJSON_Array; return item; }
cJSON *cJSON_CreateObject(void) { cJSON *item = cJSON_New_Item(); if(item) item->type = cJSON_Object; return item; }

void cJSON_AddItemToArray(cJSON *array, cJSON *item)
{
    cJSON *c;
    if (!item) return;
    if (!array->child) {
        array->child = item;
    } else {
        c = array->child;
        while (c && c->next) c = c->next;
        c->next = item;
        item->prev = c;
    }
}

void cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item)
{
    if (!item) return;
    if (item->string) cJSON_free(item->string);
    item->string = cJSON_strdup(string);
    cJSON_AddItemToArray(object, item);
}

void cJSON_AddItemToObjectCS(cJSON *object, const char *string, cJSON *item)
{
    if (!item) return;
    if (!(item->type & cJSON_StringIsConst) && item->string) cJSON_free(item->string);
    item->string = (char*)string;
    item->type |= cJSON_StringIsConst;
    cJSON_AddItemToArray(object, item);
}

/* Serializer */
typedef struct {
    char *buffer;
    size_t length;
    size_t offset;
} printbuffer;

static char* ensure(printbuffer *p, size_t needed)
{
    if (!p || !p->buffer) return NULL;
    if (p->offset + needed > p->length) {
        size_t newsize = (p->length + needed) * 2;
        char *newbuffer = (char*)realloc(p->buffer, newsize);
        if (!newbuffer) return NULL;
        p->buffer = newbuffer;
        p->length = newsize;
    }
    return p->buffer + p->offset;
}

static int print_value_unformatted(const cJSON *item, printbuffer *p);

static int print_string_ptr(const char *str, printbuffer *p)
{
    const char *ptr;
    size_t len = 0;
    if (!str) {
        if (!ensure(p, 3)) return 0;
        memcpy(p->buffer + p->offset, "\"\"", 2);
        p->offset += 2;
        p->buffer[p->offset] = 0;
        return 1;
    }
    for (ptr = str; *ptr; ptr++) {
        if (*ptr == '\"' || *ptr == '\\') len += 2;
        else len++;
    }
    if (!ensure(p, len + 3)) return 0;
    p->buffer[p->offset++] = '\"';
    for (ptr = str; *ptr; ptr++) {
        if (*ptr == '\"' || *ptr == '\\') {
            p->buffer[p->offset++] = '\\';
        }
        p->buffer[p->offset++] = *ptr;
    }
    p->buffer[p->offset++] = '\"';
    p->buffer[p->offset] = 0;
    return 1;
}

static int print_number(const cJSON *item, printbuffer *p)
{
    char str[64];
    double d = item->valuedouble;
    if (fabs(((double)item->valueint) - d) <= 1e-6 && d <= INT_MAX && d >= INT_MIN) {
        snprintf(str, sizeof(str), "%d", item->valueint);
    } else {
        snprintf(str, sizeof(str), "%f", d);
    }
    size_t len = strlen(str);
    if (!ensure(p, len + 1)) return 0;
    memcpy(p->buffer + p->offset, str, len);
    p->offset += len;
    p->buffer[p->offset] = 0;
    return 1;
}

static int print_array_unformatted(const cJSON *item, printbuffer *p)
{
    cJSON *child = item->child;
    if (!ensure(p, 2)) return 0;
    p->buffer[p->offset++] = '[';
    while (child) {
        if (!print_value_unformatted(child, p)) return 0;
        if (child->next) {
            if (!ensure(p, 2)) return 0;
            p->buffer[p->offset++] = ',';
        }
        child = child->next;
    }
    if (!ensure(p, 2)) return 0;
    p->buffer[p->offset++] = ']';
    p->buffer[p->offset] = 0;
    return 1;
}

static int print_object_unformatted(const cJSON *item, printbuffer *p)
{
    cJSON *child = item->child;
    if (!ensure(p, 2)) return 0;
    p->buffer[p->offset++] = '{';
    while (child) {
        if (!print_string_ptr(child->string, p)) return 0;
        if (!ensure(p, 2)) return 0;
        p->buffer[p->offset++] = ':';
        if (!print_value_unformatted(child, p)) return 0;
        if (child->next) {
            if (!ensure(p, 2)) return 0;
            p->buffer[p->offset++] = ',';
        }
        child = child->next;
    }
    if (!ensure(p, 2)) return 0;
    p->buffer[p->offset++] = '}';
    p->buffer[p->offset] = 0;
    return 1;
}

static int print_value_unformatted(const cJSON *item, printbuffer *p)
{
    if (!item) return 0;
    switch (item->type & 0xFF) {
        case cJSON_NULL:
            if (!ensure(p, 5)) return 0;
            memcpy(p->buffer + p->offset, "null", 4);
            p->offset += 4;
            p->buffer[p->offset] = 0;
            return 1;
        case cJSON_False:
            if (!ensure(p, 6)) return 0;
            memcpy(p->buffer + p->offset, "false", 5);
            p->offset += 5;
            p->buffer[p->offset] = 0;
            return 1;
        case cJSON_True:
            if (!ensure(p, 5)) return 0;
            memcpy(p->buffer + p->offset, "true", 4);
            p->offset += 4;
            p->buffer[p->offset] = 0;
            return 1;
        case cJSON_Number:
            return print_number(item, p);
        case cJSON_String:
            return print_string_ptr(item->valuestring, p);
        case cJSON_Array:
            return print_array_unformatted(item, p);
        case cJSON_Object:
            return print_object_unformatted(item, p);
    }
    return 0;
}

char *cJSON_PrintUnformatted(const cJSON *item)
{
    printbuffer p;
    p.buffer = (char*)malloc(256);
    if (!p.buffer) return NULL;
    p.length = 256;
    p.offset = 0;
    if (!print_value_unformatted(item, &p)) {
        free(p.buffer);
        return NULL;
    }
    return p.buffer;
}

char *cJSON_Print(const cJSON *item)
{
    return cJSON_PrintUnformatted(item);
}
