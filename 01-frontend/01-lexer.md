# 01.01 · The Lexer — Characters to Tokens

> The frontend is the part of the compiler *you* write. It turns source text into LLVM IR.
> Stage one is the **lexer**: collapse a stream of characters into a stream of **tokens**.
> We define a small language, **Toy**, that we will carry through the entire guide — frontend,
> AOT, JIT, and MLIR.

---

## 1. The language we'll compile: "Toy"

A tiny but non-trivial functional/imperative language (a cousin of LLVM's Kaleidoscope).
Enough to exercise functions, arithmetic, control flow, and calls — the things that make
codegen interesting.

```
  # comments start with '#'
  def add(a b) a + b;            # function definition; body is an expression
  def fib(n)
      if n < 2 then n
      else fib(n-1) + fib(n-2);

  extern sin(x);                 # declare an external function (from libm)

  fib(10);                       # a top-level expression — gets evaluated
```

Design choices (kept deliberately small so the *compiler* concepts stay front and center):

```
  • One numeric type: f64 (double). Keeps the type system trivial.
  • Functions take a fixed list of params, return one value.
  • Expressions only: if/then/else is an expression, not a statement.
  • 'extern' declares functions implemented elsewhere (libc/libm).
  • Operators: + - * < (we add more later; precedence matters — see parser).
```

We don't pick this because it's powerful; we pick it because **every concept in real LLVM
frontends appears here in miniature**, and you can hold the whole thing in your head.

---

## 2. What a token is

A **token** is a lexical atom: a keyword, identifier, number, operator, or punctuation. The
lexer's job:

```
   raw text                        token stream
  ───────────                     ──────────────────────────────────────
  "def fib(n)"      ───────────▶   TOK_DEF
                                   TOK_IDENT("fib")
                                   '('
                                   TOK_IDENT("n")
                                   ')'
```

Crucially the lexer **discards whitespace and comments** and **classifies** each chunk. It
does *not* understand structure — that's the parser's job. The lexer is a flat,
left-to-right scan.

```
        ┌────────────────────────────────────────────────────────┐
        │  LEXER = a loop that, each call, skips whitespace,     │
        │  reads the next maximal lexeme, and returns one token. │
        └────────────────────────────────────────────────────────┘
              getNextToken() ──▶ getNextToken() ──▶ getNextToken() ──▶ ...
                  TOK_DEF           IDENT("fib")          '('
```

---

## 3. Token representation

We use an enum for fixed tokens and side-channel variables for the *payload* (the spelling
of an identifier, the value of a number). This is the classic Kaleidoscope style — minimal
and clear.

```cpp
// Token.h
enum Token {
  tok_eof        = -1,   // end of file

  // keywords
  tok_def        = -2,
  tok_extern     = -3,
  tok_if         = -4,
  tok_then       = -5,
  tok_else       = -6,

  // primary
  tok_identifier = -7,   // payload in IdentifierStr
  tok_number     = -8,   // payload in NumVal
};

// Side-channel payloads, filled in by the lexer when it returns the above:
inline std::string IdentifierStr;  // valid when last token was tok_identifier
inline double      NumVal;         // valid when last token was tok_number
```

> Why negative numbers? So that any *single character* token (like `+`, `(`, `;`) can be
> returned as its own positive ASCII value, never colliding with these enums. `getNextToken`
> returns an `int`: negative = a known token kind, positive = "this literal character."

```
   return value of getNextToken():
   ────────────────────────────────
    -8 .. -2   a keyword / ident / number / eof   (look up payload as needed)
    '+' (43)   a literal '+' character
    '(' (40)   a literal '(' character
    ';' (59)   a literal ';' character
```

---

## 4. The lexer, fully

A hand-written, character-at-a-time scanner. Read the comments — each block handles one
*lexeme class*.

```cpp
// Lexer.cpp
#include <cctype>
#include <cstdio>
#include <string>

static int LastChar = ' ';   // one-character lookahead, primed with a space

// gettok - return the next token from standard input.
int gettok() {
  // 1. Skip any whitespace.
  while (isspace(LastChar))
    LastChar = getchar();

  // 2. Identifiers and keywords: [a-zA-Z][a-zA-Z0-9]*
  if (isalpha(LastChar)) {
    IdentifierStr = LastChar;
    while (isalnum((LastChar = getchar())))
      IdentifierStr += LastChar;

    if (IdentifierStr == "def")    return tok_def;
    if (IdentifierStr == "extern") return tok_extern;
    if (IdentifierStr == "if")     return tok_if;
    if (IdentifierStr == "then")   return tok_then;
    if (IdentifierStr == "else")   return tok_else;
    return tok_identifier;          // anything else is a name
  }

  // 3. Numbers: [0-9.]+   (no error checking for brevity)
  if (isdigit(LastChar) || LastChar == '.') {
    std::string NumStr;
    do {
      NumStr += LastChar;
      LastChar = getchar();
    } while (isdigit(LastChar) || LastChar == '.');
    NumVal = strtod(NumStr.c_str(), nullptr);
    return tok_number;
  }

  // 4. Comments: '#' to end of line, then recurse.
  if (LastChar == '#') {
    do
      LastChar = getchar();
    while (LastChar != EOF && LastChar != '\n' && LastChar != '\r');
    if (LastChar != EOF)
      return gettok();              // skip the comment, get the real next token
  }

  // 5. End of file.
  if (LastChar == EOF)
    return tok_eof;

  // 6. Otherwise return the character itself as its ASCII value (e.g. '+', '(').
  int ThisChar = LastChar;
  LastChar = getchar();
  return ThisChar;
}
```

### Trace it

Input `def add(a b) a + b;`:

```
  state of LastChar / output token
  ──────────────────────────────────────────────────────────────────
  read 'd','e','f' → IdentifierStr="def" → keyword       ▶ tok_def
  skip ' '
  read 'a','d','d' → IdentifierStr="add"                 ▶ tok_identifier
  '(' is not alnum/digit/#/eof → returned literally       ▶ '('
  read 'a'          → IdentifierStr="a"                   ▶ tok_identifier
  skip ' '
  read 'b'          → IdentifierStr="b"                   ▶ tok_identifier
  ')'                                                     ▶ ')'
  skip ' '
  'a'                                                     ▶ tok_identifier
  '+'                                                     ▶ '+'
  'b'                                                     ▶ tok_identifier
  ';'                                                     ▶ ';'
  EOF                                                     ▶ tok_eof
```

The "maximal munch" principle is visible: `def` is read as one keyword, not `d`,`e`,`f`. The
`while (isalnum(...))` loop greedily consumes the whole identifier.

---

## 5. The one-token lookahead pattern (CurTok)

Parsers need to "peek" at the current token and "advance." The universal idiom:

```cpp
// Parser uses these instead of calling gettok() directly.
static int CurTok;
static int getNextToken() { return CurTok = gettok(); }
```

```
   ┌─────────────────────────────────────────────────────────────┐
   │ CurTok always holds "the token the parser is looking at."   │
   │ getNextToken() consumes it and loads the next one.          │
   └─────────────────────────────────────────────────────────────┘

      CurTok = tok_def
         │  getNextToken()
         ▼
      CurTok = tok_identifier("add")
         │  getNextToken()
         ▼
      CurTok = '('   ...
```

This single-token lookahead (called **LL(1)**) is enough for our grammar and most
hand-written parsers. The parser in the next chapter is built entirely on `CurTok` +
`getNextToken()`.

---

## 6. Lexer design notes that scale to real languages

What we skipped, and what real lexers add — so you know the shape of the full problem:

```
  ┌──────────────────────────────────────────────────────────────────────────┐
  │ • Source locations: track (line, col) per token for error messages &     │
  │   debug info. Real frontends thread a SourceLocation through everything. │
  │ • String/char literals with escape sequences (\n, \t, \xNN, unicode).    │
  │ • Number lexing: hex/oct/bin, floats with exponents, suffixes (u/l/f).   │
  │ • Multi-char operators: ==, !=, <=, >>, ->, ::  (need >1 char lookahead).│
  │ • Keywords table (hash map) instead of if-chains once you have dozens.   │
  │ • Error recovery: emit a diagnostic and keep going, don't just abort.    │
  │ • Generators: Flex/RE2C build a DFA from regexes; fine, but hand-written │
  │   lexers are common in production (clang, rustc) for speed & control.    │
  └──────────────────────────────────────────────────────────────────────────┘
```

Multi-character operators are the most common "gotcha." Pattern for `<=`:

```cpp
  if (LastChar == '<') {
    LastChar = getchar();
    if (LastChar == '=') { LastChar = getchar(); return tok_le; /* <= */ }
    return '<';                  // just '<'
  }
```

The principle: **lex the longest valid token** (maximal munch), peeking as far as needed.

---

## Mental model checkpoint

1. What does the lexer produce, and what two things does it throw away?
2. Why are the token enums negative while single characters return their ASCII value?
3. Where does the *payload* of an identifier or number token live?
4. What is "maximal munch" and where does the code implement it?
5. Explain the `CurTok` / `getNextToken()` lookahead pattern and the term LL(1).
6. Name three features a production lexer adds that ours omits.

Next → [02-parser-ast.md](02-parser-ast.md)
