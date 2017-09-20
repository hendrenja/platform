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

int corto_symlink(
    const char *oldname,
    const char *newname) 
{
    char *fullname = NULL;
    if (oldname[0] != '/') {
        fullname = corto_asprintf("%s/%s", corto_cwd(), oldname);
        corto_path_clean(fullname, fullname);
    } else {
        /* Safe- the variable will not be modified if it's equal to newname */
        fullname = (char*)oldname;
    }

    if (symlink(fullname, newname)) {

        if (errno == ENOENT) {
            /* If error is ENOENT, try creating directory */
            char *dir = corto_path_dirname(newname);
            int old_errno = errno;

            if (dir[0] && !corto_mkdir(dir)) {
                /* Retry */
                if (corto_symlink(fullname, newname)) {
                    goto error;
                }
            } else {
                printError(old_errno, newname);
            }
            free(dir);

        } else if (errno == EEXIST) {
            /* If a file with specified name already exists, remove existing file */
            if (corto_rm(newname)) {
                goto error;
            }

            /* Retry */
            if (corto_symlink(fullname, newname)) {
                goto error;
            }
        }
    }

    if (fullname != oldname) free(fullname);
    return 0;
error:
    if (fullname != oldname) free(fullname);
    corto_seterr("symlink: %s", corto_lasterr());
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
    } else {
        printError(errno, name);
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

static
bool corto_dir_hasNext(
    corto_iter *it) 
{
    struct dirent *ep = readdir(it->ctx);
    while (ep && *ep->d_name == '.') {
        ep = readdir(it->ctx);
    }

    if (ep) {
        it->data = ep->d_name;
    }

    return ep ? true : false;
}

static
void* corto_dir_next(
    corto_iter *it) 
{
    return it->data;
}

static
void corto_dir_release(
    corto_iter *it) 
{
    closedir(it->ctx);
}

int16_t corto_dir_iter(
    const char *name, 
    corto_iter *it_out)
{
    corto_iter result = {
        .ctx = opendir(name),
        .data = NULL,
        .hasNext = corto_dir_hasNext,
        .next = corto_dir_next,
        .release = corto_dir_release
    };

    if (!result.ctx) {
        printError(errno, name);
        goto error;
    }

    *it_out = result;

    return 0;
error:
    corto_seterr("dir_iter: %s", corto_lasterr());
    return -1;
}

bool corto_dir_isEmpty(
    const char *name)
{
    corto_iter it;
    if (corto_dir_iter(name, &it)) {
        return true; /* If dir can't be opened, it might as well be empty */
    }

    bool isEmpty = !corto_iter_hasNext(&it);
    corto_iter_release(&it); /* clean up resources */
    return isEmpty;
}

corto_dirstack corto_dirstack_push(
    corto_dirstack stack,
    const char *dir)
{
    if (!stack) {
        stack = corto_ll_new();
    }

    corto_ll_append(stack, strdup(corto_cwd()));

    if (corto_chdir(dir)) {
        goto error;
    }

    return stack;
error:
    corto_seterr("dirstack_push: %s", corto_lasterr());
    return NULL;
}

int16_t corto_dirstack_pop(
    corto_dirstack stack)
{
    char *dir = corto_ll_takeLast(stack);

    if (corto_chdir(dir)) {
        goto error;
    }

    return 0;
error:
    corto_seterr("dirstack_pop: %s", corto_lasterr());
    return -1;
}

const char* corto_dirstack_wd(
    corto_dirstack stack)
{
    if (!stack || corto_ll_size(stack) == 1) {
        return ".";
    }

    char *first = corto_ll_get(stack, 0);
    char *last = corto_ll_last(stack);
    char *result = last - strlen(first);
    if (result[0] == '/') result ++;

    return result;
}

