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

corto_proc corto_proc_run(const char* exec, char *argv[]) {
    pid_t pid = fork();

    if (pid == 0) {

        /* Child process */
        if (execvp(exec, argv)) {
            corto_error("failed to start process '%s'\n  cwd='%s'\n  err='%s'",
              exec,
              corto_cwd(),
              strerror(errno));
            abort();
        }
    } else if (pid > 0) {
        /* Parent process */
    } else {
        /* Failure */
    }

    return pid;
}

corto_proc corto_proc_runRedirect(
    const char* exec, char *argv[], FILE *in, FILE *out, FILE *err)
{
    pid_t pid = fork();

    if (pid == 0) {
        FILE *devnull = NULL;
        if (!out || !err) {
            devnull = fopen("/dev/null", "w");
            if (!devnull) {
                corto_error("failed to open /dev/null");
                abort();
            }
        }

        if (dup2(fileno(out ? out : devnull), STDOUT_FILENO) < 0) {
            corto_error("failed to redirect stdout for '%s': %s", exec, strerror(errno));
            abort();
        }
        if (out && (err != stdout)) fclose(out);

        if (dup2(fileno(err ? err : devnull), STDERR_FILENO) < 0) {
            corto_error("failed to redirect stderr for '%s': %s", exec, strerror(errno));
            abort();
        }

        if (err && (err != stderr)) fclose(err);
        if (devnull) fclose(devnull);

        if (dup2(fileno(in), STDIN_FILENO) < 0) {
            corto_error("failed to redirect stdin for '%s': %s", exec, strerror(errno));
            abort();
        }
        if (in && (in != stdin)) fclose(in);

        /* Child process */
        if (execvp(exec, argv)) {
            corto_error("failed to start process '%s'", exec);
            abort();
        }
    } else if (pid > 0) {
        /* Parent process */
    } else {
        /* Failure */
    }

    return pid;
}

int corto_proc_kill(corto_proc pid, corto_procsignal sig) {
    return kill(pid, sig);
}

int corto_proc_wait(corto_proc pid, int8_t *rc) {
    int status = 0;
    int result = 0;

    if (waitpid(pid, &status, 0) != pid) {
        corto_seterr("wait for %d failed: %s", pid, strerror(errno));
        return -1;
    }

    if (WIFSIGNALED(status)) {
        result = WTERMSIG(status);
    } else {
        if (rc) {
            *rc = WEXITSTATUS(status);
        }
    }

    return result;
}

#define BUFFER_SIZE (256)

/* Simple blocking function to create and wait for a process */
int corto_proc_cmd(char* cmd, int8_t *rc) {
    int pid;
    char *args[CORTO_MAX_CMD_ARGS];
    char stack_buffer[BUFFER_SIZE];
    char *buffer = stack_buffer;

    corto_debug("cmd: %s", cmd);

    int len = strlen(cmd);
    if (len >= BUFFER_SIZE) {
        buffer = malloc(len + 1);
    }

    strcpy(buffer, cmd);    

    /* Split up commands */
    char ch, *ptr;
    uint8_t argCount = 0;
    bool newArg = FALSE;
    args[argCount] = buffer;
    for (ptr = buffer; (ch = *ptr); ptr++) {
        if (isspace(ch)) {
            *ptr = '\0';
            newArg = TRUE;
        } else if (newArg) {
            args[++argCount] = ptr;
            newArg = FALSE;
        }
    }
    args[argCount + 1] = NULL;

    if (!(pid = corto_proc_run(args[0], args))) {
        goto error;
    }

    if (buffer != stack_buffer) free(buffer);
    return corto_proc_wait(pid, rc);
error:
    if (buffer != stack_buffer) free(buffer);
    return -1;
}

int corto_proc_check(corto_proc pid, int8_t *rc) {
    int status = 0;
    int result = 0;

    result = waitpid(pid, &status, WNOHANG);
    if (!result) {
        /* Process did not change state, still running */
    } else if (WIFSIGNALED(status)) {
        /* Process exited with a signal */
        result = WTERMSIG(status);
    } else {
        /* Process exited normally */
        if (rc) {
            *rc = WEXITSTATUS(status);
        }
        result = -1;
    }

    return result;
}

/* Check whether process is being debugged */
#ifdef __MACH__
#ifndef NDEBUG
typedef unsigned long u_long;
typedef unsigned int u_int;
typedef unsigned short u_short;
typedef unsigned char u_char;
#include <sys/sysctl.h>
#endif
#endif

int corto_beingTraced(void) {
/* Some of these APIs might be unstable. Only use in debug builds. */
#ifndef NDEBUG
#ifdef __MACH__
    int                 junk;
    int                 mib[4];
    struct kinfo_proc   info;
    size_t              size;

    // Initialize the flags so that, if sysctl fails for some bizarre
    // reason, we get a predictable result.
    info.kp_proc.p_flag = 0;

    // Initialize mib, which tells sysctl the info we want, in this case
    // we're looking for information about a specific process ID.
    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_PID;
    mib[3] = getpid();

    // Call sysctl.
    size = sizeof(info);
    junk = sysctl(mib, sizeof(mib) / sizeof(*mib), &info, &size, NULL, 0);
    assert(junk == 0);

    // We're being debugged if the P_TRACED flag is set.
    return ( (info.kp_proc.p_flag & P_TRACED) != 0 );
#else
    return ptrace(PTRACE_TRACEME, 0, NULL, 0) == -1;
#endif
#else
    return 0;
#endif
}

corto_proc _corto_proc(void) {
    return getpid();
}
