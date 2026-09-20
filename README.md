# libregexp

**ECMAScript regular expressions for [sysl](https://github.com/sysl-lang/sysl) — QuickJS's engine,
carried here, so there is nothing to install and nothing for a target to be missing.**

```hocon
dependencies {
  libregexp { git = "github.com/sysl-lang/libregexp", version = "0.1.1" }
}
```

```sysl
import sh.sysl.libregexp.{compile, describe}

main() -> Result[unit, string]
    val re = compile("(?<user>\\w+)@(?<host>[\\w.]+)", "g").map_err(describe)?

    for m in re.find_all("ed@example.com and sam@example.org").map_err(describe)?.view()
        print(m.group(1).unwrap(), m.group(2).unwrap())
```

The module is **`sh.sysl.libregexp`**, named for the C library it carries, as every binding in this
organisation is. The prefix is the reverse-DNS of `sysl.sh`, so a package claims a name nobody else
will mint rather than the top-level word.

## Three regexes, and how to choose

| | dialect | where it runs | cost |
|---|---|---|---|
| `sysl.regex` (standard library) | POSIX extended | everywhere | linear time, nothing can explode |
| `sh.sysl.pcre2` | Perl | wherever PCRE2 is **installed** | backtracks; PCRE2's own limits |
| **`sh.sysl.libregexp`** | **ECMAScript** | **everywhere, vendored** | backtracks; a step budget |

`sysl.regex` has no `\d`, no lazy quantifier, no lookaround and no backreference; if you need none of
those, it is the one to use and it cannot be made to run away. `pcre2` is the mature choice for a
hosted program. **This package is the only one of the three that is both a full backtracking dialect
and available on a board or in a `wasm32-wasi` build**, because its implementation is in this
repository rather than on the machine.

And where the program *is* JavaScript-shaped — a language implemented in sysl, a config format that
borrowed JavaScript's regular expressions, a port of something from Node — this is the engine those
patterns were written against, so it is not merely a dialect that is close enough.

## The layout

```
sh/sysl/libregexp/
    libregexp.sysl      the binding: Regex, Match, Subject, flags, the budget, GetSubstitution
    tests.sysl          41 tests, every expected value produced by running node
    c/
        c.sysl          libregexp as C declares it, and the numbers its header holds
        glue.c/.h       the three functions libregexp says the embedder owes, over a caller's budget
        tests.sysl      8 tests: the struct layout against the C, and the raw calls
        libregexp.c     ---+
        libregexp.h        |
        libregexp-opcode.h |  QuickJS's own, byte for byte -- see "Upstream"
        libunicode.c       |
        libunicode.h       |
        libunicode-table.h |
        cutils.c           |
        cutils.h        ---+
LICENSE                 ISC for the binding, MIT for the vendored engine
package.hocon           who this package is, and what it needs of the machine
```

**Everything that is C lives in `c/`**, module `sh.sysl.libregexp.c`. That is the two-layer shape
every binding here uses: the `c` layer has to be *faithful*, because a signature that disagrees with
the C links perfectly and corrupts the call at run time, and the layer above it has to be *pleasant*,
which is a different question that would otherwise be answered in the same breath.

## The surface

```sysl
compile(pattern: string, flags: string) -> Result[&Regex, Error]
compile_with(pattern: string, fl: Flags, budget: Budget) -> Result[&Regex, Error]

Regex.source                                  the pattern as written
Regex.flags() -> Flags                        read back out of the bytecode
Regex.group_count() -> usize
Regex.has_named_groups() -> bool
Regex.named_groups() -> &Buf[NamedGroup]      every (?<name>...) and the number it took
Regex.group_named(name) -> Option[usize]

Regex.is_match(s)  -> Result[bool, Error]
Regex.find(s)      -> Result[Option[Match], Error]
Regex.find_at(s, from_byte)
Regex.exec(subj: &Subject, from_byte)         over a subject converted once
Regex.find_all(s)  -> Result[&Buf[Match], Error]
Regex.split(s)     -> Result[&Buf[string], Error]
Regex.replace(s, with) / replace_all(s, with) -> Result[string, Error]

Match.span(i) -> Option[(usize, usize)]       byte offsets; None when the group took no part
Match.group(i) -> Option[string]
Match.text() / start() / end() / is_empty() / group_count()

subject(s) -> &Subject                        prepare once, match many times
expand(m, re, with) -> string                 ECMAScript's GetSubstitution, on its own
parse_flags(s) -> Result[Flags, Error]
default_budget() / unlimited_budget() -> Budget
describe(e: Error) -> string
```

`Error` is `Pattern(message)`, `GaveUp`, `TooDeep`, `OutOfMemory`, `Corrupt` or `TooLong`, and it
implements `Eq` and `Display`.

## Positions are byte offsets

Every offset here — a match's start and end, a group's span, the `from` an `exec` takes — is a
**byte** offset into the UTF-8 `string` that was searched, and always on a character boundary, so
`s[a..<b]` takes one without a boundary check failing.

**That is not what JavaScript reports.** A JavaScript index counts UTF-16 code units. A consumer that
owes its own users character or code-unit positions converts, and `sysl.text.char_indices` is the
walk that does it.

## What `u` means

Without `u`, ECMAScript matches over UTF-16 code units: an astral character such as `\u{1F600}` is
**two** characters, `.` matches one half of it, and `[\u{1F600}]` is a class of two surrogates. With
`u` it is one character, `.` matches the whole of it, `\u{...}` is a code-point escape, and `\p{...}`
becomes available. This package follows ECMAScript and does **not** quietly turn `u` on, because a
pattern written for one dialect can mean something different in the other.

**What it does do is keep every reported offset on a character boundary in both modes.** A span whose
edge falls between the halves of a surrogate pair — possible only without `u` — is rounded outward to
the whole character, because the middle of a UTF-8 character is not a position a `string` has. So
`.` without `u` over `"😀"` reports one match covering all four bytes where node reports two of one
code unit each. `\p` without `u` is an identity escape in both, so `\p{L}` there is the literal text
`p{L}`; the tests pin that, because it is the silent case.

## Encoding, and what it costs

`lre_exec` does not take UTF-8. It matches over an 8-bit Latin-1 buffer or over UTF-16, so a subject
is prepared first:

- **A pure-ASCII subject is handed over as it stands.** One pass to confirm, no copy, and every
  offset libregexp reports is already a byte offset. This is the common case.
- **Anything else is converted to UTF-16 once**, with a table beside it saying which byte each code
  unit came from. That costs six bytes per code unit — two for the unit, four for the offset — and
  the conversion is kept for as long as the `Subject` lives, which is what makes `find_all` one
  conversion rather than one per match. A program matching several patterns over one text builds the
  `Subject` itself with `subject(s)` and hands it to each `exec`.

**The pattern goes to `lre_compile` as UTF-8**, which is what that function reads — with one
correction. Without `u`, ECMAScript reads the pattern as UTF-16, so an astral literal in it is a
surrogate *pair*. A pattern handed over as plain UTF-8 would be one code point, which no subject in
that mode can produce, and it would silently match nothing. So a non-`u` pattern holding an astral
character is re-encoded as CESU-8 first, exactly as a JavaScript engine does. A pattern with nothing
to convert — nearly all of them — is passed through with no allocation.

## The budget

ECMAScript regular expressions backtrack, so a pattern can be made to run for a very long time on a
short subject. `(a+)+$` against thirty `a`s and a `b` is the standard demonstration, and in a server
it is a denial of service. Every match here runs under a **step budget**, and an overrun is `GaveUp`
rather than a process that stops answering.

```sysl
val re = compile_with("(a+)+$", no_flags(), Budget(200, 1024 * 1024))?
```

`Budget.steps` is in units of ten thousand backtracking steps, which is how often libregexp reaches
its interrupt hook. `Budget.stack_bytes` bounds the *pattern parser*, the only part of libregexp that
recurses on the C stack; exceeding it is `TooDeep` from `compile`. `default_budget()` is generous
enough that no ordinary pattern comes near it, and `unlimited_budget()` is what a JavaScript engine
gives you — the right answer only for a program matching patterns and subjects it wrote itself.

**The budget counts steps rather than milliseconds** because a clock is not available on every target
this package claims, and because a step count is reproducible: the same pattern and subject give up
in the same place every time, so a test can pin it.

## Replacement text

`replace` and `replace_all` read their second argument as ECMAScript's `GetSubstitution`, and
`expand` is the same thing on its own for a program building the answer itself.

| form | what it stands for |
|---|---|
| `$$` | one literal `$` |
| ``$` `` | everything before the match |
| `$&` | the match |
| `$'` | everything after the match |
| `$n`, `$nn` | group `n`, empty when that group took no part |
| `$<name>` | the named group, empty when it took no part |

Anything else is literal, including a `$` at the end and a `$9` in a pattern with fewer than nine
groups. `$nn` prefers the two-digit reading when that group exists. **`$<name>` against a pattern
with no named groups at all is the literal text `$<name>`**, where against a pattern that has some
but not this one it is empty — that difference is the specification's and the tests pin both.

## Where it builds

**Anywhere with a C library.** The vendored sources include `<stdlib.h>`, `<stdio.h>`, `<stdarg.h>`,
`<string.h>`, `<inttypes.h>` and `<assert.h>` and nothing else — no clock, no threads, no POSIX.

- **Cortex-M33 with newlib** — verified: `arm-none-eabi-gcc -mcpu=cortex-m33 -mthumb -O2` compiles
  all four C files clean. That covers the Pico SDK, Zephyr and a FreeRTOS application.
- **A bare freestanding target fails**, and that is clang's headers rather than this code: a
  `--target thumb-freestanding` build has `stdint.h` and `stddef.h` and not `stdlib.h`. miniz,
  qrcodegen and llhttp are all in the same position.
- **`wasm32-wasi` needs `WASI_SDK_PATH` set**, which sysl refuses without: *"building for WASI needs
  wasi-sdk's own clang"*. Nothing in this package is wasm-specific — the same six headers are all
  wasi-libc provides — but it has not been run here, and this line will say so until it has.

There is **no module-level storage** anywhere in the package, in sysl or in C: `glue.c` has no
`static` variable and the budget is a struct the caller places, which is what lets an `@export` reach
this code on a target with no loader to run an initializer.

## What it costs to carry

462 KiB of vendored C, of which 247 KiB is `libunicode-table.h`. A program using the whole surface
links **192 KiB** more than the same program without it (242 KiB against 50 KiB, `-O1`, arm64 macOS).
A program that never mentions `\p{...}` still pays for the tables, since `lre_compile` reaches them.

## Upstream

Vendored from [bellard/quickjs](https://github.com/bellard/quickjs) release **2026-06-04**, from
`https://bellard.org/quickjs/quickjs-2026-06-04.tar.xz`, whose SHA-256 is

```
b376e839b322978313d929fd20663b11ba58b75df5a46c126dd19ea2fa70ad2a
```

The eight files carried are byte-identical to that tarball's, and to the same files at git commit
`04be246001599f5995fa2f2d8c91a0f198d3f34c` — checked with `cmp`, both ways, before they were copied.
**Nothing upstream is edited here**; everything this package adds is in `glue.c`, `glue.h`, `c.sysl`
and the layer above.

### Why bellard/quickjs rather than quickjs-ng

Both forks have everything a binding needs — `lre_check_timeout` and `LRE_RET_TIMEOUT`,
`lre_check_stack_overflow`, `lre_get_groupnames`, lookbehind, and the `g`/`i`/`m`/`s`/`u`/`y`/`d`
flags plus `v` (`LRE_FLAG_UNICODE_SETS`). quickjs-ng is the more actively released of the two
(v0.17.0) and adds `lre_check_bytecode`.

**The decision was the dependency footprint, and it is not close.** `libregexp.c` includes
`cutils.h`, so whatever that header drags in, every consumer of this package drags in:

| | bellard | quickjs-ng |
|---|---|---|
| `cutils.h` | 457 lines | 2021 lines |
| what it includes | `stdlib.h`, `string.h`, `inttypes.h` | those plus `time.h`, `sys/time.h`, `math.h`, `unistd.h`, `pthread.h`, `mach-o/dyld.h`, `malloc/malloc.h` |

quickjs-ng's `cutils.h` carries threads, mutexes and a clock, unconditionally, because quickjs-ng is
built for hosted platforms. A package whose entire reason for vendoring is that it reaches a board
cannot take a `<pthread.h>` with it. bellard's is the reference implementation both forks' libregexp
descends from and is what this package carries.

**Vendoring means upstream fixes do not arrive on their own.** Watch bellard's releases and
re-vendor; the eight files are a straight copy and the tests are what says the copy was right.

## Checks

- **49 tests**, 41 over the binding and 8 over the raw layer. Every expected value in the first set
  was produced by running `node`, not recalled — the package's claim is that it is ECMAScript, so the
  oracle is an ECMAScript engine, and the spans are converted from JavaScript's UTF-16 indices to
  the byte offsets this package reports.
- **AddressSanitizer**, and it reaches the C: `SYSL_EXTRA_CFLAGS="-fsanitize=address -g" sysl test .`
  is green, and a program driven deliberately past the end of a subject reports
  `heap-buffer-overflow libregexp.c:2938 in lre_exec_backtrack` — so the instrumentation is on the
  vendored source and the green run means something. `nm -u` on the built binary lists
  `__asan_memcpy`.
- **A mutation pass of twelve**, each breaking one parameter or one rule on purpose. All twelve go
  red. Two findings came out of it: removing the empty-match advance made the suite *hang* rather
  than fail, which is now impossible — `advance` guarantees the cursor moves; and a branch reporting
  `TooDeep` from a *match* was dead code, since only the pattern parser recurses in C.

## Using it without the package manager

```
sysl build-lib . -o /tmp/libregexp.syslib
sysl run yourprogram.sysl --lib /tmp/libregexp.syslib

sysl run yourprogram.sysl --lib /path/to/this/repo
```
