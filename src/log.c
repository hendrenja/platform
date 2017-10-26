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

/* This global variable is set at startup and a public symbol */
char *corto_log_appName = "";

/* Variables used for synchronization & TLS */
extern corto_mutex_s corto_log_lock;
static corto_tls CORTO_KEY_LOG = 0;

/* List of log handlers (protected by lock) */
static corto_ll corto_log_handlers;

/* These global variables are shared across threads and are *not* protected by
 * a mutex. Libraries should not invoke functions that touch these, and an
 * application should set them during startup. */
static corto_log_verbosity CORTO_LOG_LEVEL = CORTO_INFO;
static char *corto_log_fmt_application;
static char *corto_log_fmt_current = CORTO_LOGFMT_DEFAULT;
static bool corto_log_shouldEmbedCategories = true;

/* Maximum stacktrace */
#define BACKTRACE_DEPTH 60

CORTO_EXPORT char* corto_backtraceString(void);
CORTO_EXPORT void corto_printBacktrace(FILE* f, int nEntries, char** symbols);

typedef struct corto_log_frame {
    char *category;
    char const *file;
    char const *function;
    unsigned int line;
    char *error;
    int count;
    bool printed;
} corto_log_frame;

typedef struct corto_log_tlsData {
    /* Last reported error data */
    char *lastInfo;
    corto_log_frame lastFrames[CORTO_MAX_LOG_CATEGORIES + 1];
    uint32_t lastFrameSp;
    char *backtrace;
    bool viewed;

    /* Current category */
    char* categories[CORTO_MAX_LOG_CATEGORIES + 1];
    corto_log_frame frames[CORTO_MAX_LOG_CATEGORIES + 1];
} corto_log_tlsData;

struct corto_log_handler {
    corto_log_verbosity min_level, max_level;
    char *category_filter;
    corto_idmatch_program compiled_category_filter;
    char *auth_token;
    void *ctx;
    corto_log_handler_cb cb;
};

static corto_log_tlsData* corto_getThreadData(void){
    corto_log_tlsData* result;
    result = corto_tls_get(CORTO_KEY_LOG);
    if (!result) {
        result = corto_calloc(sizeof(corto_log_tlsData));
        corto_tls_set(CORTO_KEY_LOG, result);
    }
    return result;
}

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

void corto_log_handlerUnregister(
    corto_log_handler cb)
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

void corto_printBacktrace(FILE* f, int nEntries, char** symbols) {
    int i;
    for(i=1; i<nEntries; i++) { /* Skip this function */
        fprintf(f, "  %s\n", symbols[i]);
    }
    fprintf(f, "\n");
}

void corto_backtrace(FILE* f) {
    int nEntries;
    void* buff[BACKTRACE_DEPTH];
    char** symbols;

    nEntries = backtrace(buff, BACKTRACE_DEPTH);
    if (nEntries) {
        symbols = backtrace_symbols(buff, BACKTRACE_DEPTH);

        corto_printBacktrace(f, nEntries, symbols);

        free(symbols);
    } else {
        fprintf(f, "obtaining backtrace failed.");
    }
}

char* corto_backtraceString(void) {
    int nEntries;
    void* buff[BACKTRACE_DEPTH];
    char** symbols;
    char* result;

    result = malloc(10000);
    *result = '\0';

    nEntries = backtrace(buff, BACKTRACE_DEPTH);
    if (nEntries) {
        symbols = backtrace_symbols(buff, BACKTRACE_DEPTH);

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

static 
char* corto_log_categoryIndent(
    char *categories[],
    int count)
{
    int i = 0;
    corto_buffer buff = CORTO_BUFFER_INIT;

    corto_buffer_appendstr(&buff, CORTO_GREY);

    while (categories[i] && (!count || i < count)) {
        i ++;
        corto_buffer_appendstr(&buff, "|  ");
    }

    corto_buffer_appendstr(&buff, CORTO_NORMAL);

    return corto_buffer_str(&buff);
}

static 
char* corto_log_categoryString(
    char *categories[]) 
{
    int32_t i = 0;
    corto_buffer buff = CORTO_BUFFER_INIT;

    if (categories) {
        while (categories[i]) {
            corto_buffer_append(&buff, "%s%s%s", 
                CORTO_MAGENTA, categories[i], CORTO_NORMAL);
            i ++;
            if (categories[i]) {
                corto_buffer_appendstr(&buff, ".");
            }
        }
    }

    return corto_buffer_str(&buff);
}

static 
char* corto_log_colorize(
    char *msg) 
{
    corto_buffer buff = CORTO_BUFFER_INIT;
    char *ptr, ch, prev = '\0';
    bool isNum = FALSE;
    char isStr = '\0';
    bool isVar = false;

    for (ptr = msg; (ch = *ptr); ptr++) {

        if (isNum && !isdigit(ch) && !isalpha(ch) && (ch != '.') && (ch != '%')) {
            corto_buffer_appendstr(&buff, CORTO_NORMAL);
            isNum = FALSE;
        }
        if (isStr && (isStr == ch) && !isalpha(ptr[1]) && (prev != '\\')) {
            isStr = '\0';
        } else if (((ch == '\'') || (ch == '"')) && !isStr && !isalpha(prev) && (prev != '\\')) {
            corto_buffer_appendstr(&buff, CORTO_CYAN);
            isStr = ch;
        }

        if ((isdigit(ch) || (ch == '%' && isdigit(prev)) || (ch == '-' && isdigit(ptr[1]))) && !isNum && !isStr && !isVar && !isalpha(prev) && !isdigit(prev) && (prev != '_') && (prev != '.')) {
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

static 
void corto_logprint_kind(
    corto_buffer *buf, 
    corto_log_verbosity kind) 
{
    char *color, *levelstr;
    int levelspace;

    switch(kind) {
    case CORTO_THROW: color = CORTO_RED; levelstr = "exception"; break;
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

static 
void corto_logprint_time(
    corto_buffer *buf, 
    struct timespec t) 
{
    corto_buffer_append(buf, "%.9d.%.4d", t.tv_sec, t.tv_nsec / 100000);
}

static 
void corto_logprint_friendlyTime(
    corto_buffer *buf, 
    struct timespec t) 
{
    corto_id tbuff;
    time_t sec = t.tv_sec;
    struct tm * timeinfo = localtime(&sec);
    strftime(tbuff, sizeof(tbuff), "%F %T", timeinfo);
    corto_buffer_append(buf, "%s.%.4d", tbuff, t.tv_nsec / 100000);
}

static 
int corto_logprint_categories(
    corto_buffer *buf, 
    char *categories[]) 
{
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

static 
int corto_logprint_msg(
    corto_buffer *buf, 
    char* msg) 
{
    char *tokenized = msg;
    if (!msg) {
        return 0;
    }

    if (!strchr(msg, '\033')) {
        tokenized = corto_log_colorize(msg);
    }
    corto_buffer_appendstr(buf, tokenized);
    if (tokenized != msg) corto_dealloc(tokenized);

    return 1;
}

static
char const* corto_log_stripFunctionName(
    char const *file)
{
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
        return ptr;
    } else {
        return file;
    }
}

static 
int corto_logprint_file(
    corto_buffer *buf, 
    char const *file) 
{
    if (file) {
        corto_buffer_appendstr(buf, (char*)corto_log_stripFunctionName(file));
        return 1;
    } else {
        return 0;
    }
}

static 
int corto_logprint_line(
    corto_buffer *buf, 
    uint64_t line) 
{
    if (line) {
        corto_buffer_append(buf, "%s%u%s", CORTO_GREEN, line, CORTO_NORMAL);
        return 1;
    } else {
        return 0;
    }
}

static 
int corto_logprint_function(
    corto_buffer *buf, 
    char const *function) 
{
    if (function) {
        corto_buffer_append(buf, "%s%s%s", CORTO_GREY, function, CORTO_NORMAL);
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
    char const *function,
    char *msg, 
    char *categoryStr) 
{
    size_t n = 0;
    corto_buffer buf = CORTO_BUFFER_INIT, *cur;
    char *fmtptr, ch;
    corto_log_tlsData *data = corto_getThreadData();
    bool modified = false;

    bool 
        prevSeparatedBySpace = TRUE, 
        separatedBySpace = FALSE, 
        inParentheses = FALSE;
    struct timespec now;
    timespec_gettime(&now);

    for (fmtptr = corto_log_fmt_current; (ch = *fmtptr); fmtptr++) {
        corto_buffer tmp = CORTO_BUFFER_INIT;
        if (inParentheses) {
            cur = &tmp;
        } else {
            cur = &buf;
        }

        if (ch == '%' && fmtptr[1]) {
            int ret = 1;
            switch(fmtptr[1]) {
            case 'T': corto_logprint_friendlyTime(cur, now); break;
            case 't': corto_logprint_time(cur, now); break;
            case 'v': corto_logprint_kind(cur, kind); break;
            case 'k': corto_logprint_kind(cur, kind); break; /* Deprecated */
            case 'c': 
                if (categoryStr) {
                    corto_buffer_append(cur, categoryStr); 
                } else {
                    if (!corto_log_shouldEmbedCategories) {
                        int i = 0; 
                        while (data->categories[i]) {
                            char *empty = "", *indent = empty;
                            if (i > 1) {
                                indent = corto_log_categoryIndent(data->categories, i - 1);
                            }
                            if (!data->frames[i].printed) {
                                if (i) {
                                    fprintf(
                                        stderr, 
                                        "%s%s├>%s %s%s%s\n", 
                                        indent, 
                                        CORTO_GREY,
                                        CORTO_NORMAL,
                                        CORTO_MAGENTA, 
                                        data->categories[i], 
                                        CORTO_NORMAL);
                                } else {
                                    fprintf(
                                        stderr, 
                                        "%s%s%s\n", 
                                        CORTO_MAGENTA, 
                                        data->categories[i], 
                                        CORTO_NORMAL);
                                }
                                data->frames[i].printed = true;
                            }
                            if (indent != empty) free(indent);
                            i ++;
                        }
                        if (i) data->frames[i - 1].count ++;

                        if (!modified) {
                            char *indent = corto_log_categoryIndent(data->categories, 0);
                            corto_buffer_appendstr(cur, indent);
                            free(indent);
                        }
                    }
                    ret = corto_logprint_categories(cur, categories);                     
                }
                break;
            case 'f': ret = corto_logprint_file(cur, file); break;
            case 'l': ret = corto_logprint_line(cur, line); break;
            case 'r': ret = corto_logprint_function(cur, function); break;
            case 'm': ret = corto_logprint_msg(cur, msg); break;
            case 'a': corto_buffer_append(cur, "%s%s%s", CORTO_CYAN, corto_log_appName, CORTO_NORMAL); break;
            case 'V': if (kind >= CORTO_WARNING || kind == CORTO_THROW) { corto_logprint_kind(cur, kind); } else { ret = 0; } break;
            case 'F': if (kind >= CORTO_WARNING || kind == CORTO_THROW) { ret = corto_logprint_file(cur, file); } else { ret = 0; } break;
            case 'L': if (kind >= CORTO_WARNING || kind == CORTO_THROW) { ret = corto_logprint_line(cur, line); } else { ret = 0; } break;
            case 'R': if (kind >= CORTO_WARNING || kind == CORTO_THROW) { ret = corto_logprint_function(cur, function); } else { ret = 0; } break;
            default:
                corto_buffer_appendstr(cur, "%");
                corto_buffer_appendstrn(cur, &fmtptr[1], 1);
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
            } else {
                if (inParentheses) {
                    corto_buffer_appendstrn(&buf, "(", 1);
                    char *str = corto_buffer_str(cur);
                    corto_buffer_appendstr(&buf, str);
                    free(str);
                }
            }

            modified = ret;
            prevSeparatedBySpace = separatedBySpace;
            fmtptr += 1;
            inParentheses = false;
        } else {
            if (ch == '(' && fmtptr[1] == '%' && (fmtptr[2] && fmtptr[2] != '%')) {
                inParentheses = true;
            } else {
                corto_buffer_appendstrn(&buf, &ch, 1);
                modified = true;
                inParentheses = false;
            }
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

static 
char* corto_getLastError(void) {
    corto_log_tlsData *data = corto_getThreadData();
    data->viewed = TRUE;
    return data->lastFrames[0].error;
}

static 
int corto_getLastErrorViewed(void) {
    corto_log_tlsData *data = corto_getThreadData();
    return data->lastFrames[0].error ? data->viewed : TRUE;
}

static 
char* corto_getLastInfo(void) {
    corto_log_tlsData *data = corto_getThreadData();
    data->viewed = TRUE;
    return data->lastInfo;
}

static
void corto_throw_lasterror(
    corto_log_tlsData *data) 
{
    if (!data->viewed && data->lastFrames[0].error) {
        data->viewed = TRUE;

        corto_logprint(
            stderr, 
            CORTO_THROW, 
            NULL,
            data->lastFrames[0].file,
            data->lastFrames[0].line,
            data->lastFrames[0].function,
            data->lastFrames[0].error,
            data->lastFrames[0].category);

        int i;
        corto_buffer buf = CORTO_BUFFER_INIT;

        /* Walk components in reverse order */
        for (i = 1; i < data->lastFrameSp; i++) {
            corto_logprint_file(&buf, data->lastFrames[i].file);
            if (data->lastFrames[i].line) {
                corto_buffer_appendstrn(&buf, ":", 1);
                corto_logprint_line(&buf, data->lastFrames[i].line);
            }
            corto_buffer_appendstrn(&buf, " (", 2);
            corto_logprint_function(&buf, data->lastFrames[i].function);
            corto_buffer_appendstrn(&buf, ")", 1);
            corto_buffer_append(&buf, " %s%s%s", CORTO_MAGENTA, data->lastFrames[i].category, CORTO_NORMAL);
            if (data->lastFrames[i].error) {
                corto_buffer_append(&buf, ": %s", data->lastFrames[i].error);     
            }
            char *str = corto_buffer_str(&buf);
            printf("     %sfrom%s %s\n", CORTO_RED, CORTO_NORMAL, str);
            free(str);
        }
        printf("\n");

    }
    data->lastFrameSp = 0;
}

static 
void corto_lasterrorFree(
    void* tls) 
{
    corto_log_tlsData* data = tls;
    if (data) {
        corto_throw_lasterror(data);

        int i;
        for (i = 0; i < CORTO_MAX_LOG_CATEGORIES; i++) {
            char *str;
            if ((str = data->lastFrames[i].error)) free(str);
            if ((str = data->lastFrames[i].category)) free(str);
            if ((str = data->categories[i])) free(str);
            if ((str = data->frames[i].error)) free(str);
            if ((str = data->frames[i].category)) free(str);
        }

        if (data->backtrace) {
            free(data->backtrace);
        }
    }
}

static 
void corto_log_setError(
    char *category, 
    char const *file, 
    unsigned int line, 
    char const *function, 
    char* error) 
{
    corto_log_tlsData *data = corto_getThreadData();
    if (data->backtrace) corto_dealloc(data->backtrace);

    if (!data->lastFrameSp) {
        if (error) {
            /* This is a top-level error */
        
            /* Clean up */
            int i;
            for (i = 0; data->lastFrames[i].file != NULL; i ++) {
                char *str;
                if ((str = data->lastFrames[i].category)) free(str);
                if ((str = data->lastFrames[i].error)) free(str);
                data->lastFrames[i].category = NULL;
                data->lastFrames[i].error = NULL;
                /* file & function do not have to be cleaned up since as they are
                 * builtin constants */
            }

            /* Set current error */
            data->lastFrames[0].file = file;
            data->lastFrames[0].line = line;
            data->lastFrames[0].function = function;
            data->lastFrames[0].category = category ? strdup(category) : NULL;
            data->lastFrames[0].error = error ? strdup(error) : NULL;

            /* Copy category frames */
            int count;
            for (count = 0; data->frames[count + 1].file != NULL; count++);
            for (i = 1; i <= count; i ++) {
                data->lastFrames[i] = data->frames[count - i];
                data->lastFrames[i].category = strdup(data->frames[count - i].category);
            }
            data->lastFrames[i].file = NULL;
        } else {
            /* Clear error */
            data->lastFrames[data->lastFrameSp].error = NULL;
        }
    } else {
        data->lastFrames[data->lastFrameSp].file = file;
        data->lastFrames[data->lastFrameSp].line = line;
        data->lastFrames[data->lastFrameSp].function = function;        
        data->lastFrames[data->lastFrameSp].error = error ? corto_log_colorize(error) : NULL;
    }

    if (corto_log_verbosityGet() == CORTO_DEBUG) {
        data->backtrace = corto_backtraceString();
    }
    data->viewed = FALSE;
}

static 
void corto_setLastMessage(
    char* err) 
{
    corto_log_tlsData *data = corto_getThreadData();
    if (data->lastInfo) corto_dealloc(data->lastInfo);
    data->lastInfo = err ? corto_strdup(err) : NULL;
}

static 
char* corto_log_parseComponents(
    char *categories[], 
    char *msg) 
{
    char *ptr, *prev = msg, ch;
    int count = 0;
    corto_log_tlsData *data = corto_getThreadData();

    if (corto_log_shouldEmbedCategories) {
        while (data->categories[count]) {
            categories[count] = data->categories[count];
            count ++;
        }
    }

    for (ptr = msg; (ch = *ptr) && (isalpha(ch) || isdigit(ch) || (ch == ':') || (ch == '/') || (ch == '_')); ptr++) {
        if ((ch == ':') && (ptr[1] == ' ')) {
            *ptr = '\0';
            categories[count ++] = prev;
            ptr ++;
            prev = ptr + 1;
            if (count == CORTO_MAX_LOG_CATEGORIES) {
                break;
            }
        }
    }

    categories[count] = NULL;

    return prev;
}

void corto_log_fmt(
    char *fmt) 
{
    if (corto_log_fmt_application) {
        free(corto_log_fmt_application);
    }

    corto_log_fmt_current = strdup(fmt);
    corto_log_fmt_application = corto_log_fmt_current;

    corto_setenv("CORTO_LOGFMT", "%s", corto_log_fmt_current);
}

corto_log_verbosity corto_logv(
    char const *file, 
    unsigned int line, 
    char const *function, 
    corto_log_verbosity kind, 
    unsigned int level, 
    char* fmt, 
    va_list arg, 
    FILE* f) 
{
    corto_log_tlsData *data = corto_getThreadData();
    corto_throw_lasterror(data);

    data->lastFrameSp = 0;

    if (kind >= CORTO_LOG_LEVEL || corto_log_handlers) {
        char* alloc = NULL;
        char buff[CORTO_MAX_LOG + 1];
        char *categories[CORTO_MAX_LOG_CATEGORIES];
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
            corto_logprint(f, kind, categories, file, line, function, msgBody, NULL);
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

void _corto_assertv(
    char const *file, 
    unsigned int line, 
    char const *function,    
    unsigned int condition, 
    char* fmt, 
    va_list args) 
{
    if (!condition) {
        corto_logv(file, line, function, CORTO_ASSERT, 0, fmt, args, stderr);
        corto_backtrace(stderr);
        abort();
    }
}

void corto_criticalv(
    char const *file, 
    unsigned int line, 
    char const *function,        
    char* fmt, 
    va_list args) 
{
    corto_logv(file, line, function, CORTO_CRITICAL, 0, fmt, args, stderr);
    corto_backtrace(stderr);
    fflush(stderr);
    abort();
}

void corto_debugv(
    char const *file, 
    unsigned int line,
    char const *function, 
    char* fmt, 
    va_list args) 
{
    corto_logv(file, line, function, CORTO_DEBUG, 0, fmt, args, stderr);
}

void corto_tracev(
    char const *file, 
    unsigned int line, 
    char const *function,
    char* fmt, 
    va_list args) 
{
    corto_logv(file, line, function, CORTO_TRACE, 0, fmt, args, stderr);
}

void corto_warningv(
    char const *file, 
    unsigned int line,
    char const *function,
    char* fmt, 
    va_list args) 
{
    corto_logv(file, line, function, CORTO_WARNING, 0, fmt, args, stderr);
}

void corto_errorv(
    char const *file, 
    unsigned int line, 
    char const *function,
    char* fmt, 
    va_list args) 
{
    corto_logv(file, line, function, CORTO_ERROR, 0, fmt, args, stderr);
    if (CORTO_BACKTRACE_ENABLED || (corto_log_verbosityGet() == CORTO_DEBUG)) {
        corto_backtrace(stderr);
    }
}

void corto_okv(
    char const *file, 
    unsigned int line, 
    char const *function,
    char* fmt, 
    va_list args) 
{
    corto_logv(file, line, function, CORTO_OK, 0, fmt, args, stderr);
}

void corto_infov(
    char const *file, 
    unsigned int line, 
    char const *function,
    char* fmt, 
    va_list args) 
{
    corto_logv(file, line, function, CORTO_INFO, 0, fmt, args, stderr);
}

void corto_seterrv(
    char const *file, 
    unsigned int line, 
    char const *function,
    char *fmt, 
    va_list args) 
{
    char *err = NULL, *categories = NULL;
    corto_log_tlsData *data = corto_getThreadData();

    if (data) {
        categories = corto_log_categoryString(data->categories);
    }

    if (fmt) {
        err = corto_vasprintf(fmt, args);
    }

    corto_log_setError(categories, file, line, function, err);

    if (fmt && ((corto_log_verbosityGet() == CORTO_DEBUG) || CORTO_APP_STATUS)) {
        if (CORTO_APP_STATUS == 1) {
            corto_error("error raised while starting up: %s", corto_lasterr());
        } else if (CORTO_APP_STATUS){
            corto_error("error raised while shutting down: %s", corto_lasterr());
        } else {
            corto_logprint(stderr, CORTO_DEBUG, NULL, file, line, function, err, NULL);
        }
    }

    if (err) corto_dealloc(err);
}

void corto_setmsgv(
    char *fmt, 
    va_list args) 
{
    char *err = NULL;
    if (fmt) {
        err = corto_vasprintf(fmt, args);
    }
    corto_setLastMessage(err);
    corto_dealloc(err);
}

void _corto_debug(
    char const *file, 
    unsigned int line, 
    char const *function,
    char* fmt, 
    ...) 
{
    va_list arglist;

    va_start(arglist, fmt);
    corto_debugv(file, line, function, fmt, arglist);
    va_end(arglist);
}

void _corto_trace(
    char const *file, 
    unsigned int line, 
    char const *function,
    char* fmt, 
    ...) 
{
    va_list arglist;

    va_start(arglist, fmt);
    corto_tracev(file, line, function, fmt, arglist);
    va_end(arglist);
}

void _corto_info(
    char const *file, 
    unsigned int line,
    char const *function, 
    char* fmt, ...) 
{
    va_list arglist;

    va_start(arglist, fmt);
    corto_infov(file, line, function, fmt, arglist);
    va_end(arglist);
}

void _corto_ok(
    char const *file, 
    unsigned int line, 
    char const *function,
    char* fmt, 
    ...) 
{
    va_list arglist;

    va_start(arglist, fmt);
    corto_okv(file, line, function, fmt, arglist);
    va_end(arglist);
}

void _corto_warning(
    char const *file, 
    unsigned int line, 
    char const *function,    
    char* fmt, 
    ...) 
{
    va_list arglist;

    va_start(arglist, fmt);
    corto_warningv(file, line, function, fmt, arglist);
    va_end(arglist);
}

void _corto_error(
    char const *file, 
    unsigned int line, 
    char const *function,
    char* fmt, 
    ...) 
{
    va_list arglist;

    va_start(arglist, fmt);
    corto_errorv(file, line, function, fmt, arglist);
    va_end(arglist);
}

void _corto_critical(
    char const *file, 
    unsigned int line, 
    char const *function,
    char* fmt, 
    ...) 
{
    va_list arglist;

    va_start(arglist, fmt);
    corto_criticalv(file, line, function, fmt, arglist);
    va_end(arglist);
}

void _corto_assert(
    char const *file, 
    unsigned int line, 
    char const *function,
    unsigned int condition, 
    char* fmt, 
    ...) 
{
    va_list arglist;

    va_start(arglist, fmt);
    _corto_assertv(file, line, function, condition, fmt, arglist);
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

void _corto_seterr(
    char const *file, 
    unsigned int line, 
    char const *function,
    char *fmt, 
    ...) 
{
    va_list arglist;

    va_start(arglist, fmt);
    corto_seterrv(file, line, function, fmt, arglist);
    va_end(arglist);
}

void corto_setinfo(
    char *fmt, 
    ...) 
{
    va_list arglist;

    va_start(arglist, fmt);
    corto_setmsgv(fmt, arglist);
    va_end(arglist);
}

void corto_log_verbositySet(
    corto_log_verbosity level) 
{
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

int _corto_log_push(
    char const *file, 
    unsigned int line,
    char const *function,    
    const char *category) 
{
    corto_log_tlsData *data = corto_getThreadData();
    
    /* Clear any errors before pushing a new stack */
    corto_throw_lasterror(data);

    int i = 0;
    while (data->categories[i] && (i < CORTO_MAX_LOG_CATEGORIES)) {
        i ++;
    }

    if (!data->categories[i]) {
        if (data->categories[i]) free(data->categories[i]);
        data->categories[i] = category ? strdup(category) : NULL;
        data->frames[i].category = strdup(data->categories[i]);
        data->frames[i].file = corto_log_stripFunctionName(file);
        data->frames[i].line = 0; /* Line is only set if an error is thrown for this category */
        data->frames[i].function = function;
        data->frames[i].count = 0;
        data->frames[i].printed = false;

        if (i) {
            data->frames[i - 1].count ++;
        }

        return 0;
    } else {
        return -1;
    }
}

void corto_log_pop(void) {
    corto_log_tlsData *data = corto_getThreadData();

    int i = CORTO_MAX_LOG_CATEGORIES;
    while (!data->categories[i] && i) {
        i --;
    }

    if (data->lastFrames[0].error) {
        data->lastFrameSp ++;
    }

    if (data->categories[i]) free(data->categories[i]);
    if (data->frames[i].category) free(data->frames[i].category);
    data->categories[i] = NULL;
    data->frames[i].category = NULL;
    data->frames[i].file = NULL;
    data->frames[i].line = 0; 

    if (i) {
        data->frames[i - 1].count += data->frames[i].count;
    }

    if (!corto_log_shouldEmbedCategories) {
        if (data->frames[i].printed) {
            if (i) {
                char *indent = corto_log_categoryIndent(data->categories, 0);
                if (data->frames[i].count) {
                    fprintf(
                        stderr, 
                        "%s%s+%s\n", indent, CORTO_GREY, CORTO_NORMAL);
                }
                free(indent);
            } else {
                fprintf(stderr, "%s+%s\n", CORTO_GREY, CORTO_NORMAL);
            }
        }
    }    
}

void corto_log_embedCategories(
    bool embed)
{
    corto_log_shouldEmbedCategories = embed;
}

int16_t corto_log_init(void) {
    return corto_tls_new(&CORTO_KEY_LOG, corto_lasterrorFree);
}
