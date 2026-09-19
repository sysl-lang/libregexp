/*
 * What libregexp asks of whoever embeds it, answered against a caller-owned budget.
 *
 * There is deliberately no `static` anything in this file. A `static` counter would be module
 * storage, which shuts a package out of a freestanding target and makes two concurrent matches
 * share a budget; every byte of state lives in the `SyslLreBudget` the caller placed.
 */

#include <stdlib.h>

#include "libregexp.h"
#include "glue.h"

/* The allocator libregexp routes every allocation through: its bytecode buffer while compiling,
 * its character-range sets, and the backtracking stack once a match outgrows the 32 entries it
 * keeps in its own frame.
 *
 * A zero size is a free. ISO C leaves `realloc(p, 0)` implementation-defined, so it is spelled out
 * rather than left to the platform.
 */
void *lre_realloc(void *opaque, void *ptr, size_t size)
{
    (void)opaque;

    if (size == 0) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, size);
}

/* Reached once per 10000 backtracking steps. Returning non-zero stops the match with
 * `LRE_RET_TIMEOUT`, which is how a pattern like `(a+)+$` is made to answer rather than to run.
 *
 * A budget is a step count rather than a clock because a clock is not available on every target
 * this package claims -- a freestanding build has no `time()` -- and because a step count is
 * reproducible, so a test can assert it.
 */
int lre_check_timeout(void *opaque)
{
    SyslLreBudget *b = (SyslLreBudget *)opaque;

    if (b == NULL || b->ticks < 0)
        return 0;

    if (b->ticks == 0) {
        b->gave_up = 1;
        return 1;
    }

    b->ticks--;
    return 0;
}

/* Reached at the head of the two recursive functions in the pattern parser. Returning non-zero
 * makes `lre_compile` fail with "stack overflow" in its error message.
 *
 * The depth is measured as the distance between this frame and the one that entered the package,
 * which is what a stack limit means on every target where the stack is contiguous. Direction is not
 * assumed: the distance is taken as an absolute value, so a machine whose stack grows upwards is
 * measured the same way.
 */
int lre_check_stack_overflow(void *opaque, size_t alloca_size)
{
    SyslLreBudget *b = (SyslLreBudget *)opaque;
    char here;
    intptr_t used;

    if (b == NULL || b->stack_bytes == 0)
        return 0;

    used = (intptr_t)&here - b->stack_base;
    if (used < 0)
        used = -used;

    if ((int64_t)used + (int64_t)alloca_size > b->stack_bytes) {
        b->gave_up = 1;
        b->hit_stack = 1;
        return 1;
    }
    return 0;
}

uint8_t *sysl_lre_compile(int *plen, char *error_msg, int error_msg_size,
                          const char *buf, size_t buf_len, int re_flags,
                          SyslLreBudget *b)
{
    char here;

    if (b != NULL)
        b->stack_base = (intptr_t)&here;

    return lre_compile(plen, error_msg, error_msg_size, buf, buf_len, re_flags, b);
}

int sysl_lre_exec(uint8_t **capture, const uint8_t *bc_buf,
                  const uint8_t *cbuf, int cindex, int clen, int cbuf_type,
                  SyslLreBudget *b)
{
    char here;

    if (b != NULL)
        b->stack_base = (intptr_t)&here;

    return lre_exec(capture, bc_buf, cbuf, cindex, clen, cbuf_type, b);
}

void sysl_lre_free_bytecode(uint8_t *bc_buf)
{
    lre_realloc(NULL, bc_buf, 0);
}
