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

#ifndef CORTO_RBTREE_H_
#define CORTO_RBTREE_H_

#ifdef __cplusplus
extern "C" {
#endif

CORTO_EXPORT 
corto_rb corto_rb_new(
    corto_equals_cb compare,
    void *ctx);

CORTO_EXPORT 
void corto_rb_free(
    corto_rb tree);

CORTO_EXPORT 
void* corto_rb_find(
    corto_rb tree, 
    void* key);

CORTO_EXPORT 
void* corto_rb_findPtr(
    corto_rb tree, 
    void* key);

CORTO_EXPORT 
void corto_rb_set(
    corto_rb tree, 
    const void* key, 
    void* value);

CORTO_EXPORT 
void* corto_rb_findOrSet(
    corto_rb tree, 
    const void* key, 
    void* value);

CORTO_EXPORT 
void corto_rb_remove(
    corto_rb tree, 
    void* key);

CORTO_EXPORT 
bool corto_rb_hasKey(
    corto_rb tree, 
    const void* key, 
    void** value);

CORTO_EXPORT 
bool corto_rb_hasKey_w_cmp(
    corto_rb tree, 
    const void* key, 
    void** value, 
    corto_equals_cb cmp);

CORTO_EXPORT 
void* corto_rb_min(
    corto_rb tree, 
    void** key_out);

CORTO_EXPORT 
void* corto_rb_max(
    corto_rb tree, 
    void** key_out);

CORTO_EXPORT 
void* corto_rb_next(
    corto_rb tree, 
    void* key, 
    void** key_out);

CORTO_EXPORT 
void* corto_rb_prev(
    corto_rb tree, 
    void* key, 
    void** key_out);

CORTO_EXPORT 
uint32_t corto_rb_size(
    corto_rb tree);

CORTO_EXPORT 
int corto_rb_walk(
    corto_rb tree, 
    corto_elementWalk_cb callback, 
    void* userData);

CORTO_EXPORT 
int corto_rb_walkPtr(
    corto_rb tree, 
    corto_elementWalk_cb callback, 
    void* userData);

#define corto_rb_iter(tree) _corto_rb_iter(tree, alloca(sizeof(struct jsw_rbtrav)));
CORTO_EXPORT corto_iter _corto_rb_iter(corto_rb tree, void *ctx);
CORTO_EXPORT bool corto_rb_iterChanged(corto_iter *iter);

#ifdef __cplusplus
}
#endif

#endif /* CORTO_RBTREE_H_ */
