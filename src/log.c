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

char *corto_log_appName = "";
extern corto_mutex_s corto_log_lock;
static corto_tls corto_errKey = 0;
static corto_log_verbosity CORTO_LOG_LEVEL = CORTO_INFO;
static char *corto_log_fmt_application;
static char *corto_log_fmt_current = CORTO_LOGFMT_DEFAULT;

CORTO_EXPORT char* corto_backtraceString(void);
CORTO_EXPORT void corto_printBacktrace(FILE* f, int nEntries, char** symbols);

#define DEPTH 60

typedef struct corto_errThreadData {
    char *lastInfo;
    char *lastError;
    char *lastCategory;
    char *lastFile;
    unsigned int lastLine;
    char *backtrace;
    bool viewed;
    char *categories[CORTO_MAX_LOG_COMPONENTS + 1];
} corto_errThreadData;

struct corto_log_handler {
    corto_log_verbosity min_level, max_level;
    char *category_filter;
    corto_idmatch_program compiled_category_filter;
    char *auth_token;
    void *ctx;
    corto_log_handler_cb cb;
};

static void corto_lasterrorFree(void* tls) {
    corto_errThreadData* data = tls;
    if (data) {
        if (!data->viewed && data->lastError) {
            corto_warning("uncatched error (use corto_lasterr): %s%s%s",
              data->lastError, data->backtrace ? "\n" : "", data->backtrace ? data->backtrace : "");
        }
        if (data->lastError) {
            corto_dealloc(data->lastError);
        }
        if (data->lastInfo) {
            corto_dealloc(data->lastInfo);
        }
        corto_dealloc(data);
    }
}

static corto_errThreadData* corto_getThreadData(void){
    corto_errThreadData* result;
    if (!corto_errKey) {
        corto_tls_new(&corto_errKey, corto_lasterrorFree);
    }
    result = corto_tls_get(corto_errKey);
    if (!result) {
        result = corto_calloc(sizeof(corto_errThreadData));
        corto_tls_set(corto_errKey, result);
    }
    return result;
}

void corto_printBacktrace(FILE* f, int nEntries, char** symbols) {
    int i;
    for(i=1; i<nEntries; i++) { /* Skip this function */
        fprintf(f, "  %s\n", symbols[i]);
    }
    fprintf(f, "\n");
}

void corto_backtrace(FILE* f) {
    int nEntries;
    void* buff[DEPTH];
    char** symbols;

    nEntries = backtrace(buff, DEPTH);
    if (nEntries) {
        symbols = backtrace_symbols(buff, DEPTH);

        corto_printBacktrace(f, nEntries, symbols);

        free(symbols);
    } else {
        fprintf(f, "obtaining backtrace failed.");
    }
}

char* corto_backtraceString(void) {
    int nEntries;
    void* buff[DEPTH];
    char** symbols;
    char* result;

    result = malloc(10000);
    *result = '\0';

    nEntries = backtrace(buff, DEPTH);
    if (nEntries) {
        symbols = backtrace_symbols(buff, DEPTH);

        int i;
        for(i=1; i<nEntries; i++) { /* Skip this function */
            sprintf(result, "%s  %s\n", result, symbols[i]);
        }
        strcat(result, "\n");

        free(symbols);
    } else {
        printf("obtaining backtrace failed.");
    }

    return result;
}

static corto_ll corto_log_handlers;

corto_log_handler corto_log_handlerRegister(
    corto_log_verbosity min_level, 
    corto_log_verbosity max_level,
    char* category_filter, 
    char* auth_token,
    corto_log_handler_cb callback,
    void *ctx)
{
    struct corto_log_handler* result = corto_alloc(sizeof(struct corto_log_handler));

    result->min_level = min_level;
    result->max_level = max_level;
    result->category_filter = category_filter ? corto_strdup(category_filter) : NULL;
    result->auth_token = auth_token ? corto_strdup(auth_token) : NULL;
    result->cb = callback;
    result->ctx = ctx;

    if (result->category_filter) {
        result->compiled_category_filter = 
            corto_idmatch_compile(result->category_filter, TRUE, TRUE);
        if (!result->compiled_category_filter) {
            corto_seterr("invalid filter: %s", corto_lasterr());
            goto error;
        }
    } else {
        result->compiled_category_filter = NULL;
    }

    corto_mutex_lock(&corto_log_lock);
    if (!corto_log_handlers) {
        corto_log_handlers = corto_ll_new();
    }
    corto_ll_append(corto_log_handlers, result);
    corto_mutex_unlock(&corto_log_lock);

    return result;
error:
    if (result) corto_dealloc(result);
    return NULL;
}

void corto_log_handlerUnregister(corto_log_handler cb)
{
    struct corto_log_handler* callback = cb;
    if (callback) {
        corto_mutex_lock(&corto_log_lock);
        corto_ll_remove(corto_log_handlers, callback);
        if (!corto_ll_size(corto_log_handlers)) {
            corto_ll_free(corto_log_handlers);
            corto_log_handlers = NULL;
        }
        corto_mutex_unlock(&corto_log_lock);

        if (callback->category_filter) corto_dealloc(callback->category_filter);
        if (callback->auth_token) corto_dealloc(callback->auth_token);
        if (callback->compiled_category_filter) corto_idmatch_free(callback->compiled_category_filter);
        corto_dealloc(callback);
    }
}

bool corto_log_handlersRegistered(void) {
    return corto_log_handlers != NULL;
}

void corto_err_notifyCallkback(
    corto_log_handler cb,
    char *categories[],
    corto_log_verbosity level, 
    char *msg)
{
    struct corto_log_handler* callback = cb;
    bool filterMatch = TRUE;
    if (level >= callback->min_level && level <= callback->max_level) {
        if (callback->compiled_category_filter) {
            corto_buffer buff = CORTO_BUFFER_INIT;
            int32_t i;
            for (i = 0; categories[i]; i++) {
                if (i) corto_buffer_appendstr(&buff, "/");
                corto_buffer_appendstr(&buff, categories[i]);
            }
            char *str = corto_buffer_str(&buff);
            if (!corto_idmatch_run(callback->compiled_category_filter, str)) {
                filterMatch = FALSE;
            }
            corto_dealloc(str);
        }

        if (filterMatch) {
            callback->cb(level, categories, msg, callback->ctx);
        }
    }
}

#define CORTO_MAX_LOG (1024)

static char* corto_log_categoryString(char *categories[]) {
    int32_t i = 0;
    corto_buffer buff = CORTO_BUFFER_INIT;

    while (categories[i]) {
        corto_buffer_append(&buff, "%s%s%s", 
            CORTO_MAGENTA, categories[i], CORTO_NORMAL);
        i ++;
        if (categories[i]) {
            corto_buffer_appendstr(&buff, ".");
        }
    }

    return corto_buffer_str(&buff);
}

static char* corto_log_tokenize(char *msg) {
    corto_buffer buff = CORTO_BUFFER_INIT;
    char *ptr, ch, prev = '\0';
    bool isNum = FALSE;
    char isStr = '\0';
    bool isVar = false;

    for (ptr = msg; (ch = *ptr); ptr++) {

        if (isNum && !isdigit(ch) && !isalpha(ch) && (ch != '.')) {
            corto_buffer_appendstr(&buff, CORTO_NORMAL);
            isNum = FALSE;
        }
        if (isStr && (isStr == ch) && !isalpha(ptr[1]) && (prev != '\\')) {
            isStr = '\0';
        } else if (((ch == '\'') || (ch == '"')) && !isStr && !isalpha(prev) && (prev != '\\')) {
            corto_buffer_appendstr(&buff, CORTO_CYAN);
            isStr = ch;
        }

        if ((isdigit(ch) || (ch == '-' && isdigit(ptr[1]))) && !isNum && !isStr && !isVar && !isalpha(prev) && !isdigit(prev) && (prev != '_') && (prev != '.')) {
            corto_buffer_appendstr(&buff, CORTO_GREEN);
            isNum = TRUE;
        }

        if (isVar && !isalpha(ch) && !isdigit(ch) && ch != '_') {
            corto_buffer_appendstr(&buff, CORTO_NORMAL);
            isVar = FALSE;
        }

        if (!isStr && !isVar && ch == '$' && isalpha(ptr[1])) {
            corto_buffer_appendstr(&buff, CORTO_MAGENTA);
            isVar = TRUE;
        }

        corto_buffer_appendstrn(&buff, ptr, 1);

        if (((ch == '\'') || (ch == '"')) && !isStr) {
            corto_buffer_appendstr(&buff, CORTO_NORMAL);
        }

        prev = ch;
    }

    if (isNum || isStr) {
        corto_buffer_appendstr(&buff, CORTO_NORMAL);
    }

    return corto_buffer_str(&buff);
}

static void corto_logprint_kind(corto_buffer *buf, corto_log_verbosity kind) {
    char *color, *levelstr;
    int levelspace;

    switch(kind) {
    case CORTO_THROW: color = CORTO_RED; levelstr = "throw"; break;
    case CORTO_ERROR: color = CORTO_RED; levelstr = "error"; break;
    case CORTO_WARNING: color = CORTO_YELLOW; levelstr = "warn"; break;
    case CORTO_INFO: color = CORTO_BLUE; levelstr = "info"; break;
    case CORTO_OK: color = CORTO_GREEN; levelstr = "ok"; break;
    case CORTO_TRACE: color = CORTO_GREY; levelstr = "trace"; break;
    case CORTO_DEBUG: color = CORTO_GREY; levelstr = "debug"; break;
    default: color = CORTO_RED; levelstr = "critical"; break;
    }

    if (corto_log_verbosityGet() <= CORTO_TRACE) {
        levelspace = 5;
    } else {
        levelspace = 4;
    }

    corto_buffer_append(
        buf, "%s%*s%s", color, levelspace, levelstr, CORTO_NORMAL);
}

static void corto_logprint_time(corto_buffer *buf, struct timespec t) {
    corto_buffer_append(buf, "%.9d.%.4d", t.tv_sec, t.tv_nsec / 100000);
}

static void corto_logprint_friendlyTime(corto_buffer *buf, struct timespec t) {
    corto_id tbuff;
    time_t sec = t.tv_sec;
    struct tm * timeinfo = localtime(&sec);
    strftime(tbuff, sizeof(tbuff), "%F %T", timeinfo);
    corto_buffer_append(buf, "%s.%.4d", tbuff, t.tv_nsec / 100000);
}

static int corto_logprint_categories(corto_buffer *buf, char *categories[]) {
    char *categoryStr = categories ? corto_log_categoryString(categories) : NULL;
    if (categoryStr) {
        int l = categoryStr[0] != 0;
        corto_buffer_appendstr(buf, categoryStr);
        corto_dealloc(categoryStr);
        return l;
    } else {
        return 0;
    }
}

static int corto_logprint_msg(corto_buffer *buf, char* msg) {
    char *tokenized = msg;
    if (!msg) {
        return 0;
    }

    if (!strchr(msg, '\033')) {
        tokenized = corto_log_tokenize(msg);
    }
    corto_buffer_appendstr(buf, tokenized);
    if (tokenized != msg) corto_dealloc(tokenized);

    return 1;
}

static int corto_logprint_file(corto_buffer *buf, char const *file) {
    if (file) {
        /* Strip any '..' tokens */
        if (file[0] == '.') {
            char const *ptr;
            char ch;
            for (ptr = file; (ch = *ptr); ptr++) {
                if (ch == '.' || ch == '/') {
                    file = ptr + 1;
                } else {
                    break;
                }
            }
        }

        corto_buffer_appendstr(buf, (char*)file);
        return 1;
    } else {
        return 0;
    }
}

static int corto_logprint_line(corto_buffer *buf, uint64_t line) {
    if (line) {
        corto_buffer_append(buf, "%s%u%s", CORTO_GREEN, line, CORTO_NORMAL);
        return 1;
    } else {
        return 0;
    }
}

static 
void corto_logprint(
    FILE *f, 
    corto_log_verbosity kind, 
    char *categories[], 
    char const *file,
    uint64_t line, 
    char *msg, 
    char *categoryStr) 
{
    size_t n = 0;
    corto_buffer buf = CORTO_BUFFER_INIT;
    char *fmtptr, ch;
    bool prevSeparatedBySpace = TRUE, separatedBySpace = FALSE;
    struct timespec now;
    timespec_gettime(&now);

    for (fmtptr = corto_log_fmt_current; (ch = *fmtptr); fmtptr++) {
        if (ch == '%' && fmtptr[1]) {
            int ret = 1;
            switch(fmtptr[1]) {
            case 'T': corto_logprint_friendlyTime(&buf, now); break;
            case 't': corto_logprint_time(&buf, now); break;
            case 'v': corto_logprint_kind(&buf, kind); break;
            case 'k': corto_logprint_kind(&buf, kind); break; /* Deprecated */
            case 'c': 
                if (categoryStr) corto_buffer_append(&buf, categoryStr); 
                else ret = corto_logprint_categories(&buf, categories); 
                break;
            case 'f': ret = corto_logprint_file(&buf, file); break;
            case 'l': ret = corto_logprint_line(&buf, line); break;
            case 'm': ret = corto_logprint_msg(&buf, msg); break;
            case 'a': corto_buffer_append(&buf, "%s%s%s", CORTO_CYAN, corto_log_appName, CORTO_NORMAL); break;
            case 'V': if (kind >= CORTO_WARNING || kind == CORTO_THROW) { corto_logprint_kind(&buf, kind); } else { ret = 0; } break;
            case 'F': if (kind >= CORTO_WARNING || kind == CORTO_THROW) { ret = corto_logprint_file(&buf, file); } else { ret = 0; } break;
            case 'L': if (kind >= CORTO_WARNING || kind == CORTO_THROW) { ret = corto_logprint_line(&buf, line); } else { ret = 0; } break;
            default:
                corto_buffer_appendstr(&buf, "%");
                corto_buffer_appendstrn(&buf, &fmtptr[1], 1);
                break;
            }

            if (fmtptr[2] == ' ') {
                separatedBySpace = TRUE;
            } else if (fmtptr[2] && (fmtptr[2] != '%') && (fmtptr[3] == ' ')) {
                separatedBySpace = TRUE;
            } else {
                separatedBySpace = FALSE;
            }

            if (!ret) {
                if (fmtptr[2] && (fmtptr[2] != '%')) {
                    if ((fmtptr[2] != ' ') && (fmtptr[3] == ' ') && prevSeparatedBySpace) {
                        fmtptr += 2;
                    } else {
                        fmtptr += 1;
                    }
                }
            }

            prevSeparatedBySpace = separatedBySpace;

            fmtptr += 1;
        } else {
            corto_buffer_appendstrn(&buf, &ch, 1);
        }
    }

    char *str = corto_buffer_str(&buf);

    n = strlen(str) + 1;
    if (n < 80) {
        n = 80 - n;
    } else {
        n = 0;
    }

    fprintf(f, "%s\n", str);

    corto_dealloc(str);
}

static char* corto_getLastError(void) {
    corto_errThreadData *data = corto_getThreadData();
    data->viewed = TRUE;
    return data->lastError;
}

static int corto_getLastErrorViewed(void) {
    corto_errThreadData *data = corto_getThreadData();
    return data->lastError ? data->viewed : TRUE;
}

static char* corto_getLastInfo(void) {
    corto_errThreadData *data = corto_getThreadData();
    data->viewed = TRUE;
    return data->lastInfo;
}

static void corto_setLastError(char *category, char const *file, unsigned int line, char* err) {
    corto_errThreadData *data = corto_getThreadData();
    if (!data->viewed && data->lastError) {
        data->viewed = TRUE; /* Prevent recursion */
        corto_logprint(stderr, CORTO_THROW, NULL, data->lastFile, data->lastLine, data->lastError, data->lastCategory);
    }
    if (data->lastFile) corto_dealloc(data->lastFile);
    if (data->lastError) corto_dealloc(data->lastError);
    if (data->backtrace) corto_dealloc(data->backtrace);
    if (data->lastCategory) corto_dealloc(data->lastCategory);
    data->lastError = err ? corto_strdup(err) : NULL;
    data->lastCategory = category ? corto_strdup(category) : NULL;
    data->lastFile = file ? corto_strdup(file) : NULL;
    data->lastLine = line;
    if (corto_log_verbosityGet() == CORTO_DEBUG) {
        data->backtrace = corto_backtraceString();
    }
    data->viewed = FALSE;
}

static void corto_setLastMessage(char* err) {
    corto_errThreadData *data = corto_getThreadData();
    if (data->lastInfo) corto_dealloc(data->lastInfo);
    data->lastInfo = err ? corto_strdup(err) : NULL;
}

static char* corto_log_parseComponents(char *categories[], char *msg) {
    char *ptr, *prev = msg, ch;
    int count = 0;
    corto_errThreadData *data = corto_getThreadData();

    while (data->categories[count]) {
        categories[count] = data->categories[count];
        count ++;
    }

    for (ptr = msg; (ch = *ptr) && (isalpha(ch) || isdigit(ch) || (ch == ':') || (ch == '/') || (ch == '_')); ptr++) {
        if ((ch == ':') && (ptr[1] == ' ')) {
            *ptr = '\0';
            categories[count ++] = prev;
            ptr ++;
            prev = ptr + 1;
            if (count == CORTO_MAX_LOG_COMPONENTS) {
                break;
            }
        }
    }

    categories[count] = NULL;

    return prev;
}

void corto_log_fmt(char *fmt) {
    if (corto_log_fmt_application) {
        free(corto_log_fmt_application);
    }

    corto_log_fmt_current = strdup(fmt);
    corto_log_fmt_application = corto_log_fmt_current;

    corto_setenv("CORTO_LOGFMT", "%s", corto_log_fmt_current);
}

corto_log_verbosity corto_logv(char const *file, unsigned int line, corto_log_verbosity kind, unsigned int level, char* fmt, va_list arg, FILE* f) {
    if (kind >= CORTO_LOG_LEVEL || corto_log_handlers) {
        char* alloc = NULL;
        char buff[CORTO_MAX_LOG + 1];
        char *categories[CORTO_MAX_LOG_COMPONENTS];
        size_t n = 0;

        char* msg = buff, *msgBody;
        va_list argcpy;
        va_copy(argcpy, arg); /* Make copy of arglist in
                               * case vsnprintf needs to be called twice */

        CORTO_UNUSED(level);

        if ((n = (vsnprintf(buff, CORTO_MAX_LOG, fmt, arg) + 1)) > CORTO_MAX_LOG) {
            alloc = corto_alloc(n + 2);
            vsnprintf(alloc, n, fmt, argcpy);
            msg = alloc;
        }

        msgBody = corto_log_parseComponents(categories, msg);

        if (kind >= CORTO_LOG_LEVEL) {
            corto_logprint(f, kind, categories, file, line, msgBody, NULL);
        }

        if (corto_log_handlers) {
            corto_mutex_lock(&corto_log_lock);
            if (corto_log_handlers) {
                corto_iter it = corto_ll_iter(corto_log_handlers);
                while (corto_iter_hasNext(&it)) {
                    corto_log_handler callback = corto_iter_next(&it);
                    corto_err_notifyCallkback(
                        callback,
                        categories,
                        kind,
                        msgBody);
                }
            }
            corto_mutex_unlock(&corto_log_lock);
        }

        if (alloc) {
            corto_dealloc(alloc);
        }
    }

    corto_seterr(NULL);

    return kind;
}

void _corto_assertv(char const *file, unsigned int line, unsigned int condition, char* fmt, va_list args) {
    if (!condition) {
        corto_logv(file, line, CORTO_ASSERT, 0, fmt, args, stderr);
        corto_backtrace(stderr);
        abort();
    }
}

void corto_criticalv(char const *file, unsigned int line, char* fmt, va_list args) {
    corto_logv(file, line, CORTO_CRITICAL, 0, fmt, args, stderr);
    corto_backtrace(stderr);
    fflush(stderr);
    abort();
}

void corto_debugv(char const *file, unsigned int line, char* fmt, va_list args) {
    corto_logv(file, line, CORTO_DEBUG, 0, fmt, args, stderr);
}

void corto_tracev(char const *file, unsigned int line, char* fmt, va_list args) {
    corto_logv(file, line, CORTO_TRACE, 0, fmt, args, stderr);
}

void corto_warningv(char const *file, unsigned int line, char* fmt, va_list args) {
    corto_logv(file, line, CORTO_WARNING, 0, fmt, args, stderr);
}

void corto_errorv(char const *file, unsigned int line, char* fmt, va_list args) {
    corto_logv(file, line, CORTO_ERROR, 0, fmt, args, stderr);
    if (CORTO_BACKTRACE_ENABLED || (corto_log_verbosityGet() == CORTO_DEBUG)) {
        corto_backtrace(stderr);
    }
}

void corto_okv(char const *file, unsigned int line, char* fmt, va_list args) {
    corto_logv(file, line, CORTO_OK, 0, fmt, args, stderr);
}

void corto_infov(char const *file, unsigned int line, char* fmt, va_list args) {
    corto_logv(file, line, CORTO_INFO, 0, fmt, args, stderr);
}

void corto_seterrv(char const *file, unsigned int line, char *fmt, va_list args) {
    char *err = NULL, *categories = NULL;
    corto_errThreadData *data = corto_getThreadData();
    int count = 0;

    if (data) {
        categories = corto_log_categoryString(data->categories);
    }

    if (fmt) {
        err = corto_vasprintf(fmt, args);
    }

    corto_setLastError(categories, file, line, err);

    if (fmt && ((corto_log_verbosityGet() == CORTO_DEBUG) || CORTO_APP_STATUS)) {
        if (CORTO_APP_STATUS == 1) {
            corto_error("error raised while starting up: %s", corto_lasterr());
        } else if (CORTO_APP_STATUS){
            corto_error("error raised while shutting down: %s", corto_lasterr());
        } else {
            corto_logprint(stderr, CORTO_THROW, NULL, file, line, err, NULL);
        }
        corto_backtrace(stderr);
    }

    if (err) corto_dealloc(err);
}

void corto_setmsgv(char *fmt, va_list args) {
    char *err = NULL;
    if (fmt) {
        err = corto_vasprintf(fmt, args);
    }
    corto_setLastMessage(err);
    corto_dealloc(err);
}

void _corto_debug(char const *file, unsigned int line, char* fmt, ...) {
    va_list arglist;

    va_start(arglist, fmt);
    corto_debugv(file, line, fmt, arglist);
    va_end(arglist);
}

void _corto_trace(char const *file, unsigned int line, char* fmt, ...) {
    va_list arglist;

    va_start(arglist, fmt);
    corto_tracev(file, line, fmt, arglist);
    va_end(arglist);
}

void _corto_info(char const *file, unsigned int line, char* fmt, ...) {
    va_list arglist;

    va_start(arglist, fmt);
    corto_infov(file, line, fmt, arglist);
    va_end(arglist);
}

void _corto_ok(char const *file, unsigned int line, char* fmt, ...) {
    va_list arglist;

    va_start(arglist, fmt);
    corto_okv(file, line, fmt, arglist);
    va_end(arglist);
}

void _corto_warning(char const *file, unsigned int line, char* fmt, ...) {
    va_list arglist;

    va_start(arglist, fmt);
    corto_warningv(file, line, fmt, arglist);
    va_end(arglist);
}

void _corto_error(char const *file, unsigned int line, char* fmt, ...) {
    va_list arglist;

    va_start(arglist, fmt);
    corto_errorv(file, line, fmt, arglist);
    va_end(arglist);
}

void _corto_critical(char const *file, unsigned int line, char* fmt, ...) {
    va_list arglist;

    va_start(arglist, fmt);
    corto_criticalv(file, line, fmt, arglist);
    va_end(arglist);
}

void _corto_assert(char const *file, unsigned int line, unsigned int condition, char* fmt, ...) {
    va_list arglist;

    va_start(arglist, fmt);
    _corto_assertv(file, line, condition, fmt, arglist);
    va_end(arglist);
}

char* corto_lasterr(void) {
    return corto_getLastError();
}

int corto_lasterrViewed(void) {
    return corto_getLastErrorViewed();
}


char* corto_lastinfo(void) {
    return corto_getLastInfo();
}

void _corto_seterr(char const *file, unsigned int line, char *fmt, ...) {
    va_list arglist;

    va_start(arglist, fmt);
    corto_seterrv(file, line, fmt, arglist);
    va_end(arglist);
}

void corto_setinfo(char *fmt, ...) {
    va_list arglist;

    va_start(arglist, fmt);
    corto_setmsgv(fmt, arglist);
    va_end(arglist);
}

void corto_log_verbositySet(corto_log_verbosity level) {

    switch(level) {
    case CORTO_DEBUG: corto_setenv("CORTO_VERBOSITY", "DEBUG"); break;
    case CORTO_TRACE: corto_setenv("CORTO_VERBOSITY", "TRACE"); break;
    case CORTO_OK: corto_setenv("CORTO_VERBOSITY", "OK"); break;
    case CORTO_INFO: corto_setenv("CORTO_VERBOSITY", "INFO"); break;
    case CORTO_WARNING: corto_setenv("CORTO_VERBOSITY", "WARNING"); break;
    case CORTO_ERROR: corto_setenv("CORTO_VERBOSITY", "ERROR"); break;
    case CORTO_CRITICAL: corto_setenv("CORTO_VERBOSITY", "CRITICAL"); break;
    case CORTO_ASSERT: corto_setenv("CORTO_VERBOSITY", "ASSERT"); break;
    default:
        corto_critical("invalid verbosity level %d", level);
        return;
    }

    CORTO_LOG_LEVEL = level;
}

corto_log_verbosity corto_log_verbosityGet() {
    return CORTO_LOG_LEVEL;
}

int corto_log_push(char *category) {
    corto_errThreadData *data = corto_getThreadData();
    int i = 0;
    while (data->categories[i] && (i < CORTO_MAX_LOG_COMPONENTS)) {
        i ++;
    }

    if (!data->categories[i]) {
        if (data->categories[i]) free(data->categories[i]);
        data->categories[i] = category ? strdup(category) : NULL;
        return 0;
    } else {
        return -1;
    }
}

void corto_log_pop(void) {
    corto_errThreadData *data = corto_getThreadData();
    int i = CORTO_MAX_LOG_COMPONENTS;
    while (!data->categories[i] && i) {
        i --;
    }

    if (data->categories[i]) free(data->categories[i]);
    data->categories[i] = NULL;
}

