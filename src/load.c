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

/* Lock protecting the package administration */
extern corto_mutex_s corto_load_lock;

struct corto_loadedAdmin {
    char* name;
    corto_thread loading;
    int16_t result;
    corto_dl library;
    char *filename;
    char *base;
};

struct corto_fileHandler {
    char* ext;
    corto_load_cb load;
    void* userData;
};

static
corto_dl corto_load_validLibrary(
    char* fileName,
    char* *build_out);

static
int corto_load_fromDl(
    corto_dl dl,
    char *fileName,
    int argc,
    char *argv[]);

static
char* corto_ptr_castToPath(
    const char* lib,
    corto_id path)
{
    char *bptr, ch;
    const char *ptr;
    /* Convert '::' in library name to '/' */
    ptr = lib;
    bptr = path;

    if (ptr[0] == '/') {
        ptr++;
    } else
    if ((ptr[0] == ':') && (ptr[1] == ':')) {
        ptr += 2;
    }

    while ((ch = *ptr)) {
        if (ch == ':') {
            if (ptr[1] == ':') {
                ch = '/';
                ptr++;
            }
        }
        *bptr = ch;

        ptr++;
        bptr++;
    }
    *bptr = '\0';

    return path;
}

/* Lookup loaded library by name */
static
struct corto_loadedAdmin* corto_loadedAdminFind(
    const char* name)
{
    if (loadedAdmin) {
        corto_iter iter = corto_ll_iter(loadedAdmin);
        struct corto_loadedAdmin *lib;
        corto_id libPath, adminPath;

        corto_ptr_castToPath(name, libPath);

        while (corto_iter_hasNext(&iter)) {
            lib = corto_iter_next(&iter);
            corto_ptr_castToPath(lib->name, adminPath);
            if (!strcmp(adminPath, libPath)) {
                return lib;
            }
        }
    }

    return NULL;
}

/* Add file */
static
struct corto_loadedAdmin* corto_loadedAdminAdd(
    const char* library)
{
    struct corto_loadedAdmin *lib = corto_calloc(sizeof(struct corto_loadedAdmin));
    lib->name = corto_strdup(library);
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
        corto_ll_walk(fileHandlers, (corto_elementWalk_cb)corto_lookupExtWalk, &dummy_p);
    }

    if (dummy_p == &dummy) {
        dummy_p = NULL;
    }

    return dummy_p;
}

/* Convert package identifier to filename */
static
char* corto_load_packageToFile(
    char* package)
{
    char* path;
#ifdef CORTO_STANDALONE_LIB
    path = corto_asprintf("lib%s.so", package[0] == '/' ? package + 1 : package);
    char ch, *ptr;
    for (ptr = path; (ch = *ptr); ptr++) {
        if (ch == '/') *ptr = '_';
    }
#else
    char *fileName = strrchr(package, '/');
    if (fileName) {
        fileName ++;
    } else {
        fileName = package;
    }

    path = corto_asprintf("%s/lib%s.so", package, fileName);
#endif
    return path;
}

static
corto_dl corto_load_validLibrary(
    char* fileName,
    char **build_out)
{
    corto_dl result = NULL;
    char* ___ (*build)(void);

    if (build_out) {
        *build_out = NULL;
    }

    if (!(result = corto_dl_open(fileName))) {
        corto_throw("%s", corto_dl_error());
        goto error;
    }

    /* Lookup build function */
    build = (char* ___ (*)(void))corto_dl_proc(result, "corto_get_build");

    /* Validate version */
    if (build) {
        if (corto_build && strcmp(build(), corto_build)) {
            corto_throw(
              "corto: library '%s' links with conflicting corto library",
              fileName);

            if (build_out && build) {
                *build_out = corto_strdup(build() ? build() : "???");
            }
            goto error;
        }
        corto_debug("'%s' links with correct corto library", fileName);
    } else {
        corto_trace("found '%s' which doesn't link with corto", fileName);
    }

    /* If no build function is available, the library is not linked with
     * Corto, and probably represents a --nocorto package */

    return result;
error:
    if (result) corto_dl_close(result);
    return NULL;
}

static
bool corto_library_check(
    char* fileName,
    char **build_out,
    corto_dl *dl_out)
{
    corto_dl dl = corto_load_validLibrary(fileName, build_out);

    bool result = FALSE;
    if (dl) {
        result = TRUE;
        if (!dl_out) {
            corto_dl_close(dl);
        } else {
            if (*dl_out) {
                corto_dl_close(*dl_out);
            }
            *dl_out = dl;
        }
    }

    return result;
}

/* Load from a dynamic library */
static
int corto_load_fromDl(
    corto_dl dl,
    char *fileName,
    int argc,
    char *argv[])
{
    int (*proc)(int argc, char* argv[]);

    corto_assert(fileName != NULL, "NULL passed to corto_load_fromDl");

    corto_debug("invoke cortomain of '%s' with %d arguments", fileName, argc);

    if (!argv) {
        argv = (char*[]){fileName, NULL};
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
    char* build = NULL;
    bool used_dlopen = false;

    corto_assert(fileName != NULL, "NULL passed to corto_load_library");

    corto_catch();
    if (!validated) {
        dl = corto_load_validLibrary(fileName, &build);
    } else {
        dl = corto_dl_open(fileName);
        used_dlopen = true;
    }

    if (!dl) {
        if (build) {
            corto_throw("%s: uses a different corto build (%s)", fileName, build);
        } else {
            if (used_dlopen) {
                corto_throw("%s: %s", fileName, corto_dl_error());
            } else {
                corto_throw("failed to load library '%s'", fileName);
            }
        }
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

/* Load a package */
int corto_load_intern(
    const char* str,
    int argc,
    char* argv[],
    bool try,
    bool ignoreRecursive,
    bool alwaysRun)
{
    char ext[16];
    struct corto_fileHandler* h;
    int result = -1;
    struct corto_loadedAdmin *lib = NULL;

    corto_log_push(strarg("load:%s", str));

    if (corto_mutex_lock(&corto_load_lock)) {
        corto_throw(NULL);
        goto error;
    }
    lib = corto_loadedAdminFind(str);
    if (lib && lib->library) {
        if (lib->loading == corto_thread_self()) {
            goto recursive;
        } else {
            result = lib->result;
            corto_mutex_unlock(&corto_load_lock);

            /* Other thread is loading library. Wait until finished */
            while (lib->loading) {
                corto_sleep(1, 0);
            }

            if (alwaysRun) {
                result = corto_load_fromDl(lib->library, lib->filename, argc, argv);
            }
            goto loaded;
        }
    }

    if (corto_mutex_unlock(&corto_load_lock)) {
        corto_throw(NULL);
        goto error;
    }

    /* Get extension from filename */
    if (!corto_file_extension((char*)str, ext)) {
        goto error;
    }

    /* Lookup extension */
    if (corto_mutex_lock(&corto_load_lock)) {
        corto_throw(NULL);
        goto error;
    }
    h = corto_lookupExt(ext);

    if (!h) {
        corto_id extPackage;
        sprintf(extPackage, "driver/ext/%s", ext);
        if (corto_mutex_unlock(&corto_load_lock)) {
            corto_throw(NULL);
            goto error;
        }
        if (corto_use(extPackage, 0, NULL)) {
            if (!try) {
                corto_throw(
                    "unable to load file '%s' with extension '%s'",
                    str,
                    ext);
                goto error;
            }
            result = -1;
        }
        if (corto_mutex_lock(&corto_load_lock)) {
            corto_throw(NULL);
            goto error;
        }
        h = corto_lookupExt(ext);
        if (!h) {
            corto_throw(
                "package 'driver/ext/%s' loaded but extension is not registered",
                ext);
            if (corto_mutex_unlock(&corto_load_lock)) {
                corto_throw(NULL);
                goto error;
            }
            goto error;
        }
    }

    /* Load file */
    if (!lib) {
        lib = corto_loadedAdminAdd(str);
        if (corto_mutex_unlock(&corto_load_lock)) {
            corto_throw(NULL);
            goto error;
        }
        result = h->load((char*)str, argc, argv, h->userData);
    } else if (lib->filename) {
        if (lib->loading == corto_thread_self()) {
            goto recursive;
        }
        if (corto_mutex_unlock(&corto_load_lock)) {
            corto_throw(NULL);
            goto error;
        }
        lib->loading = corto_thread_self();
        result = corto_load_library(lib->filename, TRUE, &lib->library, argc, argv);
    } else {
        if (corto_mutex_unlock(&corto_load_lock)) {
            corto_throw(NULL);
            goto error;
        }
        if (!try) {
            corto_throw("'%s' is not a loadable file", lib->name);
            result = -1;
        } else {
            result = 0;
            goto loaded;
        }
    }

    if (corto_mutex_lock(&corto_load_lock)) {
        corto_throw(NULL);
        goto error;
    }

    lib->loading = 0;
    lib->result = result;

    if (corto_mutex_unlock(&corto_load_lock)) {
        corto_throw(NULL);
        goto error;
    }

    if (!result) {
        corto_ok("loaded '%s'", str[0] == '/' ? &str[1] : str);
    }

loaded:
    corto_log_pop();
    return result;
recursive:
    if (!ignoreRecursive) {
        corto_throw("illegal recursive load of file '%s'", lib->name);
        corto_buffer detail = CORTO_BUFFER_INIT;
        corto_buffer_appendstr(&detail, "error occurred while loading:\n");

        corto_iter iter = corto_ll_iter(loadedAdmin);
        while (corto_iter_hasNext(&iter)) {
            struct corto_loadedAdmin *lib = corto_iter_next(&iter);
            if (lib->loading) {
                corto_buffer_append(&detail, "    - #[cyan]%s#[normal] #[magenta]=>#[normal] #[white]%s\n", lib->name, lib->filename ? lib->filename : "");
            }
        }
        char *str = corto_buffer_str(&detail);
        corto_throw_detail(str);
        free(str);
    }
    if (corto_mutex_unlock(&corto_load_lock)) {
        corto_throw(NULL);
        goto error;
    }
    if (ignoreRecursive) {
        corto_log_pop();
        return 0;
    }
error:
    corto_log_pop();
    return -1;
}

#ifndef CORTO_STANDALONE_LIB

/* Locate the right environment for a corto package.
 * Input 'foo/bar'
 * Output: '/home/me/.corto/lib/corto/1.1/foo/bar'
 */
static
char* corto_locate_package(
    char* lib,
    char* *base,
    corto_dl *dl_out,
    bool isLibrary)
{
    char *targetLib = NULL, *homeLib = NULL;
    char *result = NULL;
    char *targetBuild = NULL, *homeBuild = NULL;
    char *details = NULL;
    bool fileError = FALSE;
    corto_dl dl = NULL;
    time_t t = 0;
    int16_t ret = 0;

    corto_assert(targetPath != NULL, "targetPath is not set");
    corto_assert(homePath != NULL, "homePath is not set");

    corto_log_push(strarg("locate:%s", lib));

    /* Look for local packages first */
    if (!(targetLib = corto_asprintf("%s/%s", targetPath, lib))) {
        goto error;
    }

    if ((ret = corto_file_test(targetLib)) == 1) {
        corto_debug("found '%s'", targetLib);
        if (!isLibrary || corto_library_check(targetLib, &targetBuild, &dl)) {
            t = corto_lastmodified(targetLib);
            result = targetLib;
            if (base) {
                *base = (char*)targetBase;
            }
        } else {
            corto_catch();
            // corto_queue_raise();
        }
    } else {
        if (ret == -1) {
            // corto_queue_raise();
        } else {
            corto_debug("'%s' not found", targetLib);
        }
    }

    /* Look for packages in BAKE_HOME */
    if (strcmp(corto_getenv("BAKE_HOME"), corto_getenv("BAKE_TARGET"))) {
        if (!(homeLib = corto_asprintf("%s/%s", homePath, lib))) {
            goto error;
        }

        if ((ret = corto_file_test(homeLib)) == 1) {
            time_t myT = corto_lastmodified(homeLib);
            corto_debug("found '%s'", homeLib);
            if ((myT >= t) || !result) {
                if (!isLibrary || corto_library_check(homeLib, &homeBuild, &dl)) {
                    t = myT;
                    result = homeLib;
                    if (base) {
                        *base = (char*)homeBase;
                    }
                } else {
                    corto_catch();
                    // corto_queue_raise();
                }
            } else if (!result) {
                corto_debug("discarding '%s' because '%s' is newer", homeLib, result);
            }
        } else {
            if (ret == -1) {
                // corto_queue_raise();
            } else {
                corto_debug("'%s' not found", homeLib);
            }
        }
    } else {
        corto_debug("'%s' already searched ($BAKE_HOME == $BAKE_TARGET)",
            corto_getenv("BAKE_HOME"));
    }

    if (targetLib && (targetLib != result)) corto_dealloc(targetLib);
    if (homeLib && (homeLib != result)) corto_dealloc(homeLib);

    if (!result) {
        if (details) {
            if (fileError) {
                corto_throw(details);
            }
        } else {
            corto_setinfo("library '%s' not found", lib);
        }
        if (dl) {
            corto_dl_close(dl);
            dl = NULL;
        }
    }

    if (dl_out) {
        *dl_out = dl;
    }

    corto_log_pop();
    return result;
error:
    corto_log_pop();
    return NULL;
}
#endif

static
char* corto_locate_getName(
    char* package,
    corto_load_locateKind kind)
{
    if (package[0] == '/') {
        package ++;
    }
    if (kind == CORTO_LOCATION_NAME) {
        char *name = strrchr(package, '/');
        if (name) {
            name ++;
        } else {
            name = package;
        }
        return corto_strdup(name);
    } else {
        return corto_strdup(package);
    }
}

char* corto_locate(
    char* package,
    corto_dl *dl_out,
    corto_load_locateKind kind)
{
    char* relativePath = NULL;
    char* result = NULL;

#ifndef CORTO_STANDALONE_LIB
    corto_dl dl = NULL;
    char* base = NULL;
    struct corto_loadedAdmin *loaded = NULL;

    /* If package has been loaded already, don't resolve it again */
    loaded = corto_loadedAdminFind(package);
    if (loaded) {
        result = loaded->filename ? corto_strdup(loaded->filename) : NULL;
        base = loaded->base;
    }
#endif

    if (!result) {
        relativePath = corto_load_packageToFile(package);
        if (!relativePath) {
            goto error;
        }
    }

#ifdef CORTO_STANDALONE_LIB
    if (!corto_library_check(relativePath, NULL, dl_out)) {
        goto error;
    }

    result = relativePath;
    switch(kind) {
    case CORTO_LOCATION_ENV:
    case CORTO_LOCATION_LIBPATH:
    case CORTO_LOCATION_INCLUDE:
    case CORTO_LOCATION_ETC:
        corto_dealloc(result);
        result = corto_strdup("");
        break;
    case CORTO_LOCATION_LIB:
        /* Result is already pointing to the lib */
        break;
    case CORTO_LOCATION_NAME:
    case CORTO_LOCATION_FULLNAME: {
        corto_dealloc(result);
        result = corto_locate_getName(package, kind);
        break;
    }
    }
#else

    bool setLoadAdminWhenFound = TRUE;
    if (!loaded || (!result && loaded->loading)) {
        result = corto_locate_package(relativePath, &base, &dl, TRUE);
        if (!result && (kind != CORTO_LOCATION_LIB)) {
            corto_catch();
            result = corto_locate_package(package, &base, &dl, FALSE);
            setLoadAdminWhenFound = FALSE;
        }
    }
    if (relativePath) corto_dealloc(relativePath);

    if (result) {
        if (!loaded) {
            loaded = corto_loadedAdminAdd(package);
            loaded->loading = 0;
        }

        if (!loaded->filename && setLoadAdminWhenFound) {
            if (corto_mutex_lock(&corto_load_lock)) {
                corto_throw(NULL);
                goto error;
            }
            strset(&loaded->filename, result);
            strset(&loaded->base, base);
            if (dl_out) {
                loaded->library = dl;
            }
            if (corto_mutex_unlock(&corto_load_lock)) {
                corto_throw(NULL);
                goto error;
            }
        }

        switch(kind) {
        case CORTO_LOCATION_ENV:
            /* Quick & dirty trick to strip everything but the env */
            result = corto_asprintf(base, "@");
            *(strchr(result, '@') - 1) = '\0'; /* Also strip the '/' */
            break;
        case CORTO_LOCATION_LIB:
            /* Result is already pointing to the lib */
            break;
        case CORTO_LOCATION_LIBPATH: {
            char* lib;
            lib = corto_asprintf(base, "lib");
            result = corto_asprintf("%s/%s", lib, package);
            break;
        }
        case CORTO_LOCATION_INCLUDE: {
            char* include;
            include = corto_asprintf(base, "include");
            result = corto_asprintf("%s/%s", include, package);
            corto_dealloc(include);
            break;
        }
        case CORTO_LOCATION_ETC: {
            char* etc;
            etc = corto_asprintf(base, "etc");
            result = corto_asprintf("%s/%s", etc, package);
            corto_dealloc(etc);
            break;
        }
        case CORTO_LOCATION_NAME:
        case CORTO_LOCATION_FULLNAME: {
            result = corto_locate_getName(package, kind);
            break;
        }
        }
    }

    if (dl_out) {
        if (loaded) {
            *dl_out = loaded->library;
        } else {
            *dl_out = NULL;
        }
    } else if (dl) {
        corto_dl_close(dl);
    }
#endif

    return result;
error:
    return NULL;
}

void* corto_load_sym(
    char *package,
    corto_dl *dl_out,
    char *symbol)
{
    void *result = NULL;

    if (!*dl_out) {
        char *location = corto_locate(package, dl_out, CORTO_LOCATION_LIB);
        if (!*dl_out) {
            if (location) {
                *dl_out = corto_load_validLibrary(location, NULL);
            }
        }
        if (!*dl_out) {
            /* TODO: THROW ERROR */
        }
        if (location) {
            free(location);
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
}

void(*corto_load_proc(char *package, corto_dl *dl_out, char *symbol))(void)
{
    return (void(*)(void))(uintptr_t)corto_load_sym(package, dl_out, symbol);
}

/* Load file with unspecified extension */
int corto_file_loader(
    char* package,
    int argc,
    char* argv[],
    void* ctx)
{
    CORTO_UNUSED(ctx);
    char* fileName;
    int result;
    corto_dl dl = NULL;

    fileName = corto_locate(package, &dl, CORTO_LOCATION_LIB);
    if (!fileName) {
        corto_throw(corto_lastinfo());
        return -1;
    }

    corto_assert(dl != NULL, "package located but dl_out is NULL");

    result = corto_load_fromDl(dl, fileName, argc, argv);
    corto_dealloc(fileName);
    if (result) {
        corto_throw(corto_lastinfo());
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

    /* Free loaded administration (always happens from mainthread) */

    if (loadedAdmin) {
        iter = corto_ll_iter(loadedAdmin);
         while(corto_iter_hasNext(&iter)) {
             struct corto_loadedAdmin *loaded = corto_iter_next(&iter);
             free(loaded->name);
             if (loaded->filename) free(loaded->filename);
             if (loaded->base) free(loaded->base);
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
    if (corto_mutex_lock(&corto_load_lock)) {
        corto_throw(NULL);
        goto error;
    }
    if ((h = corto_lookupExt(ext))) {
        if (h->load != handler) {
            corto_error("corto_load_register: extension '%s' is already registered with another loader.", ext);
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
    if (corto_mutex_unlock(&corto_load_lock)) {
        corto_throw(NULL);
        goto error;
    }

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

    /* Precompute base with parameter for lib, etc, include */
    targetBase = corto_envparse(
        "$BAKE_TARGET/%%s/corto/%s",
        version);

    /* Home path - where corto is located */
    homeBase = corto_envparse(
        "$BAKE_HOME/%%s/corto/%s",
        version);
}
