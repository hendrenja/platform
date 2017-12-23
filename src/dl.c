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

typedef void*(*dlproc)(void);

/* Link dynamic library */
corto_dl corto_dl_open(const char* file) {
    corto_dl dl;

    dl = (corto_dl)dlopen(file, RTLD_NOW | RTLD_LOCAL);

    return dl;
}

/* Close dynamic library */
void corto_dl_close(corto_dl dl) {
    dlclose(dl);
}

/* Lookup symbol in dynamic library */
void* corto_dl_sym(corto_dl dl, const char* sym) {
    return dlsym(dl, sym);
}

/* Lookup procedure in dynamic library */
void*(*corto_dl_proc(corto_dl dl, const char* proc))(void) {
    return (dlproc)(intptr_t)dlsym(dl, proc);
}

const char* corto_dl_error(void) {
    return dlerror();
}
