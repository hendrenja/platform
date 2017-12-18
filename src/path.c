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

#include <corto/base.h>

static
char *__strsep(char **str, char delim) {
    char *result = *str;
    if (result) {
        char *ptr = strchr(result, delim);
        if (ptr) {
            *ptr = 0;
            (*str) = ptr + 1;
        } else {
            *str = NULL;
        }
    }
    return result;
}

char* corto_path_clean(char *buf, char *path) {
    char work[CORTO_MAX_PATH_LENGTH], tempbuf[CORTO_MAX_PATH_LENGTH];
    char *cp, *thisp, *nextp = work;
    bool threadStr = false;
    bool equalBuf = true;

    if (!buf) {
        buf = corto_alloc(CORTO_MAX_PATH_LENGTH);
        threadStr = true;
    } else if (buf == path) {
        equalBuf = true;
        buf = tempbuf;
    }

    cp = strchr(path, '/');

    /* no '/' characters - return as-is */
    if (cp == NULL) {
        return path;
    }

    /* copy leading slash if present */
    if (cp == path) {
        strcpy(buf, "/");
    } else {
        buf[0] = '\0';
    }

    /* tokenization */
    strcpy(work, path);
    while ((thisp = __strsep(&nextp, '/')) != NULL) {
        if (*thisp == '\0') continue;

        if (strcmp(thisp, ".") == 0) continue;

        if (strcmp(thisp, "..") == 0) {
            cp = strrchr(buf, '/');

             /* "/" or "/foo" */
            if (cp == buf) {
                buf[1] = '\0';
                continue;
            }

            /* "..", "foo", or "" */
            else if (cp == NULL) {
                if (buf[0] != '\0' && strcmp(buf, "..") != 0) {
                    buf[0] = '\0';
                    continue;
                }
            }
            /* ".../foo" */
            else {
                *cp = '\0';
                continue;
            }
        }

        if (buf[0] != '\0' && buf[strlen(buf) - 1] != '/') {
            strcat(buf, "/");
        }

        strcat(buf, thisp);
    }

    if (buf[0] == '\0') strcpy(buf, ".");

    if (threadStr) {
        char *tmp = buf;
        path = corto_setThreadString(buf);
        corto_dealloc(tmp);
    } else if (equalBuf) {
        strcpy(path, buf);
    } else {
        path = buf;
    }

    return path;
}

char* corto_path_dirname(
    const char *path)
{
    char *result = strdup(path);

    char *ptr = strrchr(result, '/');
    if (ptr) {
        ptr[0] = '\0';
    } else {
        result = strdup("");
    }

    return result;
}
