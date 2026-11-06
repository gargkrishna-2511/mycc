# mycc – a tiny C compiler

A single-file compiler (~450 lines) for a small subset of C. It compiles to x86-64 assembly (Intel syntax, System V ABI) and uses `gcc` only to assemble and link.

## Build and use

```sh
gcc -o mycc mycc.c
./mycc test.c > out.s
gcc out.s -o out
./out        # sum=24 fib(10)=55
```

## Supported

- `int` functions (up to 6 params), `int` locals, recursion, calls to libc (e.g. `printf`)
- Operators: `+ - * / %`, `== != < <= > >=`, `&& ||`, `!`, unary `-`, `=`
- `if`/`else`, `while`, `for` (with `int` declaration in init), `{ }` blocks, `return`
- String literals (with `\n`, `\t`, `\\`, `\"`), `//` and `/* */` comments

## How it works

1. **Tokenizer** – turns source text into tokens
2. **Parser** – recursive descent, builds an AST
3. **Code generator** – walks the AST, emits stack-machine style assembly

## Ideas for next steps

- Pointers and arrays (`int *p`, `p[i]`, `&x`, `*p`)
- `char`, `long`, structs
- Global variables, `break`/`continue`, `do`/`while`, `switch`
- A preprocessor (`#include`, `#define`)
- Better error messages (line/column)

Good projects to study: [chibicc](https://github.com/rui314/chibicc), [8cc](https://github.com/rui314/8cc), [tcc](https://github.com/TinyCC/tinycc), and Nora Sandler's *Writing a C Compiler*.

## Put it on GitHub

```sh
git init
git add mycc.c test.c README.md
git commit -m "Tiny C compiler"
git branch -M main
git remote add origin https://github.com/<your-username>/mycc.git
git push -u origin main
```
