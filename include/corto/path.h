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
 * @section path Path utility functions
 * @brief API that makes it easier to work with paths.
 */


#ifndef CORTO_PATH_H_
#define CORTO_PATH_H_

#define CORTO_MAX_PATH_LENGTH (512)

#ifdef __cplusplus
extern "C" {
#endif

/** Create a canonical version of a path.
 * This function reduces a path to its canonical form by resolving . and .. operators.
 * @param str An id buffer in which to store the id. If NULL, a corto-managed
 * string is returned which may change with subsequent calls to corto_fullpath and
 * other functions that use the corto stringcache.
 *
 * @param path The input path. Can be the same as buffer.
 * @return The path. If buffer is NULL, the returned string is not owned by the application. Otherwise, buffer is returned.
 * @see corto_idof corto_fullname corto_path corto_pathname
 */
CORTO_EXPORT
char* corto_path_clean(
    char *buf,
    char *path);

/** Get directory name from path.
 *
 * @param path The input path.
 * @return The directory name from the specified path. Needs to be deallocated.
 */
CORTO_EXPORT
char* corto_path_dirname(
    const char *path);

/** Obtain an array with the individual elements of a path.
 * This function splits up a path using the specified separator. An element with
 * at least *CORTO_MAX_SCOPE_DEPTH* elements must be provided.
 *
 * ```warning
 * This function modifies the input path.
 * ```
 *
 * @param path The path to be split up.
 * @param elements The result array with the path elements.
 * @param sep The separator used to split up the path.
 * @return The number of elements in the array.
 * @see corto_idof corto_fullname corto_path corto_pathname
 */
CORTO_EXPORT
int32_t corto_pathToArray(
    char *path,
    const char *elements[],
    char *sep);

#ifdef __cplusplus
}
#endif

#endif
