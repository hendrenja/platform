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

/** @file
 * @section buffer Buffer API
 * @brief Utility API for efficient appending to strings.
 */

#ifndef CORTO_BUFFER_H_
#define CORTO_BUFFER_H_

#ifdef __cplusplus
extern "C" {
#endif

#define CORTO_BUFFER_INIT (corto_buffer){NULL, 0, 0}
#define CORTO_BUFFER_ELEMENT_SIZE (511)

/* A buffer builds up a list of elements which individually can be up to N bytes
 * large. While appending, data is added to these elements. More elements are
 * added on the fly when needed. When an application calls corto_buffer_str, all
 * elements are combined in one string and the element administration is freed.
 *
 * This approach prevents reallocs of large blocks of memory, and therefore
 * copying large blocks of memory when appending to a large buffer. A buffer
 * preallocates some memory for the element overhead so that for small strings
 * there is hardly any overhead, while for large strings the overhead is offset
 * by the reduced time spent on copying memory.
 */

typedef struct corto_buffer_element {
    bool buffer_embedded;
    uint32_t pos;
    char *buf;
    struct corto_buffer_element *next;
} corto_buffer_element;

typedef struct corto_buffer_element_embedded {
    corto_buffer_element super;
    char buf[CORTO_BUFFER_ELEMENT_SIZE + 1];
} corto_buffer_element_embedded;

typedef struct corto_buffer_element_str {
    corto_buffer_element super;
    char *alloc_str;
} corto_buffer_element_str;

typedef struct corto_buffer {
    /* When set by an application, append will write to this buffer */
    char *buf;

    /* The maximum number of characters that may be printed */
    uint32_t max;

    /* Size of elements minus current element */
    uint32_t size;

    /* The number of elements in use */
    uint32_t elementCount;

    /* Always allocate at least one element */
    corto_buffer_element_embedded firstElement;

    /* The current element being appended to */
    corto_buffer_element *current;
} corto_buffer;

/* Append format string to a buffer.
 * Returns false when max is reached, true when there is still space */
CORTO_EXPORT
bool corto_buffer_append(
    corto_buffer *buffer,
    const char *fmt,
    ...);

/* Append format string with argument list to a buffer.
 * Returns false when max is reached, true when there is still space */
CORTO_EXPORT
bool corto_buffer_vappend(
    corto_buffer *buffer,
    const char *fmt,
    va_list args);

/* Append string to buffer.
 * Returns false when max is reached, true when there is still space */
CORTO_EXPORT
bool corto_buffer_appendstr(
    corto_buffer *buffer,
    const char *str);

/* Append string to buffer, transfer ownership to buffer.
 * Returns false when max is reached, true when there is still space */
CORTO_EXPORT
bool corto_buffer_appendstr_zerocpy(
    corto_buffer *buffer,
    char *str);

/* Append string to buffer, do not free/modify string.
 * Returns false when max is reached, true when there is still space */
CORTO_EXPORT
bool corto_buffer_appendstr_zerocpy_const(
    corto_buffer *buffer,
    const char *str);

/* Append n characters to buffer.
 * Returns false when max is reached, true when there is still space */
CORTO_EXPORT
bool corto_buffer_appendstrn(
    corto_buffer *buffer,
    const char *str,
    uint32_t n);

/* Return result string (also resets buffer) */
CORTO_EXPORT
char *corto_buffer_str(corto_buffer *buffer);

/* Reset buffer without returning a string */
CORTO_EXPORT
void corto_buffer_reset(corto_buffer *buffer);


#ifdef __cplusplus
}
#endif

#endif
