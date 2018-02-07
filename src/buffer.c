/* Copyright (c) 2010-2018 the corto developers
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <corto/platform.h>

/* Add an extra element to the buffer */
static
void corto_buffer_grow(
    corto_buffer *b)
{
    /* Allocate new element */
    corto_buffer_element_embedded *e =
        corto_alloc(sizeof(corto_buffer_element_embedded));
    b->size += b->current->pos;
    b->current->next = (corto_buffer_element*)e;
    b->current = (corto_buffer_element*)e;
    b->elementCount ++;
    e->super.buffer_embedded = true;
    e->super.buf = e->buf;
    e->super.pos = 0;
    e->super.next = NULL;
}

/* Add an extra dynamic element */
static
void corto_buffer_grow_str(
    corto_buffer *b,
    char *str,
    char *alloc_str,
    uint32_t size)
{
    /* Allocate new element */
    corto_buffer_element_str *e =
        corto_alloc(sizeof(corto_buffer_element_str));
    b->size += b->current->pos;
    b->current->next = (corto_buffer_element*)e;
    b->current = (corto_buffer_element*)e;
    b->elementCount ++;
    e->super.buffer_embedded = false;
    e->super.pos = size ? size : strlen(str);
    e->super.next = NULL;
    e->super.buf = str;
    e->alloc_str = alloc_str;
}

static char* corto_buffer_ptr(corto_buffer *b) {
    if (b->buf) {
        return &b->buf[b->current->pos];
    } else {
        return &b->current->buf[b->current->pos];
    }
}

/* Compute the amount of space left in the current element */
static int32_t corto_buffer_memLeftInCurrentElement(corto_buffer *b) {
    if (b->current->buffer_embedded) {
        return CORTO_BUFFER_ELEMENT_SIZE - b->current->pos;
    } else {
        return 0;
    }
}

/* Compute the amount of space left */
static int32_t corto_buffer_memLeft(corto_buffer *b) {
    if (b->max) {
        return b->max - b->size - b->current->pos;
    } else {
        return INT_MAX;
    }
}

static void corto_buffer_init(corto_buffer *b) {
    /* Initialize buffer structure only once */
    if (!b->elementCount) {
        b->firstElement.super.next = NULL;
        b->firstElement.super.pos = 0;
        b->firstElement.super.buffer_embedded = true;
        b->firstElement.super.buf = b->firstElement.buf;
        b->elementCount ++;
        b->current = (corto_buffer_element*)&b->firstElement;
    }
}

/* Quick custom function to copy a maxium number of characters and
 * simultaneously determine length of source string. */
static
unsigned int fast_strncpy(
    char * dst,
    const char * src,
    unsigned int n_cpy,
    unsigned int n)
{
    const char *ptr, *orig = src;
    char ch;

    for (ptr = src; (ptr - orig < n) && (ch = *ptr); ptr ++) {
        if (ptr - orig < n_cpy) {
            *dst = ch;
            dst ++;
        }
    }

    return ptr - orig;
}

/* Append a format string to a buffer */
static bool corto_buffer_append_intern(
    corto_buffer *b,
    const char* str,
    unsigned int n,
    bool fmt_string,
    va_list args)
{
    bool result = TRUE;
    va_list arg_cpy;

    if (!str) {
        return result;
    }

    corto_buffer_init(b);

    int32_t memLeftInElement = corto_buffer_memLeftInCurrentElement(b);
    int32_t memLeft = corto_buffer_memLeft(b);

    if (!memLeft) {
        return false;
    }

    /* Compute the memory required to add the string to the buffer. If user
     * provided buffer, use space left in buffer, otherwise use space left in
     * current element. */
    int32_t max_copy = b->buf ? memLeft : memLeftInElement;
    int32_t memRequired;

    if (!n) n = INT_MAX;

    if (!fmt_string) {
        memRequired = fast_strncpy(corto_buffer_ptr(b), str, max_copy, n);
    } else {
        va_copy(arg_cpy, args);
        memRequired = vsnprintf(corto_buffer_ptr(b), max_copy + 1, str, args);
    }

    if (memRequired <= memLeftInElement) {
        /* Element was large enough to fit string */
        b->current->pos += memRequired;
    } else if ((memRequired - memLeftInElement) < memLeft) {
        /* Element was not large enough, but buffer still has space */
        if (!fmt_string) {
            b->current->pos += memLeftInElement;
            memRequired -= memLeftInElement;

            /* Current element was too small, copy remainder into new element */
            if (memRequired < CORTO_BUFFER_ELEMENT_SIZE) {
                /* A standard-size buffer is large enough for the new string */
                corto_buffer_grow(b);

                /* Copy the remainder to the new buffer */
                if (n) {
                    /* If a max number of characters to write is set, only a
                     * subset of the string should be copied to the buffer */
                    strncpy(
                        corto_buffer_ptr(b),
                        str + memLeftInElement,
                        memRequired);
                } else {
                    strcpy(corto_buffer_ptr(b), str + memLeftInElement);
                }

                /* Update to number of characters copied to new buffer */
                b->current->pos += memRequired;
            } else {
                char *remainder = corto_strdup(str + memLeftInElement);
                corto_buffer_grow_str(b, remainder, remainder, memRequired);
            }
        } else {
            /* If string is a format string, a new buffer of size memRequired is
             * needed to re-evaluate the format string and only use the part that
             * wasn't already copied to the previous element */
            if (memRequired <= CORTO_BUFFER_ELEMENT_SIZE) {
                /* Resulting string fits in standard-size buffer. Note that the
                 * entire string needs to fit, not just the remainder, as the
                 * format string cannot be partially evaluated */
                corto_buffer_grow(b);

                /* Copy entire string to new buffer */
                vsprintf(corto_buffer_ptr(b), str, arg_cpy);

                /* Ignore the part of the string that was copied into the
                 * previous buffer. The string copied into the new buffer could
                 * be memmoved so that only the remainder is left, but that is
                 * most likely more expensive than just keeping the entire
                 * string. */

                /* Update position in buffer */
                b->current->pos += memRequired;
            } else {
                /* Resulting string does not fit in standard-size buffer.
                 * Allocate a new buffer that can hold the entire string. */
                char *dst = corto_alloc(memRequired + 1);
                vsprintf(dst, str, arg_cpy);
                corto_buffer_grow_str(b, dst, dst, memRequired);
            }
        }
    } else {
        /* Buffer max has been reached */
        result = false;
    }

    if (fmt_string) {
        va_end(arg_cpy);
    }

    return result;
}

bool corto_buffer_vappend(
    corto_buffer *b,
    const char* fmt,
    va_list args)
{
    bool result = corto_buffer_append_intern(
        b, fmt, 0, true, args
    );

    return result;
}

bool corto_buffer_append(
    corto_buffer *b,
    const char* fmt,
    ...)
{
    va_list args;
    va_start(args, fmt);
    bool result = corto_buffer_append_intern(
        b, fmt, 0, true, args
    );
    va_end(args);

    return result;
}

bool corto_buffer_appendstrn(
    corto_buffer *b,
    const char* str,
    uint32_t len)
{
    return corto_buffer_append_intern(
        b, str, len, false, NULL
    );
}

bool corto_buffer_appendstr_zerocpy(
    corto_buffer *b,
    char* str)
{
    corto_buffer_init(b);
    corto_buffer_grow_str(b, str, str, 0);
    return true;
}

bool corto_buffer_appendstr_zerocpy_const(
    corto_buffer *b,
    const char* str)
{
    /* Removes const modifier, but logic prevents changing / delete string */
    corto_buffer_init(b);
    corto_buffer_grow_str(b, (char*)str, NULL, 0);
    return true;
}


bool corto_buffer_appendstr(
    corto_buffer *b,
    const char* str)
{
    return corto_buffer_append_intern(
        b, str, 0, false, NULL
    );
}

char* corto_buffer_str(corto_buffer *b) {
    char* result = NULL;

    if (b->elementCount) {
        if (b->buf) {
            result = corto_strdup(b->buf);
        } else {
            void *next = NULL;
            uint32_t len = b->size + b->current->pos + 1;

            corto_buffer_element *e = (corto_buffer_element*)&b->firstElement;

            result = corto_alloc(len);
            char* ptr = result;

            do {
                memcpy(ptr, e->buf, e->pos);
                ptr += e->pos;
                next = e->next;
                if (e != &b->firstElement.super) {
                    if (!e->buffer_embedded) {
                        free(((corto_buffer_element_str*)e)->alloc_str);
                    }
                    corto_dealloc(e);
                }
            } while ((e = next));

            result[len - 1] = '\0';
        }
    } else {
        result = NULL;
    }

    b->elementCount = 0;

    return result;
}

void corto_buffer_reset(corto_buffer *b) {
    if (b->elementCount && !b->buf) {
        void *next = NULL;
        corto_buffer_element *e = (corto_buffer_element*)&b->firstElement;
        do {
            next = e->next;
            if (e != (corto_buffer_element*)&b->firstElement) {
                corto_dealloc(e);
            }
        } while ((e = next));
    }

    *b = CORTO_BUFFER_INIT;
}
