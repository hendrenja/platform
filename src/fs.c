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

#include <include/base.h>

/*
 * Receives:
 * - the error code
 * - an initial message
 * The name for the error code will be appended.
 */
static void printError(int e, const char *ctx) {
    corto_seterr("%s '%s'", strerror(e), ctx);
}

int corto_touch(const char *file) {
    FILE* touch = NULL;

    if (file) {
        touch = fopen(file, "ab");
        if (touch) {
            fclose(touch);
        }
    }

    return touch ? 0 : -1;
}

int corto_chdir(const char *dir) {
    if (chdir(dir)) {
        corto_seterr("%s '%s'", strerror(errno), dir);
        return -1;
    }
    return 0;
}

char* corto_cwd(void) {
    corto_id cwd;
    if (getcwd(cwd, sizeof(cwd))) {
        return corto_setThreadString(cwd);
    } else {
        corto_seterr("%s", strerror(errno));
        return NULL;
    }
}

int corto_mkdir(const char *fmt, ...) {
    int _errno = 0;
    char msg[PATH_MAX];
    
    va_list args;
    va_start(args, fmt);
    char *name = corto_venvparse(fmt, args);
    va_end(args);
    if (!name) {
        goto error_name;
    }

    if (mkdir(name, 0755)) {
        _errno = errno;

        /* If error is ENOENT an element in the prefix of the name
         * doesn't exist. Recursively create pathname, and retry. */
        if (errno == ENOENT) {
            /* Allocate so as to not run out of stackspace in recursive call */
            char *prefix = corto_strdup(name);
            char *ptr = &prefix[strlen(prefix)-1], ch;
            while ((ch = *ptr) && (ptr >= prefix)) {
                if (ch == '/') {
                    *ptr = '\0';
                    break;
                }
                ptr--;
            }
            if (ch == '/') {
                if (!corto_mkdir(prefix)) {
                    /* Retry current directory */
                    if (!mkdir(name, 0755)) {
                        _errno = 0;
                    } else {
                        _errno = errno;
                    }
                } else {
                    goto error;
                }
            } else {
                goto error; /* If no prefix is found, report error */
            }
            corto_dealloc(prefix);
        }

        /* Post condition for function is that directory exists so don't
         * report an error if it already did. */
        if (_errno && (_errno != EEXIST)) {
            goto error;
        }
    }

    corto_dealloc(name);

    return 0;
error:
    sprintf(msg, "%s", name);
    printError(errno, msg);
    corto_dealloc(name);
error_name:
    return -1;
}

int corto_cp(const char *sourcePath, const char *destinationPath) {
    int _errno = 0;
    char msg[PATH_MAX];
    FILE *destinationFile;
    FILE *sourceFile;

    if (!(sourceFile = fopen(sourcePath, "rb"))) {
        _errno = errno;
        sprintf(msg, "cannot open sourcefile '%s'", sourcePath);
        goto error;
    }

    if (!(destinationFile = fopen(destinationPath, "wb"))) {
        /* If destination is a directory, append filename to directory and try
         * again */
        if (errno == EISDIR) {
            corto_id dest;
            const char *fileName = strrchr(sourcePath, '/');
            if (!fileName) {
                fileName = sourcePath;
            }
            sprintf(dest, "%s/%s", destinationPath, fileName);
            if (!(destinationFile = fopen(dest, "wb"))) {
                _errno = errno;
                sprintf(msg, "cannot open destinationfile '%s'", dest);
                fclose(sourceFile);
                goto error;
            }
        } else {
            _errno = errno;
            sprintf(msg, "cannot open destinationfile '%s'", destinationPath);
            fclose(sourceFile);
            goto error;
        }
    }

    /* "no real standard portability"
     * http://www.cplusplus.com/reference/cstdio/fseek/ */
    if (fseek(sourceFile, 0, SEEK_END)) {
        _errno = errno;
        sprintf(msg, "cannot traverse file '%s'", sourcePath);
        goto error_CloseFiles;
    }

    long fileSizeResult;
    size_t fileSize;
    fileSizeResult = ftell(sourceFile);
    if (fileSizeResult == -1) {
        sprintf(msg, "cannot obtain filesize from file %s", sourcePath);
        goto error_CloseFiles;
    }
    /* Now we can be sure that fileSizeResult doesn't contain a
     * negative value */
    fileSize = fileSizeResult;

    rewind(sourceFile);

    char *buffer = corto_alloc(fileSize);
    if (!buffer) {
        _errno = 0;
        sprintf(msg, "cannot allocate buffer for copying files");
        goto error_CloseFiles;
    }

    if (fread(buffer, 1, fileSize, sourceFile) != fileSize) {
        _errno = 0;
        sprintf(msg, "cannot read the file %s", sourcePath);
        goto error_CloseFiles_FreeBuffer;
    }

    if (fwrite(buffer, 1, fileSize, destinationFile) != fileSize) {
        _errno = 0;
        sprintf(msg, "cannot write to the file %s", destinationPath);
        goto error_CloseFiles_FreeBuffer;
    }

    corto_dealloc(buffer);
    fclose(sourceFile);
    fclose(destinationFile);

    return 0;

error_CloseFiles_FreeBuffer:
    free(buffer);
error_CloseFiles:
    fclose(sourceFile);
    fclose(destinationFile);
error:
    printError(_errno, msg);
    return -1;
}

/* Test if name is a directory */
bool corto_isdir(const char *path) {
    struct stat buff;
    if (stat(path, &buff) != 0) {
        return 0;
    }
    return S_ISDIR(buff.st_mode) ? true : false;
}

int corto_rename(const char *oldName, const char *newName) {
    if (rename(oldName, newName)) {
        corto_seterr(strerror(errno));
        goto error;
    }
    return 0;
error:
    return -1;
}

/* Remove a file. Returns 0 if OK, -1 if failed */
int corto_rm(const char *name) {
    int result = 0;

    if (corto_isdir(name)) {
        return corto_rmtree(name);
    } else if (remove(name)) {
        /* Don't care if file didn't exist since the postcondition
         * is that file doesn't exist. */
        if (errno != EEXIST) {
            result = -1;
            corto_seterr(strerror(errno));
        }
    }

    return result;
}

static int corto_rmtreeCallback(
  const char *path,
  const struct stat *sb,
  int typeflag,
  struct FTW *ftwbuf)
{
    CORTO_UNUSED(sb);
    CORTO_UNUSED(typeflag);
    CORTO_UNUSED(ftwbuf);
    if (remove(path)) {
        corto_seterr(strerror(errno));
        goto error;
    }
    return 0;
error:
    return -1;
}

/* Recursively remove a directory */
int corto_rmtree(const char *name) {
    return nftw(name, corto_rmtreeCallback, 20, FTW_DEPTH);
}

/* Read the contents of a directory */
corto_ll corto_opendir(const char *name) {
    DIR *dp;
    struct dirent *ep;
    corto_ll result = NULL;

    dp = opendir (name);

    if (dp != NULL) {
        result = corto_ll_new();
        while ((ep = readdir (dp))) {
            if (*ep->d_name != '.') {
                corto_ll_append(result, corto_strdup(ep->d_name));
            }
        }
        closedir (dp);
    }

    return result;
}

void corto_closedir(corto_ll dir) {
    corto_iter iter = corto_ll_iter(dir);

    while(corto_iter_hasNext(&iter)) {
        corto_dealloc(corto_iter_next(&iter));
    }
    corto_ll_free(dir);
}
