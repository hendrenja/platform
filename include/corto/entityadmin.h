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

#ifndef CORTO_ENTITYADMIN_H_
#define CORTO_ENTITYADMIN_H_

typedef
int (*corto_entityWalkAction)(
    void *e,
    void *instance,
    void *userData);

typedef struct corto_entity {
    void *e;
    void *instance;
} corto_entity;

CORTO_SEQUENCE(corto_entitySeq, corto_entity,);

typedef struct corto_entityPerParent {
    corto_entitySeq entities;
    char *parent;
} corto_entityPerParent;

CORTO_SEQUENCE(corto_entityPerParentSeq, corto_entityPerParent,);

/* Threads keep their own copy of active entities so there is no need for
 * a lock when walking the admin */
typedef struct corto_entityAdmin {
    /* Key for accessing local thread copy */
    corto_tls key;

    /* Total number of enitities */
    int count;

    /* Lock for modifying /reading global object */
    corto_rwmutex_s lock;

    /* counter keeps track of when entities are added / removed so that
     * threads know when to sync. */
    uint32_t changed;

    /* Entity administration */
    corto_entityPerParentSeq entities[CORTO_MAX_SCOPE_DEPTH];
} corto_entityAdmin;

int16_t corto_entityAdmin_getDepthFromId(
    const char *id);

corto_entityAdmin* corto_entityAdmin_get(
    corto_entityAdmin *_this);

int16_t corto_entityAdmin_add(
    corto_entityAdmin *_this,
    const char *parent,
    void *e,
    void *instance);

int corto_entityAdmin_remove(
    corto_entityAdmin *_this,
    const char *parent,
    void *e,
    void *instance,
    bool removeAll);

int corto_entityAdmin_walk(
    corto_entityAdmin *_this,
    corto_entityWalkAction action,
    const char *parent,
    bool recursive,
    void *userData);

void corto_entityAdmin_free(
    void *admin);

#endif
