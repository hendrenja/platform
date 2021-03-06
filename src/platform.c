#include "base.h"

int8_t CORTO_APP_STATUS = 3;
int8_t CORTO_BACKTRACE_ENABLED = 0;

/* Lock to protect global administration related to logging framework */
corto_mutex_s corto_log_lock;
corto_mutex_s corto_load_lock;

extern char *corto_log_appName;

corto_tls CORTO_KEY_THREAD_STRING;

void platform_init(char *appName) {
    corto_log_appName = appName;

    if (corto_mutex_new(&corto_log_lock)) {
        corto_critical("failed to create mutex for logging framework");
    }

    if (corto_mutex_new(&corto_load_lock)) {
        corto_critical("failed to create mutex for package loader");
    }

    void corto_threadStringDealloc(void *data);

    if (corto_tls_new(&CORTO_KEY_THREAD_STRING, corto_threadStringDealloc)) {
        corto_critical("failed to obtain tls key for thread admin");
    }

    if (corto_log_init()) {
        corto_critical("failed to initialize logging framework");
    }

    char *verbosity = corto_getenv("CORTO_VERBOSITY");
    if (verbosity) {
        if (!strcmp(verbosity, "DEBUG")) {
            corto_log_verbositySet(CORTO_DEBUG);
        }
        if (!strcmp(verbosity, "TRACE")) {
            corto_log_verbositySet(CORTO_TRACE);
        }
        if (!strcmp(verbosity, "OK")) {
            corto_log_verbositySet(CORTO_OK);
        }
        if (!strcmp(verbosity, "INFO")) {
            corto_log_verbositySet(CORTO_INFO);
        }
        if (!strcmp(verbosity, "WARNING")) {
            corto_log_verbositySet(CORTO_WARNING);
        }
        if (!strcmp(verbosity, "ERROR")) {
            corto_log_verbositySet(CORTO_ERROR);
        }
        if (!strcmp(verbosity, "CRITICAL")) {
            corto_log_verbositySet(CORTO_CRITICAL);
        }
        if (!strcmp(verbosity, "ASSERT")) {
            corto_log_verbositySet(CORTO_ASSERT);
        }
    }

    corto_log_fmt(corto_getenv("CORTO_LOGFMT"));

    char *profile = corto_getenv("CORTO_LOG_PROFILE");
    if (profile && !strcmp(profile, "TRUE")) {
        corto_log_profile(true);
    }

    CORTO_APP_STATUS = 0;
}

void platform_deinit(void) {
    corto_tls_free();
}
