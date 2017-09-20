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

#include <include/base.h>

char* corto_hostname(void) {
    corto_id buff;
    gethostname(buff, sizeof(buff));
    return corto_setThreadString(buff);
}

bool corto_os_match(
    char *os)
{
    if (!stricmp(os, CORTO_OS_STRING) ||

#ifdef __i386__
        !stricmp(os, CORTO_OS_STRING "-x86") ||
        !stricmp(os, CORTO_OS_STRING "-i386") ||
        !stricmp(os, CORTO_OS_STRING "-i686") ||
        !stricmp(os, "x86-" CORTO_OS_STRING) ||
        !stricmp(os, "i386-" CORTO_OS_STRING) ||
        !stricmp(os, "i686-" CORTO_OS_STRING))

#elif __x86_64__
        !stricmp(os, CORTO_OS_STRING "-amd64") ||
        !stricmp(os, CORTO_OS_STRING "-x64") ||
        !stricmp(os, CORTO_OS_STRING "-x86_64") ||
        !stricmp(os, CORTO_OS_STRING "-x86-64") ||        
        !stricmp(os, "amd64-" CORTO_OS_STRING) ||
        !stricmp(os, "x64-" CORTO_OS_STRING) ||
        !stricmp(os, "x86-64-" CORTO_OS_STRING) ||
        !stricmp(os, "x86_64-" CORTO_OS_STRING))

#elif defined(__arm__) && defined(CORTO_CPU_32BIT)
        !stricmp(os, CORTO_OS_STRING "-arm") ||
        !stricmp(os, CORTO_OS_STRING "-arm7l") ||
        !stricmp(os, "arm-" CORTO_OS_STRING) ||
        !stricmp(os, "arm7l-" CORTO_OS_STRING))

#elif defined(__arm__) && defined(CORTO_CPU_64BIT)
        !stricmp(os, CORTO_OS_STRING "-arm8") ||
        !stricmp(os, CORTO_OS_STRING "-arm64") ||
        !stricmp(os, "arm64-" CORTO_OS_STRING) ||
        !stricmp(os, "arm8-" CORTO_OS_STRING))
#endif
    {
        return true;
    } else {
        return false;
    }
}
