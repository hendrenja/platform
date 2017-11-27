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

    /* Current category */
    char* categories[CORTO_MAX_LOG_CATEGORIES + 1];
    corto_log_frame frames[CORTO_MAX_LOG_CATEGORIES + 1];
    uint32_t sp;

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
    bool overrideColor = false;

    for (ptr = msg; (ch = *ptr); ptr++) {

        if (!overrideColor) {
            if (isNum && !isdigit(ch) && !isalpha(ch) && (ch != '.') && (ch != '%')) {
                corto_buffer_appendstr(&buff, CORTO_NORMAL);
                isNum = FALSE;
            }
            if (isStr && (isStr == ch) && prev != '\\') {
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
        }

        if (!isVar && !isStr && !isNum && ch == '#' && ptr[1] == '[') {
            bool isColor = true;
            overrideColor = true;

            /* Custom colors */
            if (!strncmp(&ptr[2], "green]", strlen("green]"))) {
                corto_buffer_appendstr(&buff, CORTO_GREEN);
            } else if (!strncmp(&ptr[2], "red]", strlen("red]"))) {
                corto_buffer_appendstr(&buff, CORTO_RED);
            } else if (!strncmp(&ptr[2], "blue]", strlen("red]"))) {
                corto_buffer_appendstr(&buff, CORTO_BLUE);
            } else if (!strncmp(&ptr[2], "magenta]", strlen("magenta]"))) {
                corto_buffer_appendstr(&buff, CORTO_MAGENTA);
            } else if (!strncmp(&ptr[2], "cyan]", strlen("cyan]"))) {
                corto_buffer_appendstr(&buff, CORTO_CYAN);
            } else if (!strncmp(&ptr[2], "yellow]", strlen("yellow]"))) {
                corto_buffer_appendstr(&buff, CORTO_YELLOW);
            } else if (!strncmp(&ptr[2], "grey]", strlen("grey]"))) {
                corto_buffer_appendstr(&buff, CORTO_GREY);
            } else if (!strncmp(&ptr[2], "white]", strlen("white]"))) {
                corto_buffer_appendstr(&buff, CORTO_NORMAL);
            } else if (!strncmp(&ptr[2], "bold]", strlen("bold]"))) {
                corto_buffer_appendstr(&buff, CORTO_BOLD);
            } else if (!strncmp(&ptr[2], "normal]", strlen("normal]"))) {
                corto_buffer_appendstr(&buff, CORTO_NORMAL);
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
        }

        corto_buffer_appendstrn(&buff, ptr, 1);

        if (!overrideColor) {
            if (((ch == '\'') || (ch == '"')) && !isStr) {
                corto_buffer_appendstr(&buff, CORTO_NORMAL);
            }
        }

        prev = ch;
    }

    if (isNum || isStr || isVar || overrideColor) {
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
        corto_buffer_append(
            buf,
            "%s%s%s",
            CORTO_GREY,
            (char*)corto_log_stripFunctionName(file),
            CORTO_NORMAL);
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
        corto_buffer_append(buf, "%s%s%s", CORTO_BLUE, function, CORTO_NORMAL);
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
        CORTO_GREEN,
        CORTO_YELLOW,
        CORTO_BLUE,
        CORTO_MAGENTA
        CORTO_CYAN,
        CORTO_WHITE,
        CORTO_GREY,
    };
    corto_proc id = corto_proc();
    corto_buffer_append(buf, "%s%u%s", colors[id % 6], id, CORTO_NORMAL);
    return 1;
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
    bool breakAtCategory)
{
    size_t n = 0;
    corto_buffer buf = CORTO_BUFFER_INIT, *cur;
    char *fmtptr, ch;
    corto_log_tlsData *data = corto_getThreadData();
    bool modified = false, stop = false;

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
            case 'C':
                if (breakAtCategory) {
                    stop = true;
                    break;
                }

                if (kind == CORTO_THROW) {
                    ret = 0;
                    break;
                }

                if (!corto_log_shouldEmbedCategories) {
                    int i = 0;

                    while (data->categories[i]) {
                        char *empty = "", *indent = empty;

                        if (i > 1) {
                            indent = corto_log_categoryIndent(data->categories, i - 1);
                        }

                        if (!data->frames[i].printed) {
                            if (i) {
                                corto_logprint(
                                    f, kind, categories, file, line, function, NULL, TRUE);
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
                                corto_logprint(
                                    f, kind, categories, file, line, function, NULL, TRUE);
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
                break;
            case 'f': ret = corto_logprint_file(cur, file); break;
            case 'l': ret = corto_logprint_line(cur, line); break;
            case 'r': ret = corto_logprint_function(cur, function); break;
            case 'm': ret = corto_logprint_msg(cur, msg); break;
            case 'a': corto_buffer_append(cur, "%s%s%s", CORTO_CYAN, corto_log_appName, CORTO_NORMAL); break;
            case 'A': ret = corto_logprint_proc(cur); break;
            case 'V': if (kind >= CORTO_WARNING || kind == CORTO_THROW) { corto_logprint_kind(cur, kind); } else { ret = 0; } break;
            case 'F': if (kind >= CORTO_WARNING || kind == CORTO_THROW) { ret = corto_logprint_file(cur, file); } else { ret = 0; } break;
            case 'L': if (kind >= CORTO_WARNING || kind == CORTO_THROW) { ret = corto_logprint_line(cur, line); } else { ret = 0; } break;
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

    char *str = corto_buffer_str(&buf);

    if (str) {
        n = strlen(str) + 1;
        if (n < 80) {
            n = 80 - n;
        } else {
            n = 0;
        }

        if (breakAtCategory) {
            fprintf(f, "%s", str);
        } else {
            fprintf(f, "%s\n", str);
        }
    }

    corto_dealloc(str);
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
    char *categories[],
    bool first)
{
    if (frame->category) {
        char *categoryStr = corto_log_categoryString(categories);
        if (categoryStr) {
            corto_buffer_append(buf, " %s", categoryStr);
            free(categoryStr);
        }
    }

    if (codeframe->file) {
        corto_buffer_appendstr(buf, " ");
        corto_logprint_file(buf, codeframe->file);
    }
    if (codeframe->line) {
        corto_buffer_appendstrn(buf, ":", 1);
        corto_logprint_line(buf, codeframe->line);
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
            printf("     %sfrom%s%s\n", CORTO_RED, CORTO_NORMAL, str);
        } else if (first) {
            printf("%sexception%s%s\n", CORTO_RED, CORTO_NORMAL, str);
        } else {
            printf("    %safter%s%s\n", CORTO_RED, CORTO_NORMAL, str);
        }
        free(str);
    }

    if (codeframe->detail) {
        corto_print("   #[grey]detail#[normal] %s\n", codeframe->detail);
    }
}

static
void corto_raise_intern(
    corto_log_tlsData *data,
    bool clearCategory)
{

    if (!data->viewed && data->exceptionCount) {
        int category, function, count = 0, total = 0;
        corto_buffer buf = CORTO_BUFFER_INIT;

        char *categories[CORTO_MAX_LOG_CATEGORIES];
        while (data->exceptionCategories[total]) {
            categories[total] = data->exceptionCategories[total];
            total ++;
        }
        categories[total] = NULL;

        for (category = 0; category < data->exceptionCount; category ++) {
            corto_log_frame *frame = &data->exceptionFrames[category];
            for (function = 0; function < frame->sp; function ++) {
                corto_log_codeframe *codeframe = &frame->frames[function];
                corto_raise_codeframe(&buf, frame, codeframe, categories, !count);
                count ++;
            }

            if (category != data->exceptionCount - 1) {
                corto_raise_codeframe(&buf, frame, &frame->initial, categories, !count);
                count ++;
            }

            categories[total - category - 1] = NULL;
            if (clearCategory) {
                data->exceptionCategories[total - category - 1] = NULL;
            }

            frame->sp = 0;
        }

        printf("     %sproc%s %s%s %s[%s%d%s]\n",
            CORTO_RED, CORTO_NORMAL, CORTO_GREY, corto_log_appName, CORTO_NORMAL, CORTO_GREY, corto_proc(), CORTO_NORMAL);
        printf("\n");

        if (clearCategory) {
            data->exceptionCount = 0;
        }

        data->viewed = true;
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
    char* error)
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
            printf("<< raise >>\n");
            corto_raise_intern(data, false);
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

    if (data->sp) {
        data->frames[data->sp - 1].count ++;
    }

    data->sp ++;

    return -1;
}

void corto_log_pop(void) {
    corto_log_tlsData *data = corto_getThreadData();

    if (data->sp) {
        corto_log_frame *frame = &data->frames[data->sp - 1];

        if (frame->category) free(frame->category);
        frame->sp = 0;

        data->frames[data->sp - 1].count += data->frames[data->sp].count;
        data->categories[data->sp - 1] = NULL;

        /* If categories are not embedded in log message, they are displayed in
         * a hierarchical view */
        if (!corto_log_shouldEmbedCategories && !data->exceptionCount) {

            /* Only print close if messages were logged for category */
            if (frame->printed) {
                char *indent = corto_log_categoryIndent(data->categories, 0);
                /* Print everything that preceeds the category */
                corto_logprint(
                    stderr, CORTO_INFO, data->categories, NULL, 0, NULL, NULL, TRUE);
                fprintf(
                    stderr,
                    "%s%s+%s\n", indent, CORTO_GREY, CORTO_NORMAL);
                free(indent);
            }
        }

        data->sp --;
    } else {
        corto_critical("corto_log_pop called more times than corto_log_push");
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
    if (!fmt || !strlen(fmt)) {
        return;
    }

    if (corto_log_fmt_application) {
        free(corto_log_fmt_application);
    }

    corto_log_fmt_current = strdup(fmt);
    corto_log_fmt_application = corto_log_fmt_current;

    corto_setenv("CORTO_LOGFMT", "%s", corto_log_fmt_current);

    char *ptr, ch;
    for (ptr = fmt; (ch = *ptr); ptr++) {
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
            corto_logprint(f, kind, categories, file, line, function, msgBody, FALSE);
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

void corto_throwv(
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
            corto_throw("error raised while starting up");
            corto_raise();
        } else if (CORTO_APP_STATUS){
            corto_throw("error raised while shutting down");
            corto_raise();
        } else {
            corto_logprint(stderr, CORTO_DEBUG, NULL, file, line, function, err, FALSE);
        }
    }

    if (err) corto_dealloc(err);
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
    corto_throwv(file, line, function, fmt, arglist);
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

void corto_catch(void)
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
    }
}

void corto_raise(void) {
    corto_log_tlsData *data = corto_getThreadData();
    corto_raise_intern(data, true);
}

void __corto_raise_check(void) {
    corto_log_tlsData *data = corto_getThreadData();
    corto_raise_intern(data, false);
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

void corto_print(char *fmt, ...) {
    va_list arglist;
    char *formatted, *colorized;

    va_start(arglist, fmt);
    formatted = corto_vasprintf(fmt, arglist);
    va_end(arglist);

    colorized = corto_log_colorize(formatted);
    printf("%s", colorized);
    free(colorized);
    free(formatted);
}
