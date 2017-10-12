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
#include "idmatch.h"

/*
 * Receives:
 * - the error code
 * - an initial message
 * The name for the error code will be appended.
 */
static void printError(int e, const char *ctx) {
    if (e) corto_seterr("%s: %s", ctx, strerror(e));
    else  corto_seterr((char*)ctx);
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

    corto_trace("mkdir '%s'", name);

    /* Remove a file if it already exists and is not a directory */
    if (corto_file_test(name) && !corto_isdir(name)) {
        if (corto_rm(name)) {
            goto error;
        }
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

static
int corto_cp_file(
    const char *src, 
    const char *dst)
{
    int _errno = 0;
    char msg[PATH_MAX];
    FILE *destinationFile = NULL;
    FILE *sourceFile = NULL;
    char *fullDst = (char*)dst;
    int perm = 0;

    /* If destination is a directory, copy into directory */
    if (corto_file_test(dst) && corto_isdir(dst) && !corto_isdir(src)) {
        const char *base = strrchr(src, '/');
        if (!base) base = src; else base = base + 1;
        fullDst = corto_asprintf("%s/%s", dst, base);
    }

    if (!(sourceFile = fopen(src, "rb"))) {
        corto_seterr("cannot open '%s': %s", src, strerror(errno));
        goto error;
    }

    if (!(destinationFile = fopen(fullDst, "wb"))) {
        corto_seterr("cannot open '%s': %s", fullDst, strerror(errno));
        goto error_CloseFiles;
    }

    if (corto_getperm(src, &perm)) {
        corto_seterr("cannot get permissions from '%s': %s", corto_lasterr());
        goto error_CloseFiles;
    }

    if (fseek(sourceFile, 0, SEEK_END)) {
        corto_seterr("cannot seek '%s': %s", src, strerror(errno));
        goto error_CloseFiles;
    }

    size_t fileSize = ftell(sourceFile);
    if (fileSize == -1) {
        corto_seterr("cannot get size from '%s': %s", src, strerror(errno));
        goto error_CloseFiles;
    }

    rewind(sourceFile);

    char *buffer = corto_alloc(fileSize);

    size_t n;
    if ((n = fread(buffer, 1, fileSize, sourceFile)) != fileSize) {
        corto_seterr("cannot read '%s': %s", src, strerror(errno));
        goto error_CloseFiles_FreeBuffer;
    }

    if (fwrite(buffer, 1, fileSize, destinationFile) != fileSize) {
        corto_seterr("cannot write to '%s': %s", fullDst, strerror(errno));
        goto error_CloseFiles_FreeBuffer;
    }

    if (corto_setperm(fullDst, perm)) {
        corto_seterr("failed to set permissions of '%s': %s", corto_lasterr());
        goto error;
    }

    if (fullDst != dst) free(fullDst);
    corto_dealloc(buffer);
    fclose(sourceFile);
    fclose(destinationFile);

    return 0;

error_CloseFiles_FreeBuffer:
    free(buffer);
error_CloseFiles:
    if (sourceFile) fclose(sourceFile);
    if (destinationFile) fclose(destinationFile);
error:
    return -1;    
}

static
int16_t corto_cp_dir(
    const char *src, 
    const char *dst)
{
    char *lasterr = NULL;

    if (corto_mkdir(dst)) {
        goto error;
    }

    corto_dirstack stack = corto_dirstack_push(NULL, src);
    if (!stack) {
        goto error;
    }

    corto_iter it;

    if (corto_dir_iter(".", NULL, &it)) {
        goto error;
    }

    while (corto_iter_hasNext(&it)) {
        char *file = corto_iter_next(&it);

        if (corto_isdir(file)) {
            /*if (corto_cp_dir(file, dst)) {
                goto error;
            }*/
        } else {
            if (corto_cp_file(file, dst)) {
                goto error;
            }
        }
    }

    if (corto_dirstack_pop(stack)) {
        goto error;
    }

    return 0;
error:
    if (corto_lasterr()) {
        /* In case something goes wrong with the next statement, preserve error */
        lasterr = strdup(corto_lasterr());
    }
    if (corto_dirstack_pop(stack)) {
        if (lasterr) corto_seterr(lasterr);
    }
    if (lasterr) free(lasterr);
    return -1;
}

int16_t corto_cp(
    const char *src, 
    const char *dst) 
{
    int16_t result;

    corto_trace("cp '%s' => '%s'", src, dst);

    corto_log_push("cp");

    if (!corto_file_test(src)) {
        corto_seterr("source '%s' does not exist", src);
        goto error;
    }

    if (corto_isdir(src)) {
        result = corto_cp_dir(src, dst);
    } else {
        result = corto_cp_file(src, dst);
    }

    corto_log_pop();
    return result;
error:
    corto_log_pop();
    return -1;
}

static
bool corto_checklink(
    const char *link,
    const char *file)
{
    char buf[512];
    char *ptr = buf;
    int length = strlen(file);
    if (length >= 512) {
        ptr = malloc(length + 1);
    }
    int res;
    if (((res = readlink(link, ptr, length)) < 0)) {
        printf("not a link\n");
        goto nomatch;
    }
    if (res != length) {
        goto nomatch;
    }
    if (strncmp(file, ptr, length)) {
        goto nomatch;
    }
    if (ptr != buf) free(ptr);
    return true;
nomatch:
    if (ptr != buf) free(ptr);
    return false;
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

    corto_trace("symlink '%s' => '%s'", newname, fullname);    

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
            char *link;
            if (!corto_checklink(newname, fullname)) {
                /* If a file with specified name already exists, remove existing file */
                if (corto_rm(newname)) {
                    goto error;
                }

                /* Retry */
                if (corto_symlink(fullname, newname)) {
                    goto error;
                }
            } else {
                /* Existing file is a link that points to the same location */
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

int16_t corto_setperm(
    const char *name,
    int perm)
{
    corto_trace("setperm '%s' => %d", name, perm);
    if (chmod(name, perm)) {
        corto_seterr("chmod: %s", strerror(errno));
        return -1;
    }
    return 0;
}

int16_t corto_getperm(
    const char *name,
    int *perm_out)
{
    struct stat st;

    if (stat(name, &st) < 0) {
        corto_seterr("getperm: %s", strerror(errno));
        return -1;
    }

    *perm_out = st.st_mode;

    return 0;
}

bool corto_isdir(const char *path) {
    struct stat buff;
    if (stat(path, &buff) < 0) {
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

    corto_trace("rm '%s'", name);    

    if (corto_isdir(name)) {
        return corto_rmtree(name);
    } else if (remove(name)) {

        /* Don't care if file didn't exist since the postcondition
         * is that file doesn't exist. */
        if (errno != ENOENT) {
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

struct corto_dir_filteredIter {
    corto_idmatch_program program;
    void *files;
};

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
bool corto_dir_hasNextFilter(
    corto_iter *it) 
{
    struct corto_dir_filteredIter *ctx = it->ctx;
    struct dirent *ep = NULL;

    do {
        ep = readdir(ctx->files);
    } while (ep && (*ep->d_name == '.' || !corto_idmatch_run(ctx->program, ep->d_name)));

    if (ep) {
        printf ("return -> %s\n", ep->d_name);
        it->data = ep->d_name;
    }

    return ep ? true : false;
}

static
void corto_dir_release(
    corto_iter *it) 
{
    closedir(it->ctx);
}

static
void corto_dir_releaseFilter(
    corto_iter *it) 
{
    struct corto_dir_filteredIter *ctx = it->ctx;
    closedir(ctx->files);
    corto_idmatch_free(ctx->program);
    free(ctx);
}

static
void corto_dir_releaseRecursiveFilter(
    corto_iter *it)
{
    /* Free all elements */
    corto_iter _it = corto_ll_iter(it->data);
    while (corto_iter_hasNext(&_it)) {
        free(corto_iter_next(&_it));
    }

    /* Free list iterator context */
    corto_ll_iterRelease(it);
}

static
int16_t corto_dir_collectRecursive(
    const char *name, 
    corto_dirstack stack, 
    corto_idmatch_program filter, 
    corto_ll files) 
{
    corto_iter it;

    /* Move to current directory */
    if (!(stack = corto_dirstack_push(stack, name))) {
        goto stack_error;
    }

    /* Obtain iterator to current directory */
    if (corto_dir_iter(".", NULL, &it)) {
        goto error;
    }

    while (corto_iter_hasNext(&it)) {
        char *file = corto_iter_next(&it);

        /* Add file to results if it matches filter */
        char *path = corto_asprintf("%s/%s", corto_dirstack_wd(stack), file);
        corto_path_clean(path, path);
        if (corto_idmatch_run(filter, path)) {
            corto_ll_append(files, path);
        }

        /* If directory, crawl */
        if (corto_isdir(file)) {
            if (corto_dir_collectRecursive(file, stack, filter, files)) {
                goto error;
            }
        }
    }

    corto_dirstack_pop(stack);

    return 0;
error:
    corto_assert(corto_dirstack_pop(stack) == 0, "previous directory vanished");
stack_error:
    return -1;
}


int16_t corto_dir_iter(
    const char *name,
    const char *filter,
    corto_iter *it_out)
{
    if (!filter) {
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
    } else {
        corto_idmatch_program program = corto_idmatch_compile(filter, TRUE, TRUE);
        corto_iter result = CORTO_ITER_EMPTY;

        if (corto_idmatch_scope(program) == 2) {

            corto_ll files = corto_ll_new();
            if (corto_dir_collectRecursive(name, NULL, program, files)) {
                goto error;
            }

            result = corto_ll_iterAlloc(files);
            result.data = files;
            result.release = corto_dir_releaseRecursiveFilter;
        } else {
            struct corto_dir_filteredIter *ctx = corto_alloc(sizeof(struct corto_dir_filteredIter));
            ctx->files = opendir(name);
            if (!ctx->files) {
                free(ctx);
                printError(errno, name);
                goto error;
            }
            ctx->program = program;
            result = (corto_iter){
                .ctx = ctx,
                .data = NULL,
                .hasNext = corto_dir_hasNextFilter,
                .next = corto_dir_next,
                .release = corto_dir_releaseFilter
            };
        }

        *it_out = result;
    }

    return 0;
error:
    corto_seterr("dir_iter: %s", corto_lasterr());
    return -1;
}

bool corto_dir_isEmpty(
    const char *name)
{
    corto_iter it;
    if (corto_dir_iter(name, NULL, &it)) {
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
    char *last = corto_cwd();
    char *result = last + strlen(first);
    if (result[0] == '/') result ++;

    return result;
}

time_t corto_lastmodified(
    const char *name)
{
    struct stat attr;

    if (stat(name, &attr) < 0) {
        corto_seterr("failed to stat '%s' (%s)", name, strerror(errno));
    }

    return attr.st_mtime;
error:
    return -1;
}

