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


#include "idmatch.h"

static char* corto_idmatchTokenStr(corto_idmatchToken t) {
    switch(t) {
    case CORTO_MATCHER_TOKEN_NONE: return "none";
    case CORTO_MATCHER_TOKEN_IDENTIFIER: return "identifier";
    case CORTO_MATCHER_TOKEN_FILTER: return "filter";
    case CORTO_MATCHER_TOKEN_SCOPE: return "/";
    case CORTO_MATCHER_TOKEN_TREE: return "//";
    case CORTO_MATCHER_TOKEN_THIS: return ".";
    case CORTO_MATCHER_TOKEN_PARENT: return "..";
    case CORTO_MATCHER_TOKEN_SEPARATOR: return ",";
    case CORTO_MATCHER_TOKEN_AND: return "&";
    case CORTO_MATCHER_TOKEN_OR: return "|";
    case CORTO_MATCHER_TOKEN_NOT: return "^";
    }
    return NULL;
}

static int corto_idmatchValidate(corto_idmatch_program data) {
    int op;

    corto_idmatchToken t = CORTO_MATCHER_TOKEN_NONE, tprev = CORTO_MATCHER_TOKEN_NONE;
    for (op = 0; op < data->size; op++) {
        t = data->ops[op].token;
        switch(t) {
        case CORTO_MATCHER_TOKEN_AND:
            switch(tprev) {
            case CORTO_MATCHER_TOKEN_AND:
            case CORTO_MATCHER_TOKEN_OR:
            case CORTO_MATCHER_TOKEN_NOT:
            case CORTO_MATCHER_TOKEN_SEPARATOR:
            case CORTO_MATCHER_TOKEN_SCOPE:
            case CORTO_MATCHER_TOKEN_PARENT:
                goto error;
            default: break;
            }
            break;
        case CORTO_MATCHER_TOKEN_OR:
            switch(tprev) {
            case CORTO_MATCHER_TOKEN_AND:
            case CORTO_MATCHER_TOKEN_OR:
            case CORTO_MATCHER_TOKEN_NOT:
            case CORTO_MATCHER_TOKEN_SEPARATOR:
            case CORTO_MATCHER_TOKEN_SCOPE:
            case CORTO_MATCHER_TOKEN_PARENT:
                goto error;
            default: break;
            }
            break;
        case CORTO_MATCHER_TOKEN_NOT:
            switch(tprev) {
            case CORTO_MATCHER_TOKEN_AND:
            case CORTO_MATCHER_TOKEN_OR:
            case CORTO_MATCHER_TOKEN_NOT:
            case CORTO_MATCHER_TOKEN_SEPARATOR:
            case CORTO_MATCHER_TOKEN_SCOPE:
            case CORTO_MATCHER_TOKEN_PARENT:
                goto error;
            default: break;
            }
            break;
        case CORTO_MATCHER_TOKEN_SEPARATOR:
            switch(tprev) {
            case CORTO_MATCHER_TOKEN_AND:
            case CORTO_MATCHER_TOKEN_OR:
            case CORTO_MATCHER_TOKEN_NOT:
            case CORTO_MATCHER_TOKEN_SEPARATOR:
                goto error;
            default: break;
            }
            break;
        case CORTO_MATCHER_TOKEN_IDENTIFIER:
            switch(tprev) {
            case CORTO_MATCHER_TOKEN_IDENTIFIER:
            case CORTO_MATCHER_TOKEN_FILTER:
            case CORTO_MATCHER_TOKEN_THIS:
            case CORTO_MATCHER_TOKEN_PARENT:
                goto error;
            default: break;
            }
            break;
        case CORTO_MATCHER_TOKEN_SCOPE:
            switch(tprev) {
            case CORTO_MATCHER_TOKEN_SCOPE:
            case CORTO_MATCHER_TOKEN_TREE:
            case CORTO_MATCHER_TOKEN_AND:
            case CORTO_MATCHER_TOKEN_OR:
                goto error;
            default: break;
            }
            break;
        case CORTO_MATCHER_TOKEN_TREE:
            switch(tprev) {
            case CORTO_MATCHER_TOKEN_SCOPE:
            case CORTO_MATCHER_TOKEN_TREE:
            case CORTO_MATCHER_TOKEN_PARENT:
            case CORTO_MATCHER_TOKEN_AND:
            case CORTO_MATCHER_TOKEN_OR:
                goto error;
            default: break;
            }
            break;
        case CORTO_MATCHER_TOKEN_THIS:
        case CORTO_MATCHER_TOKEN_PARENT:
            switch(tprev) {
            case CORTO_MATCHER_TOKEN_THIS:
            case CORTO_MATCHER_TOKEN_PARENT:
            case CORTO_MATCHER_TOKEN_NOT:
                goto error;
            default: break;
            }
            break;
        case CORTO_MATCHER_TOKEN_FILTER:
            switch(tprev) {
            case CORTO_MATCHER_TOKEN_IDENTIFIER:
            case CORTO_MATCHER_TOKEN_THIS:
            case CORTO_MATCHER_TOKEN_PARENT:
            case CORTO_MATCHER_TOKEN_FILTER:
                goto error;
            default: break;
            }
            break;
        default:
            break;
        }
        tprev = t;
    }

    return 0;
error:
    corto_throw("unexpected '%s' after '%s'",
        corto_idmatchTokenStr(t),
        corto_idmatchTokenStr(tprev));
    return -1;
}

bool corto_idmatch_isOperator(
    char ch)
{
    switch (ch) {
    case '*':
    case '?':
    case '^':
    case '&':
    case '|':
    case ',':
        return true;
    default:
        return false;
    }
}

bool corto_idmatch_hasOperators(
    const char *expr)
{
    const char *ptr;
    char ch;
    for (ptr = expr; (ch = *ptr); ptr++) {
        if (ch == '/' && ptr[1] == '/') {
            return true;
        } else if (corto_idmatch_isOperator(ch)) {
            return true;
        }
    }
    return false;
}

int16_t corto_idmatchParseIntern(
    corto_idmatch_program data,
    const char *expr,
    bool allowScopes,
    bool allowSeparators)
{
    char *ptr, *start, ch;
    int op = 0;

    data->size = 0;
    data->kind = 0;
    data->tokens = corto_strdup(expr);
    strlower(data->tokens);

    ptr = data->tokens;
    for (; (ch = *ptr); data->ops[op].start = ptr, ptr++) {
        data->ops[op].containsWildcard = FALSE;
        data->ops[op].start = NULL;
        start = ptr;
        switch(ch) {
        case '/':
            if (!allowScopes) {
                corto_throw("scope operators not allowed");
                goto error;
            }
            if (ptr[1] == '/') {
                data->ops[op].token = CORTO_MATCHER_TOKEN_TREE;
                *ptr = '\0';
                ptr++;
            } else {
                *ptr = '\0';
                data->ops[op].token = CORTO_MATCHER_TOKEN_SCOPE;
            }
            break;
        case ':':
            if (!allowScopes) {
                corto_throw("scope operators not allowed");
                goto error;
            }
            if (ptr[1] == ':') {
                data->ops[op].token = CORTO_MATCHER_TOKEN_SCOPE;
                *ptr = '\0';
                ptr++;
            } else {
                corto_throw("invalid usage of ':'");
                goto error;
            }
            break;
        case '|':
            data->ops[op].token = CORTO_MATCHER_TOKEN_OR;
            *ptr = '\0';
            break;
        case '&':
            data->ops[op].token = CORTO_MATCHER_TOKEN_AND;
            *ptr = '\0';
            break;
        case '^':
            data->ops[op].token = CORTO_MATCHER_TOKEN_NOT;
            *ptr = '\0';
            break;
        case ',':
            if (!allowSeparators) {
                corto_throw("scope operators not allowed");
                goto error;
            }
            data->ops[op].token = CORTO_MATCHER_TOKEN_SEPARATOR;
            *ptr = '\0';
            break;
        case '.':
            if (!allowScopes) {
                corto_throw("scope operators not allowed");
                goto error;
            }
            if (ptr[1] == '.') {
                if ((op < 4) ||
                    ((data->ops[op - 2].token == CORTO_MATCHER_TOKEN_PARENT) &&
                     (data->ops[op - 1].token == CORTO_MATCHER_TOKEN_SCOPE)))
                {
                    data->ops[op].token = CORTO_MATCHER_TOKEN_PARENT;
                } else {
                    op -= 4;
                }
                *ptr = '\0';
                ptr++;
            } else {
                if ((op < 2) || (data->ops[op - 1].token != CORTO_MATCHER_TOKEN_SCOPE)) {
                    data->ops[op].token = CORTO_MATCHER_TOKEN_THIS;
                } else {
                    op -= 2;
                }
                *ptr = '\0';
            }
            break;
        default:
            data->ops[op].token = CORTO_MATCHER_TOKEN_IDENTIFIER;
            while((ch = *ptr++) &&
                  (isalnum(ch) || (ch == '_') || (ch == '*') || (ch == '?') ||
                    (ch == '(') || (ch == ')') || (ch == '{') || (ch == '}') ||
                    (ch == ' ') || (ch == '$') || (ch == '.')))
            {
                if ((ch == '*') || (ch == '?')) {
                    data->ops[op].token = CORTO_MATCHER_TOKEN_FILTER;
                }
            }

            ptr--; /* Go back one character to adjust for lookahead of one */
            if (!(ptr - start)) {
                corto_throw("invalid character '%c' (expr = '%s')", ch, expr);
                goto error;
            }
            ptr--;
            break;
        }

        if (!data->ops[op].start) {
            data->ops[op].start = start;
        }
        if (++op == (CORTO_MATCHER_MAX_OP - 2)) {
            corto_throw("expression contains too many tokens");
            goto error;
        }
    }

    if (op) {
        /* If expression ends with scope or tree, append '*' */
        if ((data->ops[op - 1].token == CORTO_MATCHER_TOKEN_SCOPE) ||
            (data->ops[op - 1].token == CORTO_MATCHER_TOKEN_TREE))
        {
            data->ops[op].token = CORTO_MATCHER_TOKEN_FILTER;
            data->ops[op].start = "*";
            op ++;
        }
        data->size = op;
        data->ops[op].token = CORTO_MATCHER_TOKEN_NONE; /* Close with NONE */
        if (corto_idmatchValidate(data)) {
            data->size = 0;
            goto error;
        }
    }

    return 0;
error:
    return -1;
}

corto_idmatch_program corto_idmatch_compile(
    const char *expr,
    bool allowScopes,
    bool allowSeparators)
{
    corto_idmatch_program result = corto_alloc(sizeof(struct corto_idmatch_program_s));
    result->kind = 0;
    result->tokens = NULL;

    corto_debug("match: compile expression '%s'", expr);
    if (corto_idmatchParseIntern(result, expr, allowScopes, allowSeparators) || !result->size) {
        if (!result->size) {
            corto_throw("expression '%s' resulted in empty program", expr);
        }
        corto_dealloc(result->tokens);
        corto_dealloc(result);
        result = NULL;
        goto error;
    }

    /* Optimize for common cases (*, simple identifier) */
    if (result->size == 1) {
        if (result->ops[0].token == CORTO_MATCHER_TOKEN_IDENTIFIER) {
            result->kind = 1;
        } else if (result->ops[0].token == CORTO_MATCHER_TOKEN_THIS) {
            result->kind = 2;
        } else if (result->ops[0].token == CORTO_MATCHER_TOKEN_FILTER) {
            if (!strcmp(result->ops[0].start, "*")) {
                result->kind = 3;
            }
        }
    } else if (result->size == 2) {
        if (result->ops[0].token == CORTO_MATCHER_TOKEN_SCOPE) {
            if (result->ops[1].token == CORTO_MATCHER_TOKEN_FILTER) {
                if (!strcmp(result->ops[1].start, "*")) {
                    result->kind = 3;
                }
            } else if (result->ops[1].token == CORTO_MATCHER_TOKEN_IDENTIFIER) {
                result->kind = 1;
            } else if (result->ops[1].token == CORTO_MATCHER_TOKEN_THIS) {
                result->kind = 2;
            }
        } else if (result->ops[0].token == CORTO_MATCHER_TOKEN_TREE) {
            if (result->ops[1].token == CORTO_MATCHER_TOKEN_FILTER) {
                if (!strcmp(result->ops[1].start, "*")) {
                    result->kind = 4;
                }
            }
        }
    }

    return result;
error:
    return NULL;
}

int corto_idmatch_scope(
    corto_idmatch_program program)
{
    int result = 0;

    if (program->kind == 1) {
        result = 0;
    } else if (program->kind == 2) {
        result = 0;
    } else if (program->kind == 3) {
        result = 1;
    } else if (program->kind == 4) {
        result = 2;
    } else {
        int i;
        for (i = 0; i < program->size; i++) {
            switch(program->ops[i].token) {
            case CORTO_MATCHER_TOKEN_SCOPE:
                if (result < 1) result = 1;
                break;
            case CORTO_MATCHER_TOKEN_TREE:
                result = 2;
                break;
            default:
                break;
            }
        }
    }

    return result;
}

bool corto_idmatch_runExpr(
    corto_idmatchOp **op,
    const char **elements[],
    corto_idmatchToken precedence)
{
    bool done = FALSE;
    bool result = TRUE;
    bool right = FALSE;
    bool identifierMatched = FALSE;
    corto_idmatchOp *cur;
    const char **start = *elements; // Pointer to array of strings

    do {
        /*printf("eval %s [%s => %s] (prec=%s)\n",
          corto_idmatchTokenStr((*op)->token),
          (*elements)[0],
          (*op)->start,
          corto_idmatchTokenStr(precedence));*/

        cur = *op; (*op) ++;

        switch(cur->token) {
        case CORTO_MATCHER_TOKEN_THIS:
            result = !strcmp(".", (*elements)[0]);
            identifierMatched = TRUE;
            break;
        case CORTO_MATCHER_TOKEN_IDENTIFIER:
        case CORTO_MATCHER_TOKEN_FILTER: {
            const char *elem = (*elements)[0];
            if (elem && (elem[0] != '.')) {
                result = !fnmatch(cur->start, (*elements)[0], 0);
            } else {
                result = FALSE;
                done = TRUE;
            }
            identifierMatched = TRUE;
            break;
        }
        case CORTO_MATCHER_TOKEN_AND:
            right = corto_idmatch_runExpr(op, elements, CORTO_MATCHER_TOKEN_IDENTIFIER);
            if (result) result = right;
            break;
        case CORTO_MATCHER_TOKEN_OR:
            right = corto_idmatch_runExpr(op, elements, CORTO_MATCHER_TOKEN_AND);
            if (!result) result = right;
            break;
        case CORTO_MATCHER_TOKEN_NOT:
            right = corto_idmatch_runExpr(op, elements, CORTO_MATCHER_TOKEN_OR);
            if (result) result = !right;
            break;
        case CORTO_MATCHER_TOKEN_SCOPE:
            (*elements)++;
            if (!(*elements)[0]) {
                result = FALSE;
                done = TRUE;
                (*op)++; /* progress op for skipped value */
            } else {
                right = corto_idmatch_runExpr(op, elements, CORTO_MATCHER_TOKEN_NOT);
                if (result) result = right;
            }
            break;
        case CORTO_MATCHER_TOKEN_TREE: {
            corto_idmatchOp *opPtr = *op;
            if (identifierMatched) {
                if (!result) {
                    done = TRUE;
                    break;
                }
                (*elements)++;

            }

            const char **elementPtr = *elements, **elementFound = NULL;
            right = FALSE;
            if (!elementPtr[0]) {
                result = TRUE;
                done = TRUE;
                (*op)++; /* progress op for skipped value */
            } else {
                do {
                    *elements = elementPtr;
                    *op = opPtr;
                    right = corto_idmatch_runExpr(op, elements, CORTO_MATCHER_TOKEN_SCOPE);
                    if (right) {
                        elementFound = *elements;
                    }
                } while ((elementFound ? right : !right) && (++elementPtr)[0]);
                if ((result = (elementFound != NULL))) {
                    *elements = elementFound;
                }
            }
            break;
        }
        case CORTO_MATCHER_TOKEN_SEPARATOR:
            *(elements) = start;
            right = corto_idmatch_runExpr(op, elements, CORTO_MATCHER_TOKEN_TREE);
            if (!result) {
                result = right;
            }
            break;
        default:
            result = FALSE;
            done = TRUE;
            break;
        }
    } while(!done &&
            ((*op)->token != CORTO_MATCHER_TOKEN_NONE) &&
            ((*op)->token <= precedence));

    return result;
}

bool corto_idmatch_run(
    corto_idmatch_program program,
    const char *str)
{
    bool result = FALSE;

    if (!program->size) {
        return FALSE;
    }

    if (program->kind == 0) {
        const char *elements[CORTO_MAX_SCOPE_DEPTH + 1];
        corto_idmatchOp *op = program->ops;
        const char **elem = elements;
        corto_id id;
        strcpy(id, str);
        strlower(id);

        int8_t elementCount = corto_pathToArray(id, elements, "/");
        if (elementCount == -1) {
            goto error;
        }
        elements[elementCount] = NULL;

        /* Ignore leading scope tokens ('/') in expression and string */
        if (op->token == CORTO_MATCHER_TOKEN_SCOPE) {
            op ++;
        }
        if (!elements[0][0]) elem++;

        result = corto_idmatch_runExpr(&op, &elem, CORTO_MATCHER_TOKEN_SEPARATOR);
        if (result) {
            if (elem != &elements[elementCount - 1]) {
                /* Not all elements have been matched */
                result = FALSE;
            }
        }
    } else if (program->kind == 1) {
        /* Match identifier */
        result = !stricmp(program->ops[0].start, str);
    } else if (program->kind == 2) {
        result = !strcmp(".", str);
    } else if (program->kind == 3) {
        /* Match any identifier in scope */
        const char *ptr = str;
        if (ptr[0] == '/') ptr ++;
        if (ptr[0] == '.' && !ptr) {
            result = FALSE;
        } else if (!strchr(ptr, '/')) {
            result = TRUE;
        } else {
            result = FALSE;
        }
    } else if (program->kind == 4) {
        /* Match any identifier in tree */
        if (strcmp(str, ".")) {
            result = TRUE;
        }
    }

    return result;
error:
    return FALSE;
}

const char* corto_matchParent(
    const char *parent,
    const char *expr)
{
    const char *parentPtr = parent, *exprPtr = expr;
    char parentCh, exprCh;

    if (!parent) {
        return expr;
    }

    if (*parentPtr == '/') parentPtr++;
    if (*exprPtr == '/') exprPtr++;

    if (!*parentPtr) {
        return exprPtr;
    }

    while ((parentCh = *parentPtr) &&
           (exprCh = *exprPtr))
    {
        if (parentCh < 97) parentCh = tolower(parentCh);
        if (exprCh < 97) exprCh = tolower(exprCh);

        if (parentCh != exprCh) {
            break;
        }

        parentPtr++;
        exprPtr++;
    }

    if (*parentPtr == '\0') {
        if (*exprPtr == '/') {
            exprPtr ++;
        } else if (*exprPtr == '\0') {
            exprPtr = ".";
        } else {
            exprPtr = NULL;
        }
        return exprPtr;
    } else {
        return NULL;
    }
}

void corto_idmatch_free(corto_idmatch_program matcher) {
    if (matcher) {
        if (matcher->tokens) {
            corto_dealloc(matcher->tokens);
        }
        corto_dealloc(matcher);
    }
}

bool corto_idmatch(
    const char *expr,
    const char *str)
{
    struct corto_idmatch_program_s matcher;
    if (corto_idmatchParseIntern(&matcher, expr, TRUE, TRUE)) {
        goto error;
    }
    bool result = corto_idmatch_run(&matcher, str);
    corto_dealloc(matcher.tokens);
    return result;
error:
    return FALSE;
}

int corto_idmatch_get_scope(
    corto_idmatch_program program)
{
    int result = 1;
    int32_t op;
    bool quit = false;

    for (op = 0; (op < program->size) && !quit; op ++) {
        switch (program->ops[op].token) {
        case CORTO_MATCHER_TOKEN_IDENTIFIER:
        case CORTO_MATCHER_TOKEN_THIS:
        case CORTO_MATCHER_TOKEN_PARENT:
            result = 0;
            break;
        case CORTO_MATCHER_TOKEN_SCOPE:
            result = 1;
            break;
        case CORTO_MATCHER_TOKEN_TREE:
            result = 2;
        default:
            quit = true;
            break;
        }
    }

    return result;
}
