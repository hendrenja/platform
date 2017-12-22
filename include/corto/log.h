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

/** @file
 * @section logging framework.
 * @brief Functions for throwing, catching, forwarding and printing logging.
 *
 * The logging framework is the basis for a multi-language approach that can be
 * used by languages/frameworks that do and do not support native exception handling.
 *
 * Usage of the framework is as follows:
 * - Nested- and library functions use corto_throw and corto_lasterr to log errors.
 *   This will store error messages in internal, thread-specific buffers until
 *   they are logged to the console by corto_error.
 *
 * - Top level functions use corto_error directly. This will
 *   log directly to the console (when verbosity level permits) and
 *   forward message to any registered handlers (always). Top level functions
 *   should use corto_lasterr when an error is caused by a nested call.
 *
 * When corto_throw is used, and corto_error is called without using corto_lasterr
 * the framework will report a warning to the console that an error has been
 * silenced. This protects users from accidentally missing error information.
 * When an application calls corto_stop and corto_lasterr has not been called
 * after calling corto_throw, the message is also logged to the console.
 *
 * The corto_info function should only be used by application-level functions
 * (not by functions in a library).
 *
 * The corto_ok, corto_trace and corto_debug functions may be used by both
 * library functions and application functions. These functions will only log to
 * the console when verbosity level permits.
 */

#ifndef CORE_LOG_H_
#define CORE_LOG_H_

#ifdef __cplusplus
extern "C" {
#endif



/* -- Setting & getting verbosity level -- */

/* Error verbosity levels */
typedef enum corto_log_verbosity {
    CORTO_THROW = -1,   /* used for internal debugging purposes */
    CORTO_DEBUG = 0,    /* implementation-specific tracing */
    CORTO_TRACE = 1,    /* progress/state of an application */
    CORTO_OK = 2,       /* successful completion of a task */
    CORTO_INFO = 3,     /* neutral message directed at user */
    CORTO_WARNING = 4,  /* issue that does not require immediate action */
    CORTO_ERROR = 5,    /* unsuccessful completion of a task */
    CORTO_CRITICAL = 6, /* task left application in undefined state (abort) */
    CORTO_ASSERT = 7    /* assertion failed (abort) */
} corto_log_verbosity;

/** Set verbosity level
 * The verbosity level is the same for all threads.
 * @param verbosity Verbosity level.
 */
CORTO_EXPORT
corto_log_verbosity corto_log_verbositySet(
    corto_log_verbosity verbosity);

/** Get verbosity level
 * The verbosity level is the same for all threads.
 * @return The current verbosity level.
 */
CORTO_EXPORT
corto_log_verbosity corto_log_verbosityGet(void);

/** Enable or disable colors.
 */
CORTO_EXPORT
bool corto_log_useColors(
    bool enable);

/** Enable or disable profiling.
 *
 * When profiling is enabled, categories will always be printed to the console.
 */
CORTO_EXPORT
bool corto_log_profile(
    bool enable);

/* -- Logging format used in console -- */

/** Default logging format. */
#define CORTO_LOGFMT_DEFAULT "%V %F:%L (%R) %C: %m"

/** Set logging format for console.
 * The following characters can be used for specifying the format:
 * -v Verbosity
 * -a Application name
 * -A Process id
 * -c Category
 * -C Category (tree view)
 * -m Formatted message
 * -t numerical representation of time (sec, nanosec)
 * -T user-friendly representation of time (1985-4-11 20:00:00.0000)
 * -d Time delta since last message
 * -f filename (use F for filename only in error messages)
 * -l line number (use L for linenumber only in error messages)
 * -r function (use R for function only in error messages)
 *
 * The logging format is the same for all threads. This function will also set
 * the CORTO_LOGFMT environment variable.
 *
 * @param fmt Format used for logging.
 */
CORTO_EXPORT
void corto_log_fmt(
    char *fmt);

CORTO_EXPORT
const char* corto_log_fmtGet(void);

/* -- Pushing/popping logging categories -- */

/** Push a category to the category stack.
 *
 * @param category Identifier of category.
 * @return Zero if success, non-zero if failed (max number of categories pushed).
 */
CORTO_EXPORT
int _corto_log_push(
    char const *file,
    unsigned int line,
    char const *function,
    const char *category);

/** Pop a category from the category stack.
 */
CORTO_EXPORT
void _corto_log_pop(
    char const *file,
    unsigned int line,
    char const *function);

/** Embed categories in logmessage or print them on push/pop
 *
 */
CORTO_EXPORT
void corto_log_embedCategories(
    bool embed);

/* -- Registering handlers for logging -- */

typedef void* corto_log_handler;
typedef void (*corto_log_handler_cb)(
    corto_log_verbosity level,
    char *category[],
    char *msg,
    void *ctx);

/** Register callback that catches log messages.
 * The parameters of this function specify filters for incoming messages.
 * @param min_level Minimum verbosity level. Set to DEBUG for all messages.
 * @param max_level Maximum verbosity level. Set to CRITICAL for all messages.
 * @param category_filter Filter on category using idmatch format. Set to "//" for all messages.
 * @param auth_token Specify authorization token when security is enabled.
 * @param callback Message handler callback.
 * @param context Generic value that will be passed to handler.
 * @return Handler object that can be used to unregister callback.
 */
CORTO_EXPORT
corto_log_handler corto_log_handlerRegister(
    corto_log_verbosity min_level,
    corto_log_verbosity max_level,
    char* category_filter,
    char* auth_token,
    corto_log_handler_cb callback,
    void *context);

/** Unregister a handler.
 *
 * @param handler The handler object.
 */
CORTO_EXPORT
void corto_log_handlerUnregister(
    corto_log_handler handler);

/** Check if any handlers are registered.
 *
 * @return true if handlers are registered, otherwise false.
 */
CORTO_EXPORT
bool corto_log_handlersRegistered(void);



/* -- Logging messages to console -- */

/** Log message with level CORTO_DEBUG.
 * This function should not be called directly, but through a macro as follows:
 *   corto_debug(fmt, ...)
 * The macro automatically adds information about the current file and line number.
 *
 * @param fmt A printf-style format string.
 */
CORTO_EXPORT
void _corto_debug(
    char const *file,
    unsigned int line,
    char const *function,
    const char* fmt,
    ...);

/** Log message with level CORTO_TRACE.
 * This function should not be called directly, but through a macro as follows:
 *   corto_trace(fmt, ...)
 * The macro automatically adds information about the current file and line number.
 *
 * @param fmt A printf-style format string.
 */
CORTO_EXPORT
void _corto_trace(
    char const *file,
    unsigned int line,
    char const *function,
    const char* fmt,
    ...);

/** Log message with level CORTO_INFO.
 * This function should not be called directly, but through a macro as follows:
 *   corto_info(fmt, ...)
 * The macro automatically adds information about the current file and line number.
 *
 * @param fmt A printf-style format string.
 */
CORTO_EXPORT
void _corto_info(
    char const *file,
    unsigned int line,
    char const *function,
    const char* fmt,
    ...);

/** Log message with level CORTO_OK.
 * This function should not be called directly, but through a macro as follows:
 *   corto_ok(fmt, ...)
 * The macro automatically adds information about the current file and line number.
 *
 * @param fmt A printf-style format string.
 */
CORTO_EXPORT
void _corto_ok(
    char const *file,
    unsigned int line,
    char const *function,
    const char* fmt,
    ...);

/** Log message with level CORTO_WARNING.
 * This function should not be called directly, but through a macro as follows:
 *   corto_warning(fmt, ...)
 * The macro automatically adds information about the current file and line number.
 *
 * @param fmt A printf-style format string.
 */
CORTO_EXPORT
void _corto_warning(
    char const *file,
    unsigned int line,
    char const *function,
    const char* fmt,
    ...);

/** Log message with level CORTO_ERROR.
 * This function should not be called directly, but through a macro as follows:
 *   corto_error(fmt, ...)
 * The macro automatically adds information about the current file and line number.
 *
 * @param fmt A printf-style format string.
 */
CORTO_EXPORT
void _corto_error(
    char const *file,
    unsigned int line,
    char const *function,
    const char* fmt,
    ...);

/** Log message with level CORTO_ASSERT.
 * This function should not be called directly, but through a macro as follows:
 *   corto_assert(condition, fmt, ...)
 * The macro automatically adds information about the current file and line number.
 *
 * When the condition is false, this function will print a stack trace and exit
 * the process using abort(). This function ignores the verbosity level.
 *
 * Asserts are disabled when built with NDEBUG.
 *
 * @param fmt A printf-style format string.
 * @param condition A condition to evaluate. When false, the process aborts.
 */
CORTO_EXPORT
void _corto_assert(
    char const *file,
    unsigned int line,
    char const *function,
    unsigned int condition,
    const char* fmt,
    ...);

/** Log message with level CORTO_CRITICAL.
 * This function should not be called directly, but through a macro as follows:
 *   corto_critical(fmt, ...)
 * The macro automatically adds information about the current file and line number.
 *
 * This function will abort the process after displaying the formatted message.
 * This function ignores the verbosity level.
 *
 * @param fmt A printf-style format string.
 */
CORTO_EXPORT
void _corto_critical(
    char const *file,
    unsigned int line,
    char const *function,
    const char* fmt,
    ...);

/** Overwrite log if below configured verbosity. */
CORTO_EXPORT
void _corto_log_overwrite(
    char const *file,
    unsigned int line,
    char const *function,
    corto_log_verbosity verbosity,
    const char *fmt,
    ...);

/** As corto_debug, but with va_list parameter. */
CORTO_EXPORT
void corto_debugv(
    char const *file,
    unsigned int line,
    char const *function,
    const char* fmt,
    va_list args);

/** As corto_trace, but with va_list parameter. */
CORTO_EXPORT
void corto_tracev(
    char const *file,
    unsigned int line,
    char const *function,
    const char* fmt,
    va_list args);

/** As corto_info, but with va_list parameter. */
CORTO_EXPORT
void corto_infov(
    char const *file,
    unsigned int line,
    char const *function,
    const char *fmt,
    va_list args);

/** As corto_ok, but with va_list parameter. */
CORTO_EXPORT
void corto_okv(
    char const *file,
    unsigned int line,
    char const *function,
    const char *fmt,
    va_list args);

/** As corto_warning, but with va_list parameter. */
CORTO_EXPORT
void corto_warningv(
    char const *file,
    unsigned int line,
    char const *function,
    const char *fmt,
    va_list args);

/** As corto_error, but with va_list parameter. */
CORTO_EXPORT
void corto_errorv(
    char const *file,
    unsigned int line,
    char const *function,
    const char *fmt,
    va_list args);

/** As corto_assert, but with va_list parameter. */
CORTO_EXPORT
void _corto_assertv(
    char const *file,
    unsigned int line,
    char const *function,
    unsigned int condition,
    const char *fmt,
    va_list args);

/** As corto_critical, but with va_list parameter. */
CORTO_EXPORT
void corto_criticalv(
    char const *file,
    unsigned int line,
    char const *function,
    const char *fmt,
    va_list args);



/* -- Exception handling -- */

/** Propagate error to calling function upon failure.
 * This error must always be called when a function fails. When the function
 * fails because it in turn called a function that failed, the function may
 * choose to not call corto_throw, but rely on the error propagated by the
 * failed function. It is good practice however to enrich errors with additional
 * context wherever possible.
 *
 * @param fmt printf-style format string.
 */
CORTO_EXPORT
void _corto_throw(
    char const *file,
    unsigned int line,
    char const *function,
    char *fmt,
    ...);

CORTO_EXPORT
void _corto_throw_fallback(
    char const *file,
    unsigned int line,
    char const *function,
    char *fmt,
    ...);

/* As corto_throw, but with va_list parameter. */
CORTO_EXPORT
void corto_throwv(
    char const *file,
    unsigned int line,
    char const *function,
    char *fmt,
    va_list args);

/** Add details to an exception */
CORTO_EXPORT
void corto_throw_detail(
    const char *fmt,
    ...);

/** Catch an exception.
 * If an exception is not catched, it will eventually be logged to the console.
 *
 * @return true if an exception was catched.
 */
CORTO_EXPORT
bool corto_catch(void);

/** Check if exception was raised.
 * @return true if an exception was raised.
 */
CORTO_EXPORT
bool corto_raised(void);

/** Raise an exception.
 */
CORTO_EXPORT
bool corto_raise(void);

/** Check for unraised exceptions (internal usage).
 */
CORTO_EXPORT
bool __corto_raise_check(void);

typedef enum corto_log_exceptionAction {
    CORTO_LOG_ON_EXCEPTION_IGNORE = 0,
    CORTO_LOG_ON_EXCEPTION_EXIT = 1,
    CORTO_LOG_ON_EXCEPTION_ABORT = 2
} corto_log_exceptionAction;

/** Specify what to do when exception is raised.
 */
CORTO_EXPORT
corto_log_exceptionAction corto_log_setExceptionAction(
    corto_log_exceptionAction action);


/** Propagate information to calling function.
 * This function is useful when a function wants to propagate a message that
 * is does not necessarily indicate an error. The framework will not report a
 * warning when this information is not viewed.
 */
CORTO_EXPORT void corto_setinfo(char *fmt, ...);

/** Retrieve last propagated information. */
CORTO_EXPORT
char* corto_lastinfo(void);

/* -- Print stacktraces -- */

/** Print current stacktrace to a file.
 *
 * @param f The file to which to print the stacktrace.
 */
CORTO_EXPORT
void corto_backtrace(
    FILE* f);


/* Stub */
CORTO_EXPORT
char *corto_lasterr();

/* -- Utilities -- */
CORTO_EXPORT
void corto_log(char *str, ...);

CORTO_EXPORT
void corto_log_tail(char *str, ...);

/* -- Helper macro's -- */

#define corto_critical(...) _corto_critical(__FILE__, __LINE__, CORTO_FUNCTION, __VA_ARGS__)
#define corto_critical_fl(file, line, ...) _corto_critical(file, line, CORTO_FUNCTION, __VA_ARGS__)

#define _SHOULD_PRINT(lvl)\
    corto_log_handlersRegistered() ||\
    corto_log_verbosityGet() <= lvl

#define corto_throw(...) _corto_throw(__FILE__, __LINE__, CORTO_FUNCTION, __VA_ARGS__)
#define corto_throw_fallback(...) _corto_throw_fallback(__FILE__, __LINE__, CORTO_FUNCTION, __VA_ARGS__)
#define corto_error(...) _corto_error(__FILE__, __LINE__, CORTO_FUNCTION, __VA_ARGS__)
#define corto_warning(...) _corto_warning(__FILE__, __LINE__, CORTO_FUNCTION, __VA_ARGS__)
#define corto_log_push(category) _corto_log_push(__FILE__, __LINE__, CORTO_FUNCTION, category);
#define corto_log_pop() _corto_log_pop(__FILE__, __LINE__, CORTO_FUNCTION);
#define corto_throw_fl(f, l, ...) _corto_throw(f, l, CORTO_FUNCTION, __VA_ARGS__)
#define corto_throw_fallback_fl(f, l, ...) _corto_throw_fallback(f, l, CORTO_FUNCTION, __VA_ARGS__)
#define corto_error_fl(f, l, ...) _corto_error(f, l, CORTO_FUNCTION, __VA_ARGS__)
#define corto_warning_fl(f, l, ...) _corto_warning(f, l, CORTO_FUNCTION, __VA_ARGS__)
#ifndef NDEBUG
#define corto_assert(condition, ...) if (!(condition)){_corto_assert(__FILE__, __LINE__, CORTO_FUNCTION, condition, "(" #condition ") " __VA_ARGS__);}
#define corto_debug(...) if(_SHOULD_PRINT(CORTO_DEBUG)) { _corto_debug(__FILE__, __LINE__, CORTO_FUNCTION, __VA_ARGS__);} else { __corto_raise_check(); }
#define corto_trace(...) if(_SHOULD_PRINT(CORTO_TRACE)) { _corto_trace(__FILE__, __LINE__, CORTO_FUNCTION, __VA_ARGS__);} else { __corto_raise_check(); }
#define corto_info(...) if(_SHOULD_PRINT(CORTO_INFO)) { _corto_info(__FILE__, __LINE__, CORTO_FUNCTION, __VA_ARGS__);} else { __corto_raise_check(); }
#define corto_ok(...) if(_SHOULD_PRINT(CORTO_OK)) { _corto_ok(__FILE__, __LINE__, CORTO_FUNCTION, __VA_ARGS__);} else { __corto_raise_check(); }
#define corto_log_overwrite(...) _corto_log_overwrite(__FILE__, __LINE__, CORTO_FUNCTION, __VA_ARGS__);
#else
#define corto_assert(condition, ...) (void)(condition)
#define corto_debug(...)
#define corto_trace(...)
#define corto_info(...)
#define corto_ok(...)
#endif

#ifdef __cplusplus
}
#endif

#endif /* CORE_ERR_H_ */
