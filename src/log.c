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
static corto_log_verbosity CORTO_LOG_TAIL_LEVEL = CORTO_CRITICAL;
static bool CORTO_LOG_PROFILE = false;
static corto_log_exceptionAction CORTO_LOG_EXCEPTION_ACTION = 0;
static char *corto_log_fmt_application;
static char *corto_log_fmt_current = CORTO_LOGFMT_DEFAULT;
static bool corto_log_shouldEmbedCategories = true;
static bool CORTO_LOG_USE_COLORS = true;

/* Maximum stacktrace */
#define BACKTRACE_DEPTH 60

CORTO_EXPORT char* corto_backtraceString(void);
CORTO_EXPORT void corto_printBacktrace(FILE* f, int nEntries, char** symbols);

/* One frame for each function where corto_throw is called */
typedef struct corto_log_codeframe {
    char *file;
    char *function;
    unsigned int line;
    char *error;
    char *detail;
    bool thrown; /* Is frame thrown or copied from category stack */
} corto_log_codeframe;

/* One frame for each category */
typedef struct corto_log_frame {
    char *category;
    int count;
    bool printed;

    /* The initial frame contains where log_push was called */
    corto_log_codeframe initial;

    /* The frames array contains the seterr calles for the current category */
    corto_log_codeframe frames[CORTO_MAX_LOG_CODEFRAMES];
    uint32_t sp;
    struct timespec lastTime;
} corto_log_frame;

/* Main thread-specific log administration type */
typedef struct corto_log_tlsData {
    /* Last reported error data */
    char *lastInfo;
    char *exceptionCategories[CORTO_MAX_LOG_CATEGORIES + 1];
    corto_log_frame exceptionFrames[CORTO_MAX_LOG_CATEGORIES + 1];
    uint32_t exceptionCount;
    bool viewed;
    char *backtrace;
    uint16_t last_printed_len;

    /* Current category */
    char* categories[CORTO_MAX_LOG_CATEGORIES + 1];
    corto_log_frame frames[CORTO_MAX_LOG_CATEGORIES + 1];
    uint32_t sp;

    /* Last reported time (used for computing deltas) */
    struct timespec lastTime;

    /* Detect if program is unwinding stack in case error was reported */
    void *stack_marker;
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
    if (!CORTO_KEY_LOG) {
        fprintf(
            stderr,
            "*** CORTO_KEY_LOG not initialized! Run corto_start first ***\n");
        abort();
    }

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
            corto_throw("invalid filter");
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
        if (!corto_ll_count(corto_log_handlers)) {
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
        fprintf(stderr, "obtaining backtrace failed.");
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

    while (categories[i] && (!count || i < count)) {
        i ++;
        corto_buffer_appendstr(&buff, "#[grey]|#[normal]  ");
    }

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
            corto_buffer_append(&buff, "#[green]%s#[normal]", categories[i]);
            i ++;
            if (categories[i]) {
                corto_buffer_appendstr(&buff, ".");
            }
        }
    }

    return corto_buffer_str(&buff);
}

static
size_t printlen(
    const char *str)
{
    const char *ptr;
    char ch;
    int len = 0;
    for (ptr = str; (ch = *ptr); ptr++) {
        if (ch == '\033') {
            ptr += 7;
        }
        if (ch == '\xF0') {
            ptr += 2;
        }
        len ++;
        if (!*ptr) break;
    }
    return len;
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
    bool overrideColor = false;
    bool autoColor = true;

    for (ptr = msg; (ch = *ptr); ptr++) {

        if (!overrideColor) {
            if (isNum && !isdigit(ch) && !isalpha(ch) && (ch != '.') && (ch != '%')) {
                if (CORTO_LOG_USE_COLORS) corto_buffer_appendstr(&buff, CORTO_NORMAL);
                isNum = FALSE;
            }
            if (isStr && (isStr == ch) && prev != '\\') {
                isStr = '\0';
            } else if (((ch == '\'') || (ch == '"')) && !isStr && !isalpha(prev) && (prev != '\\')) {
                if (CORTO_LOG_USE_COLORS) corto_buffer_appendstr(&buff, CORTO_CYAN);
                isStr = ch;
            }

            if ((isdigit(ch) || (ch == '%' && isdigit(prev)) || (ch == '-' && isdigit(ptr[1]))) && !isNum && !isStr && !isVar && !isalpha(prev) && !isdigit(prev) && (prev != '_') && (prev != '.')) {
                if (CORTO_LOG_USE_COLORS) corto_buffer_appendstr(&buff, CORTO_GREEN);
                isNum = TRUE;
            }

            if (isVar && !isalpha(ch) && !isdigit(ch) && ch != '_') {
                if (CORTO_LOG_USE_COLORS) corto_buffer_appendstr(&buff, CORTO_NORMAL);
                isVar = FALSE;
            }

            if (!isStr && !isVar && ch == '$' && isalpha(ptr[1])) {
                if (CORTO_LOG_USE_COLORS) corto_buffer_appendstr(&buff, CORTO_CYAN);
                isVar = TRUE;
            }
        }

        if (!isVar && !isStr && !isNum && ch == '#' && ptr[1] == '[') {
            bool isColor = true;
            overrideColor = true;

            /* Custom colors */
            if (!strncmp(&ptr[2], "]", strlen("]"))) {
                autoColor = false;
            } else if (!strncmp(&ptr[2], "green]", strlen("green]"))) {
                if (CORTO_LOG_USE_COLORS) corto_buffer_appendstr(&buff, CORTO_GREEN);
            } else if (!strncmp(&ptr[2], "red]", strlen("red]"))) {
                if (CORTO_LOG_USE_COLORS) corto_buffer_appendstr(&buff, CORTO_RED);
            } else if (!strncmp(&ptr[2], "blue]", strlen("red]"))) {
                if (CORTO_LOG_USE_COLORS) corto_buffer_appendstr(&buff, CORTO_BLUE);
            } else if (!strncmp(&ptr[2], "magenta]", strlen("magenta]"))) {
                if (CORTO_LOG_USE_COLORS) corto_buffer_appendstr(&buff, CORTO_MAGENTA);
            } else if (!strncmp(&ptr[2], "cyan]", strlen("cyan]"))) {
                if (CORTO_LOG_USE_COLORS) corto_buffer_appendstr(&buff, CORTO_CYAN);
            } else if (!strncmp(&ptr[2], "yellow]", strlen("yellow]"))) {
                if (CORTO_LOG_USE_COLORS) corto_buffer_appendstr(&buff, CORTO_YELLOW);
            } else if (!strncmp(&ptr[2], "grey]", strlen("grey]"))) {
                if (CORTO_LOG_USE_COLORS) corto_buffer_appendstr(&buff, CORTO_GREY);
            } else if (!strncmp(&ptr[2], "white]", strlen("white]"))) {
                if (CORTO_LOG_USE_COLORS) corto_buffer_appendstr(&buff, CORTO_NORMAL);
            } else if (!strncmp(&ptr[2], "bold]", strlen("bold]"))) {
                if (CORTO_LOG_USE_COLORS) corto_buffer_appendstr(&buff, CORTO_BOLD);
            } else if (!strncmp(&ptr[2], "normal]", strlen("normal]"))) {
                if (CORTO_LOG_USE_COLORS) corto_buffer_appendstr(&buff, CORTO_NORMAL);
                overrideColor = false;
            } else {
                isColor = false;
                overrideColor = false;
            }

            if (isColor) {
                ptr += 2;
                while ((ch = *ptr) != ']') ptr ++;
                if (!(ch = *(++ptr))) {
                    break;
                }
            }
            if (!autoColor) {
                overrideColor = true;
            }
        }

        corto_buffer_appendstrn(&buff, ptr, 1);

        if (!overrideColor) {
            if (((ch == '\'') || (ch == '"')) && !isStr) {
                if (CORTO_LOG_USE_COLORS) corto_buffer_appendstr(&buff, CORTO_NORMAL);
            }
        }

        prev = ch;
    }

    if (isNum || isStr || isVar || overrideColor) {
        if (CORTO_LOG_USE_COLORS) corto_buffer_appendstr(&buff, CORTO_NORMAL);
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
    case CORTO_THROW: color = "#[red]"; levelstr = "exception"; break;
    case CORTO_ERROR: color = "#[red]"; levelstr = "error"; break;
    case CORTO_WARNING: color = "#[yellow]"; levelstr = "warn"; break;
    case CORTO_INFO: color = "#[blue]"; levelstr = "info"; break;
    case CORTO_OK: color = "#[green]"; levelstr = "ok"; break;
    case CORTO_TRACE: color = "#[grey]"; levelstr = "trace"; break;
    case CORTO_DEBUG: color = "#[grey]"; levelstr = "debug"; break;
    default: color = "#[red]"; levelstr = "critical"; break;
    }

    if (corto_log_verbosityGet() <= CORTO_TRACE) {
        levelspace = 5;
    } else {
        levelspace = 4;
    }

    corto_buffer_append(
        buf, "%s%*s#[normal]", color, levelspace, levelstr);
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
void corto_logprint_deltaTime(
    corto_buffer *buf,
    struct timespec now,
    corto_log_tlsData *data,
    bool printCategory)
{
    corto_buffer_appendstr(buf, "#[grey]");
    if (CORTO_LOG_PROFILE) {
        corto_buffer_appendstr(buf, " --------");
    } else {
        if (data->lastTime.tv_sec || (data->sp && !printCategory)) {
            struct timespec delta;
            if (data->sp && !data->frames[data->sp - 1].printed && !printCategory) {
                delta = timespec_sub(now, data->frames[data->sp - 1].lastTime);
            } else {
                delta = timespec_sub(now, data->lastTime);
            }

            corto_buffer_append(buf, "+%.2d.%.5d", delta.tv_sec, delta.tv_nsec / 10000);
        } else {
            corto_buffer_appendstr(buf, " --------");
        }
    }
    corto_buffer_appendstr(buf, "#[normal]");
}

static
bool corto_logprint_sumTime(
    corto_buffer *buf,
    struct timespec now,
    corto_log_tlsData *data)
{
    corto_log_frame *frame = &data->frames[data->sp - 1];
    if (frame->lastTime.tv_sec) {
        struct timespec delta = timespec_sub(now, frame->lastTime);
        if (!delta.tv_sec && delta.tv_nsec < 50000) {
            return false;
        }
        corto_buffer_append(buf, " #[green]%.2d.%.5d#[normal]", delta.tv_sec, delta.tv_nsec / 10000);
    } else {
        corto_buffer_appendstr(buf, " --------");
    }
    return true;
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
    if (!msg) {
        return 0;
    }
    corto_buffer_appendstr(buf, msg);
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

#define CORTO_LOG_FILE_LEN (20)

static
int corto_logprint_file(
    corto_buffer *buf,
    char const *file,
    bool fixedWidth)
{
    file = corto_log_stripFunctionName(file);
    if (file) {
        if (!fixedWidth) {
            corto_buffer_append(buf, "#[cyan]%s#[normal]", file);
        } else {
            int len = strlen(file);
            if (len > (CORTO_LOG_FILE_LEN)) {
                file += len - CORTO_LOG_FILE_LEN + 2;
                corto_buffer_append(buf, "#[cyan]..%*s#[normal]", CORTO_LOG_FILE_LEN - 2, file);
            } else {
                corto_buffer_append(buf, "#[cyan]%*s#[normal]", CORTO_LOG_FILE_LEN, file);
            }
        }
        return 1;
    } else {
        return 0;
    }
}

static
int corto_logprint_line(
    corto_buffer *buf,
    uint64_t line,
    bool fixedWidth)
{
    if (line) {
        corto_buffer_append(buf, "#[green]%u#[normal]", line);
        if (fixedWidth) {
            int len = 4 - (floor(log10(line)) + 1);
            if (len) {
                corto_buffer_append(buf, "%*s", len, "");
            }
        }
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
        corto_buffer_append(buf, "#[cyan]%s#[normal]", function);
        return 1;
    } else {
        return 0;
    }
}

static
int corto_logprint_proc(
    corto_buffer *buf)
{
    char *colors[] = {
        "green",
        "yellow",
        "blue",
        "magenta"
        "cyan",
        "white",
        "grey",
    };
    corto_proc id = corto_proc();
    corto_buffer_append(buf, "#[%s]%u#[normal]", colors[id % 6], id);
    return 1;
}

static
void corto_log_resetCursor(
    corto_log_tlsData *data)
{
    int i;
    for (i = 0; i < data->last_printed_len; i ++) {
        fprintf(stderr, "\b");
    }
}

static
void corto_log_clearLine(
    corto_log_tlsData *data)
{
    int i;
    for (i = 0; i < data->last_printed_len; i ++) {
        fprintf(stderr, " ");
    }
    for (i = 0; i < data->last_printed_len; i ++) {
        fprintf(stderr, "\b");
    }
    data->last_printed_len = 0;

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
    uint16_t breakAtCategory,
    bool closeCategory)
{
    corto_buffer buf = CORTO_BUFFER_INIT, *cur;
    char *fmtptr, ch;
    corto_log_tlsData *data = corto_getThreadData();
    bool modified = false, stop = false;
    bool isTail = kind < CORTO_LOG_LEVEL;

    bool
        prevSeparatedBySpace = TRUE,
        separatedBySpace = FALSE,
        inParentheses = FALSE;
    struct timespec now;

    if (!breakAtCategory || closeCategory) {
        timespec_gettime(&now);
    } else {
        now = data->frames[breakAtCategory - 1].lastTime;
    }

    corto_log_clearLine(data);

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
            case 'd':
                if (closeCategory) {
                    if (!corto_logprint_sumTime(cur, now, data)) {
                        if (CORTO_LOG_PROFILE) {
                            stop = true;
                            break;
                        }
                    }
                } else {
                    corto_logprint_deltaTime(cur, now, data, breakAtCategory);
                }
                break;
            case 'T':
                corto_logprint_friendlyTime(cur, now);
                break;
            case 't': corto_logprint_time(cur, now); break;
            case 'v': corto_logprint_kind(cur, kind); break;
            case 'c':
            case 'C':
                if (breakAtCategory) {
                    stop = true;
                    break;
                }

                if (kind == CORTO_THROW) {
                    ret = 0;
                    break;
                }

                if (!isTail && !corto_log_shouldEmbedCategories) {
                    int i = 0;

                    while (data->categories[i]) {
                        char *empty = "", *indent = empty;

                        if (i > 1) {
                            indent = corto_log_categoryIndent(data->categories, i - 1);
                        }

                        if (!data->frames[i].printed) {
                            bool computeSum = !data->categories[i + 1] && !msg;
                            if (i) {
                                corto_logprint(
                                    f, kind, categories, file, line, function, NULL, i + 1, computeSum);
                                corto_log(
                                    "%s#[grey]├>#[normal] #[green]%s#[normal]\n",
                                    indent,
                                    data->categories[i]);
                            } else {
                                corto_logprint(
                                    f, kind, categories, file, line, function, NULL, i + 1, computeSum);
                                corto_log(
                                    "#[green]%s#[normal]\n",
                                    data->categories[i]);
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
                break;
            case 'f': ret = corto_logprint_file(cur, file, !corto_log_shouldEmbedCategories); break;
            case 'l': ret = corto_logprint_line(cur, line, !corto_log_shouldEmbedCategories); break;
            case 'r': ret = corto_logprint_function(cur, function); break;
            case 'm': ret = corto_logprint_msg(cur, msg); break;
            case 'a': corto_buffer_append(cur, "#[cyan]%s#[normal]", corto_log_appName); break;
            case 'A': ret = corto_logprint_proc(cur); break;
            case 'V': if (kind >= CORTO_WARNING || kind == CORTO_THROW) { corto_logprint_kind(cur, kind); } else { ret = 0; } break;
            case 'F': if (kind >= CORTO_WARNING || kind == CORTO_THROW) { ret = corto_logprint_file(cur, file, FALSE); } else { ret = 0; } break;
            case 'L': if (kind >= CORTO_WARNING || kind == CORTO_THROW) { ret = corto_logprint_line(cur, line, FALSE); } else { ret = 0; } break;
            case 'R': if (kind >= CORTO_WARNING || kind == CORTO_THROW) { ret = corto_logprint_function(cur, function); } else { ret = 0; } break;
            default:
                corto_buffer_appendstr(cur, "%");
                corto_buffer_appendstrn(cur, &fmtptr[1], 1);
                break;
            }

            if (stop) {
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

    data->lastTime = now;

    char *str = corto_buffer_str(&buf);

    if (str) {
        char *colorized = corto_log_colorize(str);
        if (breakAtCategory) {
            fprintf(f, "%s", colorized);
        } else {
            if (isTail) {
                fprintf(f, "%s", colorized);
                data->last_printed_len = printlen(colorized);
                corto_log_resetCursor(data);
            } else {
                if (msg) {
                    fprintf(f, "%s\n", colorized);
                }
            }
        }
        free(colorized);
        free(str);
    }
}

static
char* corto_getLastInfo(void) {
    corto_log_tlsData *data = corto_getThreadData();
    return data->lastInfo;
}

static
void corto_raise_codeframe(
    corto_buffer *buf,
    corto_log_frame *frame,
    corto_log_codeframe *codeframe,
    bool first)
{
    if (frame->category) {
        if (frame->category) {
            corto_buffer_append(buf, " %s%s%s", CORTO_GREY, frame->category, CORTO_NORMAL);
        }
    }

    if (codeframe->file) {
        corto_buffer_appendstr(buf, " ");
        corto_logprint_file(buf, codeframe->file, FALSE);
    }
    if (codeframe->line) {
        corto_buffer_appendstrn(buf, ":", 1);
        corto_logprint_line(buf, codeframe->line, FALSE);
    }
    if (codeframe->function) {
        corto_buffer_appendstrn(buf, " (", 2);
        corto_logprint_function(buf, codeframe->function);
        corto_buffer_appendstrn(buf, ")", 1);
    }
    if (codeframe->error) {
        corto_buffer_append(buf, ": %s", codeframe->error);
    }

    char *str = corto_buffer_str(buf);
    if (str) {
        if (codeframe->thrown && !first) {
            fprintf(stderr, "     %sfrom%s%s\n", CORTO_RED, CORTO_NORMAL, str);
        } else if (first) {
            fprintf(stderr, "%sexception%s%s\n", CORTO_RED, CORTO_NORMAL, str);
        } else {
            fprintf(stderr, "    %safter%s%s\n", CORTO_RED, CORTO_NORMAL, str);
        }
        free(str);
    }

    if (codeframe->detail) {
        corto_log("   #[grey]detail#[normal] %s\n", codeframe->detail);
    }
}

static
bool corto_raise_intern(
    corto_log_tlsData *data,
    bool clearCategory)
{
    if (!data->viewed && data->exceptionCount && (CORTO_LOG_LEVEL <= CORTO_ERROR)) {
        int category, function, count = 0, total = data->exceptionCount;
        corto_buffer buf = CORTO_BUFFER_INIT;

        for (category = 0; category < data->exceptionCount; category ++) {
            corto_log_frame *frame = &data->exceptionFrames[category];
            for (function = 0; function < frame->sp; function ++) {
                corto_log_codeframe *codeframe = &frame->frames[function];
                corto_raise_codeframe(&buf, frame, codeframe, !count);
                count ++;
            }

            if (category != data->exceptionCount - 1) {
                corto_raise_codeframe(&buf, frame, &frame->initial, !count);
                count ++;
            }

            if (clearCategory) {
                data->exceptionCategories[total - category - 1] = NULL;
            }

            frame->sp = 0;
        }

        fprintf(stderr, "     %sproc%s %s%s %s[%s%d%s]\n\n",
            CORTO_RED, CORTO_NORMAL, CORTO_GREY, corto_log_appName, CORTO_NORMAL, CORTO_GREY, corto_proc(), CORTO_NORMAL);

        if (clearCategory) {
            data->exceptionCount = 0;
        }

        data->viewed = true;

        if (CORTO_LOG_EXCEPTION_ACTION == CORTO_LOG_ON_EXCEPTION_EXIT) {
            exit(-1);
        } else if (CORTO_LOG_EXCEPTION_ACTION == CORTO_LOG_ON_EXCEPTION_ABORT) {
            abort();
        }

        return true;
    } else {
        return false;
    }
}

static
void corto_frame_free(
    corto_log_frame *frame)
{
    char *str;
    int i;
    for (i = 0; i < frame->sp; i ++) {
        corto_log_codeframe *codeframe = &frame->frames[i];
        if ((str = codeframe->error)) free(str);
        if ((str = codeframe->file)) free(str);
        if ((str = codeframe->function)) free(str);
        if ((str = codeframe->detail)) free(str);
    }
    frame->sp = 0;
}

static
void corto_lasterrorFree(
    void* tls)
{
    corto_log_tlsData* data = tls;
    if (data) {
        corto_raise_intern(data, true);
        int i;
        for (i = 0; i < data->sp; i ++) {
            corto_frame_free(&data->frames[i]);
        }
        for (i = 0; i < data->exceptionCount; i ++) {
            corto_frame_free(&data->exceptionFrames[i]);
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
    char* error,
    bool raiseUnreported)
{
    int stack_marker; /* Use this variable to check if moving up or down on the stack */
    (void)stack_marker;

    corto_log_tlsData *data = corto_getThreadData();
    if (data->backtrace) corto_dealloc(data->backtrace);

    data->viewed = false;

    if (!data->exceptionCount && !data->exceptionFrames[0].sp) {
        data->stack_marker = (void*)&stack_marker;

        /* Copy all current frames to exception cache in reverse order */
        int i;
        for (i = 1; i <= data->sp; i ++) {
            data->exceptionFrames[i - 1] = data->frames[data->sp - i];
            data->exceptionFrames[i - 1].category = strdup(data->frames[data->sp - i].category);
            data->exceptionFrames[i - 1].initial.file = strdup(data->frames[data->sp - i].initial.file);
            data->exceptionFrames[i - 1].initial.function = strdup(data->frames[data->sp - i].initial.function);
            data->exceptionFrames[i - 1].sp = 0;
        }
        data->exceptionCount = data->sp + 1;

        /* Copy category stack in normal order */
        for (i = 1; i <= data->sp; i ++) {
            data->exceptionCategories[i - 1] = data->exceptionFrames[data->sp - i].category;
        }

        /* Array must end with a NULL */
        data->exceptionCategories[i - 1] = NULL;

        /* Set error in top-level frame */
        data->exceptionFrames[0].frames[0].file = strdup(file);
        data->exceptionFrames[0].frames[0].function = strdup(function);
        data->exceptionFrames[0].frames[0].line = line;
        data->exceptionFrames[0].frames[0].error = error ? corto_log_colorize(error) : NULL;
        data->exceptionFrames[0].frames[0].thrown = true;
        data->exceptionFrames[0].sp = 1;

    } else {
        corto_assert(
            data->exceptionCount != 0,
            "no active exception to append to (sp = %d)",
            data->exceptionFrames[0].sp);

        if (data->stack_marker >= (void*)&stack_marker) {
            /* If stack marker is higher than the current stack, program is
             * traveling up the stack after an exception was reported. */
            if (raiseUnreported) {
                corto_raise_intern(data, false);
            } else {
                /* Do not append new error, as it is reported as fallback */
            }
        } else {
            /* Add another level to the error stack */
            corto_assert(
                data->exceptionCount > data->sp,
                "the total number of exceptions must be larger than the current stack"
            );

            corto_log_frame *frame = &data->exceptionFrames[data->exceptionCount - data->sp - 1];

            corto_assert(frame->sp < CORTO_MAX_LOG_CODEFRAMES, "max number of code frames reached");

            frame->frames[frame->sp].file = strdup(file);
            frame->frames[frame->sp].function = strdup(function);
            frame->frames[frame->sp].line = line;
            frame->frames[frame->sp].error = error ? corto_log_colorize(error) : NULL;
            frame->frames[frame->sp].thrown = true;
            frame->sp ++;
        }
    }
}

int _corto_log_push(
    char const *file,
    unsigned int line,
    char const *function,
    const char *category)
{
    corto_log_tlsData *data = corto_getThreadData();

    corto_assert(data->sp < CORTO_MAX_LOG_CATEGORIES, "cannot push category '%s', max nested categories reached(%d)",
        CORTO_MAX_LOG_CATEGORIES);

    /* Clear any errors before pushing a new stack */
    corto_raise_intern(data, false);

    corto_log_frame *frame = &data->frames[data->sp];

    frame->category = category ? strdup(category) : NULL;
    data->categories[data->sp] = frame->category;
    frame->count = 0;
    frame->printed = false;
    frame->initial.file = strdup(corto_log_stripFunctionName(file));
    frame->initial.function = strdup(function);
    frame->initial.line = line;
    frame->initial.thrown = false;
    frame->sp = 0;
    timespec_gettime(&frame->lastTime);

    if (data->sp) {
        data->frames[data->sp - 1].count ++;
    }

    data->sp ++;

    return -1;
}

void _corto_log_pop(
    char const *file,
    unsigned int line,
    char const *function)
{
    corto_log_tlsData *data = corto_getThreadData();
    bool printed = false;

    if (data->sp) {
        corto_log_frame *frame = &data->frames[data->sp - 1];

        if (!frame->printed && CORTO_LOG_PROFILE) {
            printed = true;
            corto_logprint(
                stderr, CORTO_INFO, NULL, file, line, function, NULL, FALSE, TRUE);
        }

        if (strcmp(frame->initial.function, function)) {
            corto_warning_fl(
                file,
                line,
                "log_pop called in '%s' but matching log_push in '%s'",
                function, frame->initial.function);
        }

        if (frame->initial.file) free(frame->initial.file);
        if (frame->initial.function) free(frame->initial.function);
        if (frame->category) free(frame->category);
        frame->sp = 0;

        data->frames[data->sp - 1].count += data->frames[data->sp].count;
        data->categories[data->sp - 1] = NULL;

        /* If categories are not embedded in log message, they are displayed in
         * a hierarchical view */
        if (!corto_log_shouldEmbedCategories && !data->exceptionCount) {

            /* Only print close if messages were logged for category */
            if (frame->printed && !printed) {
                char *indent = corto_log_categoryIndent(data->categories, 0);
                /* Print everything that preceeds the category */
                corto_logprint(
                    stderr, CORTO_INFO, data->categories, file, line, NULL, NULL, TRUE, TRUE);
                corto_log(
                    "%s#[grey]+#[normal]\n", indent ? indent : "");
                if (indent) free(indent);
            }
        }

        data->sp --;
    } else {
        corto_critical_fl(file, line, "corto_log_pop called more times than corto_log_push");
    }
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
    bool newValue = true;
    if (!fmt || !strlen(fmt)) {
        newValue = false;
    }

    if (newValue && corto_log_fmt_application) {
        free(corto_log_fmt_application);
    }

    if (newValue) {
        corto_log_fmt_current = strdup(fmt);
        corto_log_fmt_application = corto_log_fmt_current;
    }

    corto_setenv("CORTO_LOGFMT", "%s", corto_log_fmt_current);

    char *ptr, ch;
    for (ptr = corto_log_fmt_current; (ch = *ptr); ptr++) {
        if (ch == '%') {
            if (ptr[1] == 'C') {
                corto_log_embedCategories(false);
            } else if (ptr[1] == 'c') {
                corto_log_embedCategories(true);
            }
        }
    }
}

const char* corto_log_fmtGet(void)
{
    return corto_log_fmt_current;
}

corto_log_verbosity corto_logv(
    char const *file,
    unsigned int line,
    char const *function,
    corto_log_verbosity kind,
    unsigned int level,
    const char *fmt,
    va_list arg,
    FILE* f)
{
    corto_log_tlsData *data = corto_getThreadData();
    corto_raise_intern(data, false);

    if (kind >= CORTO_LOG_LEVEL || kind >= CORTO_LOG_TAIL_LEVEL || corto_log_handlers) {
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

        if (kind >= CORTO_LOG_LEVEL || kind >= CORTO_LOG_TAIL_LEVEL) {
            corto_logprint(f, kind, categories, file, line, function, msgBody, FALSE, FALSE);
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

    corto_catch();

    return kind;
}

void _corto_assertv(
    char const *file,
    unsigned int line,
    char const *function,
    unsigned int condition,
    const char *fmt,
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
    const char *fmt,
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
    const char *fmt,
    va_list args)
{
    corto_logv(file, line, function, CORTO_DEBUG, 0, fmt, args, stderr);
}

void corto_tracev(
    char const *file,
    unsigned int line,
    char const *function,
    const char *fmt,
    va_list args)
{
    corto_logv(file, line, function, CORTO_TRACE, 0, fmt, args, stderr);
}

void corto_warningv(
    char const *file,
    unsigned int line,
    char const *function,
    const char *fmt,
    va_list args)
{
    corto_logv(file, line, function, CORTO_WARNING, 0, fmt, args, stderr);
}

void corto_errorv(
    char const *file,
    unsigned int line,
    char const *function,
    const char *fmt,
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
    const char *fmt,
    va_list args)
{
    corto_logv(file, line, function, CORTO_OK, 0, fmt, args, stderr);
}

void corto_infov(
    char const *file,
    unsigned int line,
    char const *function,
    const char *fmt,
    va_list args)
{
    corto_logv(file, line, function, CORTO_INFO, 0, fmt, args, stderr);
}

static
void corto_throwv_intern(
    char const *file,
    unsigned int line,
    char const *function,
    char *fmt,
    va_list args,
    bool raiseUnreported)
{
    char *err = NULL, *categories = NULL;
    corto_log_tlsData *data = corto_getThreadData();

    if (data) {
        categories = corto_log_categoryString(data->categories);
    }

    if (fmt) {
        err = corto_vasprintf(fmt, args);
    }

    corto_log_setError(categories, file, line, function, err, raiseUnreported);

    if (fmt && ((corto_log_verbosityGet() == CORTO_DEBUG) || CORTO_APP_STATUS)) {
        if (CORTO_APP_STATUS == 1) {
            corto_throw("error raised while starting up");
            corto_raise();
        } else if (CORTO_APP_STATUS){
            corto_throw("error raised while shutting down");
            corto_raise();
        } else {
            corto_logprint(stderr, CORTO_DEBUG, NULL, file, line, function, err, FALSE, FALSE);
        }
    }

    if (err) corto_dealloc(err);
}

void corto_throwv(
    char const *file,
    unsigned int line,
    char const *function,
    char *fmt,
    va_list args)
{
    corto_throwv_intern(file, line, function, fmt, args, TRUE);
}

void corto_throw_detailv(
    const char *fmt,
    va_list args)
{
    corto_log_tlsData *data = corto_getThreadData();

    corto_assert(
        data->exceptionCount != 0,
        "no active exception to append to (sp = %d)",
        data->exceptionFrames[0].sp);

    if (fmt) {
        corto_log_frame *frame = &data->exceptionFrames[data->exceptionCount - data->sp - 1];
        corto_assert(frame->sp > 0, "no codeframe to attach detail to");
        char *detail = corto_vasprintf(fmt, args);
        frame->frames[frame->sp - 1].detail = detail;
    }
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
    const char *fmt,
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
    const char *fmt,
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
    const char *fmt, ...)
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
    const char *fmt,
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
    const char *fmt,
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
    const char *fmt,
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
    const char *fmt,
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
    const char *fmt,
    ...)
{
    va_list arglist;

    va_start(arglist, fmt);
    _corto_assertv(file, line, function, condition, fmt, arglist);
    va_end(arglist);
}

char* corto_lastinfo(void) {
    return corto_getLastInfo();
}

void _corto_throw(
    char const *file,
    unsigned int line,
    char const *function,
    char *fmt,
    ...)
{
    va_list arglist;

    va_start(arglist, fmt);
    corto_throwv_intern(file, line, function, fmt, arglist, TRUE);
    va_end(arglist);
}

void _corto_throw_fallback(
    char const *file,
    unsigned int line,
    char const *function,
    char *fmt,
    ...)
{
    va_list arglist;

    va_start(arglist, fmt);
    corto_throwv_intern(file, line, function, fmt, arglist, FALSE);
    va_end(arglist);
}

void corto_throw_detail(
    const char *fmt,
    ...)
{
    va_list arglist;

    va_start(arglist, fmt);
    corto_throw_detailv(fmt, arglist);
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

bool corto_catch(void)
{
    /* Clear exception */
    corto_log_tlsData *data = corto_getThreadData();
    int i;
    if (data->exceptionCount) {
        for (i = 0; i < data->exceptionCount; i ++) {
            corto_log_frame *frame = &data->exceptionFrames[i];
            corto_frame_free(frame);
        }
        data->exceptionCount = 0;
        return true;
    } else {
        return false;
    }
}

bool corto_raise(void) {
    corto_log_tlsData *data = corto_getThreadData();
    return corto_raise_intern(data, true);
}

bool __corto_raise_check(void) {
    corto_log_tlsData *data = corto_getThreadData();
    return corto_raise_intern(data, false);
}

static
char *corto_log_levelToStr(
    corto_log_verbosity level)
{
    switch(level) {
    case CORTO_DEBUG: return "DEBUG";
    case CORTO_TRACE: return "TRACE";
    case CORTO_OK: return "OK";
    case CORTO_INFO: return "INFO";
    case CORTO_WARNING: return "WARNING";
    case CORTO_ERROR: return "ERROR";
    case CORTO_CRITICAL: return "CRITICAL";
    case CORTO_ASSERT: return "ASSERT";
    default:
        corto_critical("invalid verbosity level %d", level);
        return NULL;
    }
}

corto_log_verbosity corto_log_verbositySet(
    corto_log_verbosity level)
{
    corto_log_verbosity old = CORTO_LOG_LEVEL;
    corto_setenv("CORTO_VERBOSITY", corto_log_levelToStr(level));
    CORTO_LOG_LEVEL = level;
    return old;
}

corto_log_verbosity corto_log_tailVerbositySet(
    corto_log_verbosity level)
{
    corto_log_verbosity old = CORTO_LOG_LEVEL;
    corto_setenv("CORTO_TAIL_VERBOSITY", corto_log_levelToStr(level));
    CORTO_LOG_TAIL_LEVEL = level;
    return old;
}

corto_log_verbosity corto_log_verbosityGet() {
    return CORTO_LOG_LEVEL;
}

corto_log_verbosity corto_log_tailVerbosityGet() {
    return CORTO_LOG_TAIL_LEVEL;
}

void corto_log_embedCategories(
    bool embed)
{
    corto_log_shouldEmbedCategories = embed;
}

int16_t corto_log_init(void) {
    return corto_tls_new(&CORTO_KEY_LOG, corto_lasterrorFree);
}

char *corto_lasterr() {
    corto_catch();
    return "< lasterr deprecated, replace with corto_catch or corto_raise >";
}

void corto_log(char *fmt, ...) {
    va_list arglist;
    char *formatted, *colorized;
    corto_log_tlsData *data = corto_getThreadData();
    int len;

    corto_log_clearLine(data);

    va_start(arglist, fmt);
    formatted = corto_vasprintf(fmt, arglist);
    va_end(arglist);

    colorized = corto_log_colorize(formatted);
    len = printlen(colorized);
    fprintf(stderr, "%s", colorized);

    /* If no newline is printed, keep track of how many backtrace characters
     * need to ba appended before printing the next log statement */
    if (colorized[len - 1] != '\n') {
        data->last_printed_len = len;
        corto_log_resetCursor(data);
        fflush(stderr);
    }

    free(colorized);
    free(formatted);
}

bool corto_log_profile(
    bool enable)
{
    bool result = CORTO_LOG_PROFILE;
    CORTO_LOG_PROFILE = enable;
    if (enable) {
        corto_setenv("CORTO_LOG_PROFILE", "TRUE");
    } else {
        corto_setenv("CORTO_LOG_PROFILE", "FALSE");
    }
    return result;
}

corto_log_exceptionAction corto_log_setExceptionAction(
    corto_log_exceptionAction action)
{
    corto_log_exceptionAction result = CORTO_LOG_EXCEPTION_ACTION;
    CORTO_LOG_EXCEPTION_ACTION = action;
    return result;
}

bool corto_log_useColors(
    bool enable)
{
    bool result = CORTO_LOG_USE_COLORS;
    CORTO_LOG_USE_COLORS = enable;
    return result;
}
