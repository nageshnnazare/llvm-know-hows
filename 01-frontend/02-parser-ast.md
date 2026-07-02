# 01.02 · The Parser & AST — Tokens to a Tree of Meaning

> The parser consumes the token stream and builds an **Abstract Syntax Tree** that encodes
> structure and operator precedence. We use **recursive descent** for statements and **Pratt
> / operator-precedence** parsing for expressions — the combination used by clang, rustc, and
> most hand-written production parsers.

---

## 1. The AST: nodes that mirror the language

Each construct in Toy gets a class. They form a tree; codegen (next chapter) walks it.

```
   Expr (abstract base)
     ├── NumberExpr        3.14
     ├── VariableExpr      x
     ├── BinaryExpr        lhs OP rhs        (+ - * <)
     ├── CallExpr          callee(args...)
     └── IfExpr            if C then T else E

   Prototype               name + param names   (a signature)
   Function                Prototype + body Expr (a definition)
```

```cpp
// AST.h
#include <memory>
#include <string>
#include <vector>

// Base class for all expression nodes.
class ExprAST {
public:
  virtual ~ExprAST() = default;
  virtual llvm::Value *codegen() = 0;   // we'll fill this in next chapter
};

// Numeric literal like "1.0".
class NumberExprAST : public ExprAST {
  double Val;
public:
  NumberExprAST(double Val) : Val(Val) {}
  llvm::Value *codegen() override;
};

// A variable reference like "x".
class VariableExprAST : public ExprAST {
  std::string Name;
public:
  VariableExprAST(std::string Name) : Name(std::move(Name)) {}
  llvm::Value *codegen() override;
};

// A binary operator: LHS Op RHS.
class BinaryExprAST : public ExprAST {
  char Op;                                 // '+', '-', '*', '<'
  std::unique_ptr<ExprAST> LHS, RHS;
public:
  BinaryExprAST(char Op, std::unique_ptr<ExprAST> L, std::unique_ptr<ExprAST> R)
      : Op(Op), LHS(std::move(L)), RHS(std::move(R)) {}
  llvm::Value *codegen() override;
};

// A function call: Callee(Args...).
class CallExprAST : public ExprAST {
  std::string Callee;
  std::vector<std::unique_ptr<ExprAST>> Args;
public:
  CallExprAST(std::string Callee, std::vector<std::unique_ptr<ExprAST>> Args)
      : Callee(std::move(Callee)), Args(std::move(Args)) {}
  llvm::Value *codegen() override;
};

// if Cond then Then else Else  — an EXPRESSION (yields a value).
class IfExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Cond, Then, Else;
public:
  IfExprAST(std::unique_ptr<ExprAST> C, std::unique_ptr<ExprAST> T,
            std::unique_ptr<ExprAST> E)
      : Cond(std::move(C)), Then(std::move(T)), Else(std::move(E)) {}
  llvm::Value *codegen() override;
};

// A function signature: name and argument names (all doubles in Toy).
class PrototypeAST {
  std::string Name;
  std::vector<std::string> Args;
public:
  PrototypeAST(std::string Name, std::vector<std::string> Args)
      : Name(std::move(Name)), Args(std::move(Args)) {}
  const std::string &getName() const { return Name; }
  llvm::Function *codegen();
};

// A function definition: a prototype plus a body expression.
class FunctionAST {
  std::unique_ptr<PrototypeAST> Proto;
  std::unique_ptr<ExprAST> Body;
public:
  FunctionAST(std::unique_ptr<PrototypeAST> P, std::unique_ptr<ExprAST> B)
      : Proto(std::move(P)), Body(std::move(B)) {}
  llvm::Function *codegen();
};
```

> Note `codegen()` is a virtual method on each node — the **visitor-by-virtual-dispatch**
> style. Real compilers often use a separate visitor class; for a small language, methods on
> nodes are clean. Either way, codegen is "walk the tree, emit IR per node."

---

## 2. The grammar (informal EBNF)

```
   toplevel   ::= definition | external | expression | ';'
   definition ::= 'def' prototype expression
   external   ::= 'extern' prototype
   prototype  ::= identifier '(' identifier* ')'

   expression ::= primary (binop primary)*          ← precedence handled specially
   primary    ::= numberexpr | parenexpr | identifierexpr | ifexpr
   numberexpr ::= number
   parenexpr  ::= '(' expression ')'
   identifierexpr ::= identifier
                    | identifier '(' (expression (',' expression)*)? ')'   ← a call
   ifexpr     ::= 'if' expression 'then' expression 'else' expression
```

Recursive descent means: **one function per grammar rule**, and the call graph of those
functions mirrors the grammar. `ParseExpression` calls `ParsePrimary`, which may call
`ParseExpression` again (for parens) — recursion mirrors nesting.

---

## 3. Parsing primary expressions (recursive descent)

Helpers and error reporting first:

```cpp
// Parser.cpp
static std::unique_ptr<ExprAST> LogError(const char *Str) {
  fprintf(stderr, "Error: %s\n", Str);
  return nullptr;
}
static std::unique_ptr<PrototypeAST> LogErrorP(const char *Str) {
  LogError(Str); return nullptr;
}

static std::unique_ptr<ExprAST> ParseExpression();   // forward decl
```

`numberexpr ::= number`:

```cpp
static std::unique_ptr<ExprAST> ParseNumberExpr() {
  auto Result = std::make_unique<NumberExprAST>(NumVal);  // NumVal set by lexer
  getNextToken();                                          // consume the number
  return std::move(Result);
}
```

`parenexpr ::= '(' expression ')'`:

```cpp
static std::unique_ptr<ExprAST> ParseParenExpr() {
  getNextToken();                       // eat '('
  auto V = ParseExpression();           // recurse!
  if (!V) return nullptr;
  if (CurTok != ')') return LogError("expected ')'");
  getNextToken();                       // eat ')'
  return V;
}
```

`identifierexpr ::= identifier | identifier '(' args ')'` — the lookahead at `(` decides
between a variable and a call:

```cpp
static std::unique_ptr<ExprAST> ParseIdentifierExpr() {
  std::string IdName = IdentifierStr;
  getNextToken();                       // eat identifier

  if (CurTok != '(')                    // simple variable reference
    return std::make_unique<VariableExprAST>(IdName);

  // It's a call: identifier '(' ... ')'
  getNextToken();                       // eat '('
  std::vector<std::unique_ptr<ExprAST>> Args;
  if (CurTok != ')') {
    while (true) {
      if (auto Arg = ParseExpression()) Args.push_back(std::move(Arg));
      else return nullptr;
      if (CurTok == ')') break;
      if (CurTok != ',') return LogError("expected ')' or ',' in argument list");
      getNextToken();                   // eat ','
    }
  }
  getNextToken();                       // eat ')'
  return std::make_unique<CallExprAST>(IdName, std::move(Args));
}
```

`ifexpr`:

```cpp
static std::unique_ptr<ExprAST> ParseIfExpr() {
  getNextToken();                       // eat 'if'
  auto Cond = ParseExpression();
  if (!Cond) return nullptr;
  if (CurTok != tok_then) return LogError("expected 'then'");
  getNextToken();                       // eat 'then'
  auto Then = ParseExpression();
  if (!Then) return nullptr;
  if (CurTok != tok_else) return LogError("expected 'else'");
  getNextToken();                       // eat 'else'
  auto Else = ParseExpression();
  if (!Else) return nullptr;
  return std::make_unique<IfExprAST>(std::move(Cond), std::move(Then), std::move(Else));
}
```

The dispatcher for `primary`:

```cpp
static std::unique_ptr<ExprAST> ParsePrimary() {
  switch (CurTok) {
  case tok_identifier: return ParseIdentifierExpr();
  case tok_number:     return ParseNumberExpr();
  case '(':            return ParseParenExpr();
  case tok_if:         return ParseIfExpr();
  default:             return LogError("unknown token when expecting an expression");
  }
}
```

---

## 4. Operator precedence — the Pratt / precedence-climbing core

The naive grammar `expression ::= primary (binop primary)*` is ambiguous about precedence:
should `a + b * c` parse as `(a+b)*c` or `a+(b*c)`? We need `*` to bind tighter than `+`.
The elegant solution is **operator-precedence parsing**.

Assign each binary operator a precedence number:

```cpp
#include <map>
static std::map<char, int> BinopPrecedence = {
    {'<', 10},
    {'+', 20}, {'-', 20},
    {'*', 40},                 // higher = binds tighter
};

static int GetTokPrecedence() {
  if (!isascii(CurTok)) return -1;
  auto it = BinopPrecedence.find(CurTok);
  if (it == BinopPrecedence.end()) return -1;   // not a known binop
  return it->second;
}
```

The algorithm, visualized for `a + b * c`:

```
  parse 'a' as LHS.
  see '+' (prec 20). Loop: parse RHS starting at 'b'.
    parse 'b' as RHS-so-far.
    peek next op '*' (prec 40). Is 40 > 20? YES → '*' binds tighter,
      so 'b' belongs to the '*' on its right, not the '+' on its left.
      RECURSE: parse the right-hand side at higher precedence → builds (b * c).
    RHS = (b * c).
  build (a + (b * c)).  ✔ correct precedence
```

```
        a + b * c

   step:  LHS=a
          op=+ (20)
              RHS=b, but next op * (40) > 20  ⇒ pull b into a tighter subtree
              RHS := (b * c)
          ⇒ (a + (b * c))

                +
               ╱ ╲
              a   *
                 ╱ ╲
                b   c
```

The code (`ParseBinOpRHS` is the famous heart of precedence-climbing):

```cpp
static std::unique_ptr<ExprAST>
ParseBinOpRHS(int ExprPrec, std::unique_ptr<ExprAST> LHS) {
  while (true) {
    int TokPrec = GetTokPrecedence();

    // If this op binds less tightly than the minimum we're allowed to take,
    // we're done — return LHS to the caller (who holds the looser operator).
    if (TokPrec < ExprPrec) return LHS;

    int BinOp = CurTok;
    getNextToken();                          // eat the operator

    auto RHS = ParsePrimary();               // parse the primary after the op
    if (!RHS) return nullptr;

    // If the NEXT operator binds tighter than this one, let it take RHS first.
    int NextPrec = GetTokPrecedence();
    if (TokPrec < NextPrec) {
      RHS = ParseBinOpRHS(TokPrec + 1, std::move(RHS));   // recurse with higher floor
      if (!RHS) return nullptr;
    }

    // Merge LHS and RHS, then loop to handle further operators at this level.
    LHS = std::make_unique<BinaryExprAST>(BinOp, std::move(LHS), std::move(RHS));
  }
}

static std::unique_ptr<ExprAST> ParseExpression() {
  auto LHS = ParsePrimary();
  if (!LHS) return nullptr;
  return ParseBinOpRHS(0, std::move(LHS));   // 0 = accept any operator
}
```

> This ~20-line function correctly handles *any* set of precedences and associativities.
> It's worth tracing by hand on `a < b + c * d` until it's obvious. The recursion's
> "precedence floor" (`ExprPrec`) is the whole trick: it says "only fold operators at least
> this tight into me; looser ones belong to my caller."

---

## 5. Parsing definitions, externs, and the top level

```cpp
// prototype ::= id '(' id* ')'
static std::unique_ptr<PrototypeAST> ParsePrototype() {
  if (CurTok != tok_identifier) return LogErrorP("expected function name in prototype");
  std::string FnName = IdentifierStr;
  getNextToken();
  if (CurTok != '(') return LogErrorP("expected '(' in prototype");

  std::vector<std::string> ArgNames;
  while (getNextToken() == tok_identifier)
    ArgNames.push_back(IdentifierStr);
  if (CurTok != ')') return LogErrorP("expected ')' in prototype");
  getNextToken();                       // eat ')'
  return std::make_unique<PrototypeAST>(FnName, std::move(ArgNames));
}

// definition ::= 'def' prototype expression
static std::unique_ptr<FunctionAST> ParseDefinition() {
  getNextToken();                       // eat 'def'
  auto Proto = ParsePrototype();
  if (!Proto) return nullptr;
  if (auto E = ParseExpression())
    return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
  return nullptr;
}

// external ::= 'extern' prototype
static std::unique_ptr<PrototypeAST> ParseExtern() {
  getNextToken();                       // eat 'extern'
  return ParsePrototype();
}

// Top-level expressions become anonymous zero-arg functions, so we can codegen/run them.
static std::unique_ptr<FunctionAST> ParseTopLevelExpr() {
  if (auto E = ParseExpression()) {
    auto Proto = std::make_unique<PrototypeAST>("__anon_expr", std::vector<std::string>());
    return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
  }
  return nullptr;
}
```

Wrapping a bare top-level expression in an anonymous function (`__anon_expr`) is a neat trick:
it gives us something callable, which is exactly what the JIT needs to *evaluate* a REPL
line (section 04).

---

## 6. The driver loop

```cpp
static void MainLoop() {
  while (true) {
    switch (CurTok) {
    case tok_eof:    return;
    case ';':        getNextToken(); break;          // ignore stray semicolons
    case tok_def:    HandleDefinition();   break;
    case tok_extern: HandleExtern();       break;
    default:         HandleTopLevelExpr(); break;
    }
  }
}
// (each Handle* calls the matching Parse*, then codegen() — wired up next chapter)
```

```
        getNextToken (prime CurTok)
                 │
                 ▼
        ┌─────── MainLoop ────────┐
        │  look at CurTok:        │
        │   'def'    → definition │──▶ ParseDefinition  ──▶ FunctionAST
        │   'extern' → external   │──▶ ParseExtern      ──▶ PrototypeAST
        │   ';'      → skip       │
        │   eof      → done       │
        │   else     → top expr   │──▶ ParseTopLevelExpr──▶ FunctionAST(__anon_expr)
        └─────────────────────────┘
```

---

## 7. The output: an AST ready for codegen

For `def add(a b) a + b;` the parser yields:

```
   FunctionAST
     ├── Prototype  name="add"  args=[a, b]
     └── Body: BinaryExprAST('+')
                 ├── LHS: VariableExprAST("a")
                 └── RHS: VariableExprAST("b")
```

For `if n < 2 then n else fib(n-1)+fib(n-2)`:

```
   IfExprAST
     ├── Cond: BinaryExprAST('<', Var(n), Number(2))
     ├── Then: VariableExprAST(n)
     └── Else: BinaryExprAST('+',
                  CallExprAST(fib, [BinaryExprAST('-', Var(n), Number(1))]),
                  CallExprAST(fib, [BinaryExprAST('-', Var(n), Number(2))]))
```

This tree is the input to codegen. In the next chapter every node's `codegen()` turns into a
sequence of `IRBuilder` calls.

---

## Mental model checkpoint

1. What does "one function per grammar rule" mean in recursive descent?
2. How does `ParseIdentifierExpr` decide between a variable and a call?
3. In precedence climbing, what does the `ExprPrec` "floor" parameter accomplish?
4. Trace `a + b * c` and show why you get `a + (b*c)`.
5. Why wrap a top-level expression in an anonymous `__anon_expr` function?
6. Draw the AST for `if x then a else b*c`.

Next → [03-ast-to-ir.md](03-ast-to-ir.md)
