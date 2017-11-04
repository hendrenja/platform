
/* Copyright (c) 2010-2017 the corto developers
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

#include <corto/base.h>

static 
void printError(
    int e, 
    const char *ctx) 
{
    corto_throw("%s '%s'", strerror(e), ctx);
}

FILE* corto_file_open(
    const char* filename,
    const char* mode)
{
    FILE *result = fopen(filename, mode);

    if (!result && (strchr(mode, 'a') || strchr(mode, 'w'))) {
        if (errno == ENOENT) {
            char *dir = corto_path_dirname(filename);
            if (corto_mkdir(dir)) {
                free(dir);
                goto error;
            }
            free(dir);

            /* Retry */
            if (!(result = fopen(filename, mode))) {
                printError(errno, filename);
                goto error;
            }
        }
    }

    return result;
error:
    return NULL;
}

char* corto_file_load(
    const char* filename) 
{
    FILE* file;
    char* content;
    int size;

    /* Open file for reading */
    file = fopen(filename, "r");
    if (!file) {
        corto_throw("%s (%s)", strerror(errno), filename);
        goto error;
    }

    /* Determine file size */
    fseek (file, 0 , SEEK_END);
    size = ftell (file);
    if (size == -1) {
        goto error;
    }
    rewind(file);

    /* Load contents in memory */
    content = malloc (size + 1);
    if (fread(content, 1, size, file) != (unsigned int)size) {
        corto_dealloc(content);
        content = NULL;
    } else {
        content[size] = '\0';
    }

    fclose(file);

    return content;
error:
    return NULL;
}

int16_t corto_file_test(
    const char* filefmt, 
    ...) 
{
    FILE* exists = NULL;
    va_list arglist;

    va_start(arglist, filefmt);
    char *file = corto_venvparse(filefmt, arglist);
    va_end(arglist);

    if (file) {
        errno = 0;
        exists = fopen(file, "rb");
        if (exists) {
            fclose(exists);
        } else if ((errno != ENOENT) && (errno != ENOTDIR)) {
            corto_throw("%s: %s", file, strerror(errno));
            return -1;
        } else {
            errno = 0;
        }
    }

    corto_dealloc(file);

    return (exists != 0) ? true : false;
}

/* Get file size */
unsigned int corto_file_size(
    FILE* file) 
{
    unsigned int size;

    /* Determine file size */
    fseek (file, 0 , SEEK_END);
    size = ftell (file);
    rewind(file);

    return size;
}

/* Read line from file */
char* corto_file_readln(
    FILE* file, 
    char* buf, 
    unsigned int length) 
{
    int c;
    char* ptr;

    if (!file) {
        return 0;
    }

    c = EOF;
    ptr = buf;

    while (!feof(file)) {

        c = fgetc(file);
        if ((c == '\n') || (c == EOF)) {
            break;
        }

        *ptr = c;
        ptr++;
        if ((unsigned int)(ptr - buf) == (length - 1)) {
            break;
        }
    }

    *ptr = '\0';

    return (c == EOF) ? 0 : buf;
}

static
bool corto_file_hasNext(
    corto_iter *it)
{
    return !feof(it->ctx);
}

static
void* corto_file_next(
    corto_iter *it)
{
    corto_buffer buf = CORTO_BUFFER_INIT;

    while (!feof(it->ctx)) {
        char c = fgetc(it->ctx);
        if (c == '\n' || c == EOF) {
            break;
        }
        
        corto_buffer_appendstrn(&buf, &c, 1);
    }

    if (it->data) free(it->data);
    it->data = corto_buffer_str(&buf);
    return it->data;
}

static
void corto_file_release(
    corto_iter *it)
{
    if (it->data) free(it->data);
    if (it->ctx) fclose(it->ctx);
}

int16_t corto_file_iter(
    char *filename,
    corto_iter *iter_out)
{
    FILE *f = fopen(filename, "r");
    if (!f) {
        printError(errno, filename);
        goto error;
    }

    iter_out->ctx = f;
    iter_out->data = NULL;
    iter_out->hasNext = corto_file_hasNext;
    iter_out->next = corto_file_next;
    iter_out->release = corto_file_release;

    return 0;
error:
    return -1;
}

/* Get file extension */
char* corto_file_extension(
    char* file, 
    char* buffer) 
{
    char *ptr, *bptr;
    char ch;

    bptr = buffer;

    ptr = file + strlen(file);
    while (ptr >= file) {
        if (*ptr == '.') break;
        ptr--;
    }
    if (ptr >= file) {
        ptr++;
        while ((ch = *ptr) && ((bptr - buffer) < CORTO_MAX_FILE_EXTENSION)) {
            *bptr = ch;
            ptr++;
            bptr++;
        }
    }
    if ((bptr - buffer) >= CORTO_MAX_FILE_EXTENSION) {
        return NULL;
    } else {
        *bptr = '\0';
        return buffer;
    }
}

/* Get file base */
char* corto_file_base(char* file, char* buffer) {
    strcpy(buffer, file);
    char *ch = strrchr(buffer, '.');
    if (ch) {
        *ch = '\0';
    }
    return buffer;
}

/* Get file path */
char* corto_file_path(char* file, char* buffer) {
    int32_t i;

    strcpy(buffer, file);
    i = strlen(buffer);
    while (i >= 0) {
        if ((buffer[i] == '/') || (buffer[i] == '\\')) {
            buffer[i] = '\0';
            break;
        }
        i--;
    }

    return (i != -1) ? buffer : NULL;
}

