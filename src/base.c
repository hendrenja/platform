#include <include/base.h>

int8_t CORTO_APP_STATUS = 3;
int8_t CORTO_BACKTRACE_ENABLED = 0;

/* Lock to protect global administration related to logging framework */
corto_mutex_s corto_log_lock;

extern char *corto_log_appName;

corto_tls CORTO_KEY_THREAD_STRING;

void base_init(char *appName) {
    corto_log_appName = appName;

    if (corto_mutex_new(&corto_log_lock)) {
        corto_critical("failed to create mutex for logging framework");
    }

    void corto_threadStringDealloc(void *data);

    if (corto_tls_new(&CORTO_KEY_THREAD_STRING, corto_threadStringDealloc)) {
        corto_critical("failed to obtain tls key for thread admin");
    }

    CORTO_APP_STATUS = 0;
}
