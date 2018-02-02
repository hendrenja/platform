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

static corto_ll fileHandlers = NULL;
static corto_ll loadedAdmin = NULL;
static corto_ll libraries = NULL;

/* Static variables set during initialization that contain paths to packages */
static const char *targetEnv, *homeEnv;
static const char *targetPath, *homePath;
static const char *targetBase, *homeBase;
static const char *version;
static const char *corto_build;
static bool target_different_from_home;

/* Lock protecting the package administration */
extern corto_mutex_s corto_load_lock;

struct corto_loaded {
    char* id; /* package id or file */

    /* Package-specific members (not filled out for ordinary files) */
    char *base;
    char *env; /* Environment in which the package is installed */
    char *lib; /* Path to library (if available) */
    char *app; /* Path to executable (if available) */
    char *bin; /* Path to binary (if available) */
    char *etc; /* Path to project etc (if available) */
    char *include; /* Path to project include (if available) */
    char *project; /* Path to project lib. Always available if valid project */
    bool tried_binary; /* Set to true if already tried loading the bin path */

    /* Members used by package loader to indicate current loading status */
    corto_thread loading; /* current thread loading package */
    int16_t loaded; /* -1 error, 0 not loaded, 1 loaded */

    corto_dl library; /* pointer to library */
};

struct corto_fileHandler {
    char* ext;
    corto_load_cb load;
    void* userData;
};

static
int corto_load_fromDl(
    corto_dl dl,
    const char *fileName,
    int argc,
    char *argv[]);

/* Lookup loaded library by name */
static
struct corto_loaded* corto_loaded_find(
    const char* name)
{
    if (loadedAdmin) {
        corto_iter iter = corto_ll_iter(loadedAdmin);
        struct corto_loaded *lib;
        const char *nameptr = name;
        if (nameptr[0] == '/') nameptr ++;

        while (corto_iter_hasNext(&iter)) {
            lib = corto_iter_next(&iter);
            const char *idptr = lib->id;
            if (idptr[0] == '/') idptr ++;
            if (!strcmp(nameptr, idptr)) {
                return lib;
            }
        }
    }

    return NULL;
}

/* Add file */
static
struct corto_loaded* corto_loaded_add(
    const char* library)
{
    struct corto_loaded *lib = corto_calloc(sizeof(struct corto_loaded));
    lib->id = corto_strdup(library);
    lib->loading = corto_thread_self();
    if (!loadedAdmin) {
        loadedAdmin = corto_ll_new();
    }
    corto_ll_insert(loadedAdmin, lib);
    return lib;
}

/* Lookup file handler action */
static
int corto_lookupExtWalk(
    struct corto_fileHandler* h,
    struct corto_fileHandler** data)
{
    if (!strcmp(h->ext, (*data)->ext)) {
        *data = h;
        return 0;
    }
    return 1;
}

/* Lookup file handler */
static
struct corto_fileHandler* corto_lookupExt(
    char* ext)
{
    struct corto_fileHandler dummy, *dummy_p;

    dummy.ext = ext;
    dummy_p = &dummy;

    /* Walk handlers */
    if (fileHandlers) {
        corto_ll_walk(
            fileHandlers, (corto_elementWalk_cb)corto_lookupExtWalk, &dummy_p);
    }

    if (dummy_p == &dummy) {
        dummy_p = NULL;
    }

    return dummy_p;
}

/* Load from a dynamic library */
static
int corto_load_fromDl(
    corto_dl dl,
    const char *fileName,
    int argc,
    char *argv[])
{
    int (*proc)(int argc, char* argv[]);

    corto_assert(fileName != NULL, "NULL passed to corto_load_fromDl");

    corto_debug("invoke cortomain of '%s' with %d arguments", fileName, argc);

    if (!argv) {
        argv = (char*[]){(char*)fileName, NULL};
        argc = 1;
    }

    proc = (int(*)(int,char*[]))corto_dl_proc(dl, "cortoinit");
    if (proc) {
        if (proc(argc, argv)) {
            corto_throw("cortoinit failed for '%s'", fileName);
            goto error;
        }
    }

    proc = (int(*)(int,char*[]))corto_dl_proc(dl, "cortomain");
    if (proc) {
        if (proc(argc, argv)) {
            corto_throw("cortomain failed for '%s'", fileName);
            goto error;
        }
    } else {
        char* ___ (*build)(void);

        /* Lookup build function */
        build = (char* ___ (*)(void))corto_dl_proc(dl, "corto_get_build");
        if (build) {
            corto_trace(
                "library '%s' linked with corto but does not have a cortomain",
                fileName);
        }
    }

    /* Add library to libraries list */
    if (corto_mutex_lock (&corto_load_lock)) {
        corto_throw(NULL);
        goto error;
    }
    if (!libraries || !corto_ll_hasObject(libraries, dl)) {
        if (!libraries) {
            libraries = corto_ll_new();
        }

        corto_ll_insert(libraries, dl);
        corto_debug("loaded '%s'", fileName);
    }
    if (corto_mutex_unlock (&corto_load_lock)) {
        corto_throw(NULL);
        goto error;
    }

    return 0;
error:
    return -1;
}

/*
 * Load a Corto library
 * Receives the absolute path to the lib<name>.so file.
 */
static
int corto_load_library(
    char* fileName,
    bool validated,
    corto_dl *dl_out,
    int argc,
    char* argv[])
{
    corto_dl dl = NULL;

    corto_assert(fileName != NULL, "NULL passed to corto_load_library");
    corto_catch();

    dl = corto_dl_open(fileName);

    if (!dl) {
        corto_throw("%s: %s", fileName, corto_dl_error());
        goto error;
    }

    if (corto_load_fromDl(dl, fileName, argc, argv)) {
        goto error;
    }

    if (dl_out) {
        *dl_out = dl;
    }

    return 0;
error:
    if (dl) corto_dl_close(dl);
    return -1;
}

/*
 * An adapter on top of corto_load_library to fit the corto_load_cb signature.
 */
int corto_load_libraryAction(
    char* file,
    int argc,
    char* argv[],
    void *data)
{
    CORTO_UNUSED(data);
    return corto_load_library(file, FALSE, NULL, argc, argv);
}

static
int16_t corto_test_package(
    const char *env,
    const char *package,
    time_t *t_out)
{
    int16_t result = 0;
    char *path = corto_asprintf("%s/%s/project.json", env, package);
    if ((result = corto_file_test(path)) == 1) {
        corto_debug("found '%s'", path);
        *t_out = corto_lastmodified(path);
    } else {
        if (result != -1) {
            corto_trace("'%s' not found", path);
        }
    }
    free(path);
    return result;
}

static
int16_t corto_locate_package(
    const char* package,
    const char **env,
    const char **base)
{
    time_t t_target = 0, t_home = 0;
    int16_t ret = 0;

    corto_assert(targetPath != NULL, "targetPath is not set");
    corto_assert(homePath != NULL, "homePath is not set");

    corto_log_push(strarg("locate:%s", package));

    /* Look for project in BAKE_TARGET first */
    corto_debug("try to find '%s' in $BAKE_TARGET ('%s')", package, targetPath);
    if ((ret = corto_test_package(targetPath, package, &t_target)) != -1) {
        if (ret == 1) {
            *env = targetEnv;
            *base = targetBase;
        }

        /* If no errors and BAKE_TARGET != BAKE_HOME look in BAKE_HOME */
        if (target_different_from_home) {
            corto_debug("try to find '%s' in $BAKE_HOME ('%s')", package, homePath);
            if ((corto_test_package(homePath, package, &t_home) == 1)) {
                if (!ret || t_target < t_home) {
                    *env = homeEnv;
                    *base = homeBase;
                }
            }
        } else {
            corto_debug("skip looking in $BAKE_HOME (same as $BAKE_TARGET)");
        }
    }

    corto_log_pop();
    return ret;
}

static
const char* corto_locate_getName(
    const char* package)
{
    if (package[0] == '/') {
        package ++;
    }

    char *name = strrchr(package, '/');
    if (name) {
        return name + 1;
    } else {
        return package;
    }
}

static
int16_t corto_locate_binary(
    const char *id,
    struct corto_loaded *loaded)
{
    const char *name = corto_locate_getName(id);
    int16_t ret = 0;

    /* First test for library */
    char *bin = corto_asprintf("%s/lib%s.so", loaded->project, name);
    if ((ret = corto_file_test(bin)) == 1) {
        /* Library found */
        loaded->lib = bin;
        loaded->bin = bin;
        ret = 0;
    } else if (ret == 0) {
        free(bin);
        bin = corto_asprintf("%s/%s", loaded->project, name);
        if ((ret = corto_file_test(bin)) == 1) {
            /* Application found */
            loaded->app = bin;
            loaded->bin = bin;
            ret = 0;
        } else {
            free(bin);
        }
    }

    /* Prevent looking for the binary again */
    loaded->tried_binary = true;

    /* If ret is -1, a binary was found, but accessing it caused an error */
    return ret;
}

const char* corto_locate(
    const char* package,
    corto_dl *dl_out,
    corto_locate_kind kind)
{
    char *result = NULL;
    const char *base = NULL, *env = NULL;
    struct corto_loaded *loaded = NULL;

    corto_try (
        corto_mutex_lock(&corto_load_lock), NULL);

    /* If package has been loaded already, don't resolve it again */
    loaded = corto_loaded_find(package);
    if (loaded && loaded->base) {
        base = loaded->base;
    } else {
        /* If package has not been found, try locating it */
        if (corto_locate_package(package, &env, &base) == -1) {
            /* Error happened other than not being able to find the package */
            corto_throw(NULL);
            goto error;
        }
    }

    /* If package is not in load admin but has been located, add to admin */
    if ((!loaded || !loaded->base) && base) {
        corto_assert(env != NULL, "if base is set, env should also be set");
        if (!loaded) {
            loaded = corto_loaded_add(package);
            loaded->loading = 0; /* locate is not going to load package */
        }
        strset(&loaded->env, env);
        strset(&loaded->base, base);

        /* Project location is guaranteed to exist if we have a base */
        loaded->project = corto_asprintf(base, "lib", package);
    }

    /* If loaded hasn't been loaded by now, package isn't found */
    if (!loaded) {
        corto_trace("package '%s' not found", package);
        goto error;
    }

    /* If base has been found, derive location */
    if (base) {
        switch(kind) {
        case CORTO_LOCATE_ENV:
            result = loaded->env; /* always set */
            break;
        case CORTO_LOCATE_PACKAGE:
            result = loaded->project; /* always set */
            break;
        case CORTO_LOCATE_ETC:
            if (!loaded->etc) {
                loaded->etc = corto_asprintf(base, "etc", package);
            }
            result = loaded->etc;
            break;
        case CORTO_LOCATE_INCLUDE:
            if (!loaded->include) {
                loaded->include = corto_asprintf(base, "include", package);
            }
            result = loaded->include;
            break;
        case CORTO_LOCATE_LIB:
        case CORTO_LOCATE_APP:
        case CORTO_LOCATE_BIN:
            if (!loaded->tried_binary) {
                if (corto_locate_binary(package, loaded)) {
                    goto error;
                }
            }
            if (kind == CORTO_LOCATE_LIB) result = loaded->lib;
            if (kind == CORTO_LOCATE_APP) result = loaded->app;
            if (kind == CORTO_LOCATE_BIN) result = loaded->bin;
            break;
        }
    }

    /* If locating a library, library is requested and lookup is successful,
     * load library within lock (can save additional lookups/locks) */
    if (dl_out && kind == CORTO_LOCATE_LIB && loaded->lib) {
        if (!loaded->library) {
            loaded->library = corto_dl_open(loaded->lib);
        }
        *dl_out = loaded->library;
    } else if (dl_out) {
        /* Library was not found */
    }

    corto_try (
        corto_mutex_unlock(&corto_load_lock), NULL);

    return result;
error:
    if (corto_mutex_unlock(&corto_load_lock)) {
        corto_throw(NULL);
    }
    return NULL;
}

static
struct corto_fileHandler* corto_load_filehandler(
    const char *file)
{
    /* Get extension from filename */
    char ext[16];
    if (!corto_file_extension((char*)file, ext)) {
        goto error;
    }

    /* Lookup corresponding handler for file */
    struct corto_fileHandler *h = corto_lookupExt(ext);

    /* If filehandler is not found, look it up in driver/ext */
    if (!h) {
        corto_id extPackage;
        sprintf(extPackage, "driver/ext/%s", ext);

        corto_try(
            corto_mutex_unlock(&corto_load_lock), NULL);

        /* Try to load the extension package */
        if (corto_use(extPackage, 0, NULL)) {
            corto_throw(
                "unable to load file '%s' with extension '%s'",
                file,
                ext);
            goto error;
        }
        corto_try (
            corto_mutex_lock(&corto_load_lock), NULL);

        /* Extension package should have registered the extension in the
         * cortomain function, so try loading again. */
        h = corto_lookupExt(ext);
        if (!h) {
            corto_throw(
                "package 'driver/ext/%s' loaded but extension is not registered",
                ext);
            corto_try(
                corto_mutex_unlock(&corto_load_lock), NULL);
            goto error;
        }
    }
    return h;
error:
    return NULL;
}

/* Load a file of a supported type (file path or in package repo) */
int corto_load_intern(
    const char* file,
    int argc,
    char* argv[],
    bool try,
    bool ignore_recursive,
    bool always_load)
{
    struct corto_fileHandler* h;
    int result = -1;
    bool other_thread_loading = false;

    corto_log_push(strarg("load:%s", file));

    corto_try(
        corto_mutex_lock(&corto_load_lock), NULL);

    /* Check if file is added to admin */
    struct corto_loaded *loaded_admin = corto_loaded_find(file);

    if (loaded_admin) {
        if (loaded_admin->loading == corto_thread_self()) {
            /* Illegal recursive load of file */
            goto recursive;
        } else {
            other_thread_loading = true;
        }
    }

    if (!loaded_admin) {
        loaded_admin = corto_loaded_add(file);
    }

    /* If other thread is loading file, wait until it finishes. This can happen
     * when the other thread unlocked to allow for running the file handler. */
    if (other_thread_loading) {
        while (loaded_admin->loading) {
            /* Need to unlock, as other thread will try to relock after the file
             * handler is executed. */
            corto_try(
                corto_mutex_unlock(&corto_load_lock), NULL);

            while (loaded_admin->loading) {
                corto_sleep(0, 100000000);
            }

            /* Relock, so we can safely inspect & modify the admin again. */
            corto_try(
                corto_mutex_lock(&corto_load_lock), NULL);

            /* Keep looping until we have the lock and there is no other thread
             * currently loading this file */
        }
    }

    /* Only load when file was not loaded, except when always_load is set */
    if (loaded_admin->loaded == 0 || always_load) {
        /* Load handler for file type */
        h = corto_load_filehandler(file);
        if (!h) {
            corto_throw(NULL);
            goto error;
        }

        /* This thread is going to load the file */
        loaded_admin->loading = corto_thread_self();

        /* Unlock, so file handler can load other files without deadlocking */
        corto_try(
            corto_mutex_unlock(&corto_load_lock), NULL);

        /* Load file */
        result = h->load((char*)file, argc, argv, h->userData);

        /* Relock admin to update */
        corto_try(
            corto_mutex_lock(&corto_load_lock), NULL);

        if (result) {
            /* Set loaded to 1 if success, or -1 if failed */
            loaded_admin->loaded = result ? -1 : 1;
        }

        /* Thread is no longer loading the file */
        loaded_admin->loading = 0;
    }

    corto_try(
        corto_mutex_unlock(&corto_load_lock), NULL);

    if (!result) {
        corto_ok("loaded '%s'", file);
    }

    corto_log_pop();

    return result;
recursive:
    if (!ignore_recursive) {
        corto_buffer detail = CORTO_BUFFER_INIT;
        corto_throw("illegal recursive load of file '%s'", loaded_admin->id);
        corto_buffer_appendstr(&detail, "error occurred while loading:\n");
        corto_iter iter = corto_ll_iter(loadedAdmin);
        while (corto_iter_hasNext(&iter)) {
            struct corto_loaded *lib = corto_iter_next(&iter);
            if (lib->loading) {
                corto_buffer_append(
                    &detail,
                    "    - #[cyan]%s#[normal] #[magenta]=>#[normal] #[white]%s\n",
                    lib->id, lib->bin ? lib->bin : "");
            }
        }
        char *str = corto_buffer_str(&detail);
        corto_throw_detail(str);
        free(str);
    }
    corto_try(
        corto_mutex_unlock(&corto_load_lock), NULL);

    if (ignore_recursive) {
        corto_log_pop();
        return 0;
    }
error:
    corto_log_pop();
    return -1;
}

void* corto_load_sym(
    char *package,
    corto_dl *dl_out,
    char *symbol)
{
    void *result = NULL;

    if (!*dl_out) {
        const char *lib = corto_locate(package, dl_out, CORTO_LOCATE_LIB);
        if (!*dl_out) {
            if (lib) {
                *dl_out = corto_dl_open(lib);
            }
        }
        if (!*dl_out) {
            corto_throw(NULL);
            goto error;
        }
    }

    if (*dl_out) {
        result = corto_dl_sym(*dl_out, symbol);
        if (!result) {
            char *err = (char*)corto_dl_error();
            if (err) {
                corto_throw(err);
            } else {
                corto_throw("symbol lookup failed for '%s'", symbol);
            }
        }
    } else {
        corto_throw("failed to load '%s'", package);
    }

    return result;
error:
    return NULL;
}

void(*corto_load_proc(
    char *package,
    corto_dl *dl_out,
    char *symbol))(void)
{
    return (void(*)(void))(uintptr_t)corto_load_sym(package, dl_out, symbol);
}

/* Load file with no extension */
int corto_file_loader(
    char* package,
    int argc,
    char* argv[],
    void* ctx)
{
    CORTO_UNUSED(ctx);
    const char* fileName;
    int result;
    corto_dl dl = NULL;

    fileName = corto_locate(package, &dl, CORTO_LOCATE_LIB);
    if (!fileName) {
        corto_throw(NULL);
        return -1;
    }

    corto_assert(dl != NULL, "package located but dl_out is NULL");

    result = corto_load_fromDl(dl, fileName, argc, argv);

    if (result) {
        corto_throw(NULL);
    }

    return result;
}

void corto_loaderOnExit(
    void* ctx)
{
    struct corto_fileHandler* h;
    corto_dl dl;
    corto_iter iter;

    CORTO_UNUSED(ctx);

    /* Free loaded administration. Always happens from mainthread, so no lock
     * required. */

    if (loadedAdmin) {
        iter = corto_ll_iter(loadedAdmin);
         while(corto_iter_hasNext(&iter)) {
             struct corto_loaded *loaded = corto_iter_next(&iter);
             free(loaded->id);
             if (loaded->base) free(loaded->base);
             if (loaded->env) free(loaded->env);
             if (loaded->lib) free(loaded->lib);
             if (loaded->app) free(loaded->app);
             if (loaded->etc) free(loaded->etc);
             if (loaded->include) free(loaded->include);
             if (loaded->project) free(loaded->project);
             free(loaded);
         }
         corto_ll_free(loadedAdmin);
    }

    /* Free handlers */
    while ((h = corto_ll_takeFirst(fileHandlers))) {
        corto_dealloc(h);
    }
    corto_ll_free(fileHandlers);

    /* Free libraries */
    if (libraries) {
        while ((dl = corto_ll_takeFirst(libraries))) {
            corto_dl_close(dl);
        }
        corto_ll_free(libraries);
    }
}

/* Load a package */
int corto_use(
    const char* str,
    int argc,
    char* argv[])
{
    return corto_load_intern(str, argc, argv, FALSE, FALSE, FALSE);
}

/* Run a package */
int corto_run(
    const char* str,
    int argc,
    char* argv[])
{
    return corto_load_intern(str, argc, argv, FALSE, FALSE, TRUE);
}

/* Register a filetype */
int corto_load_register(
    char* ext,
    corto_load_cb handler,
    void* userData)
{
    struct corto_fileHandler* h;

    /* Check if extension is already registered */
    corto_try(
        corto_mutex_lock(&corto_load_lock), NULL);

    if ((h = corto_lookupExt(ext))) {
        if (h->load != handler) {
            corto_error(
                "corto_load_register: extension '%s' is already registered with another loader.",
                ext);
            abort();
            goto error;
        }
    } else {
        h = corto_alloc(sizeof(struct corto_fileHandler));
        h->ext = ext;
        h->load = handler;
        h->userData = userData;

        if (!fileHandlers) {
            fileHandlers = corto_ll_new();
        }

        corto_trace("registered file extension '%s'", ext);
        corto_ll_append(fileHandlers, h);
    }

    corto_try(
        corto_mutex_unlock(&corto_load_lock), NULL);

    return 0;
error:
    return -1;
}

/* Initialize paths necessary for loader */
void corto_load_init(
    const char *target,
    const char *home,
    const char *v,
    const char *build)
{
    targetEnv = target;
    homeEnv = home;
    version = v;
    corto_build = build;

    /* Target path - where packages are being built */
    targetPath = corto_envparse(
        "$BAKE_TARGET/lib/corto/%s",
        version);

    /* Home path - where corto is located */
    homePath = corto_envparse(
        "$BAKE_HOME/lib/corto/%s",
        version);

    /* Precompute whether home is different from target */
    target_different_from_home = strcmp(targetPath, homePath);

    /* Precompute base with parameter for lib, etc, include */
    targetBase = corto_envparse(
        "$BAKE_TARGET/%%s/corto/%s/%%s",
        version);

    /* Home path - where corto is located */
    homeBase = corto_envparse(
        "$BAKE_HOME/%%s/corto/%s/%%s",
        version);
}
