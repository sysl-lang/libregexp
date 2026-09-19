/*
 * The three functions libregexp declares and does not define, and the budget they read.
 *
 * `libregexp.h` ends with a block marked "must be provided by the user": `lre_realloc`,
 * `lre_check_timeout` and `lre_check_stack_overflow`. QuickJS supplies them from its runtime; a
 * binding has to supply them too, and this file is that. Nothing else here is upstream's.
 *
 * Everything is driven from one caller-owned struct. The sysl side declares a `Budget` with the
 * same fields, asserts its size and every offset against this header, and passes its address as
 * libregexp's `opaque`. So there is no static storage anywhere in this package and no hidden state
 * between calls -- two regexes running on two threads read two budgets.
 */
#ifndef SYSL_LRE_GLUE_H
#define SYSL_LRE_GLUE_H

#include <stddef.h>
#include <stdint.h>

typedef struct SyslLreBudget {
    /* How many more times `lre_check_timeout` may be reached before the match gives up. libregexp
       calls it once per 10000 backtracking steps, so this is a step budget divided by 10000.
       Negative means unlimited. */
    int64_t ticks;

    /* How many bytes of C stack the compiler may descend through, measured from the frame that
       entered this package. Zero means unlimited. Only `lre_compile` recurses; matching keeps its
       backtracking stack on the heap. */
    int64_t stack_bytes;

    /* Set to 1 by whichever check refused, and never cleared here -- the caller clears it when it
       arms the budget. It is what tells "no match" apart from "gave up", the two being the same
       return value from the compiler's point of view. */
    int32_t gave_up;

    /* 1 when the refusal was the stack check rather than the tick budget. */
    int32_t hit_stack;

    /* The frame `sysl_lre_compile` or `sysl_lre_exec` was entered from. Written by them, read by
       the stack check. */
    intptr_t stack_base;
} SyslLreBudget;

/* `lre_compile`, with the budget's stack base recorded first. The arguments after `b` are
   upstream's, unchanged. */
uint8_t *sysl_lre_compile(int *plen, char *error_msg, int error_msg_size,
                          const char *buf, size_t buf_len, int re_flags,
                          SyslLreBudget *b);

/* `lre_exec`, the same way. */
int sysl_lre_exec(uint8_t **capture, const uint8_t *bc_buf,
                  const uint8_t *cbuf, int cindex, int clen, int cbuf_type,
                  SyslLreBudget *b);

/* Hand a bytecode buffer back to the allocator it came from. `lre_realloc` is the whole of the
   answer, but spelling it out here keeps the sysl side from having to know that freeing is a
   zero-sized reallocation. */
void sysl_lre_free_bytecode(uint8_t *bc_buf);

#endif /* SYSL_LRE_GLUE_H */
