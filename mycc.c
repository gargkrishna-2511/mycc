// mycc - a tiny C compiler (subset of C) targeting x86-64 Linux (System V ABI).
//
// Supported: int functions with up to 6 int params, int locals, function calls,
//   + - * / %  == != < <= > >=  && || !  unary -, assignment,
//   if/else, while, for, blocks, return.
//
// Usage:  ./mycc input.c > out.s && gcc out.s -o out
//
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------- Utilities ----------
static char *src;

static void error(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "error: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  exit(1);
}

// ---------- Tokenizer ----------
typedef enum { T_NUM, T_IDENT, T_KW, T_PUNCT, T_STR, T_EOF } TokKind;

typedef struct Token Token;
struct Token {
  TokKind kind;
  Token *next;
  int val;      // for T_NUM
  char *str;    // for others (NUL-terminated copy)
};

static Token *tok;

static Token *new_token(TokKind kind, Token *cur, char *s, int len) {
  Token *t = calloc(1, sizeof(Token));
  t->kind = kind;
  t->str = strndup(s, len);
  cur->next = t;
  return t;
}

static int is_kw(const char *s) {
  static const char *kws[] = {"return", "if", "else", "while", "for", "int"};
  for (size_t i = 0; i < sizeof(kws) / sizeof(*kws); i++)
    if (!strcmp(s, kws[i])) return 1;
  return 0;
}

static void tokenize(char *p) {
  Token head = {0};
  Token *cur = &head;
  while (*p) {
    if (isspace((unsigned char)*p)) { p++; continue; }
    if (!strncmp(p, "//", 2)) { while (*p && *p != '\n') p++; continue; }
    if (!strncmp(p, "/*", 2)) {
      char *q = strstr(p + 2, "*/");
      if (!q) error("unclosed comment");
      p = q + 2;
      continue;
    }
    if (*p == '"') {
      p++;
      char *buf = calloc(1, strlen(p) + 1);
      int len = 0;
      while (*p != '"') {
        if (!*p) error("unclosed string literal");
        if (*p == '\\') {
          p++;
          switch (*p) {
          case 'n': buf[len++] = '\n'; break;
          case 't': buf[len++] = '\t'; break;
          case '0': buf[len++] = '\0'; break;
          default:  buf[len++] = *p; break;
          }
          p++;
        } else {
          buf[len++] = *p++;
        }
      }
      p++;
      cur = new_token(T_STR, cur, buf, len);
      cur->val = len;   // string length (without terminator)
      continue;
    }
    if (isdigit((unsigned char)*p)) {
      char *q = p;
      long v = strtol(p, &p, 10);
      cur = new_token(T_NUM, cur, q, (int)(p - q));
      cur->val = (int)v;
      continue;
    }
    if (isalpha((unsigned char)*p) || *p == '_') {
      char *q = p;
      while (isalnum((unsigned char)*p) || *p == '_') p++;
      cur = new_token(T_IDENT, cur, q, (int)(p - q));
      if (is_kw(cur->str)) cur->kind = T_KW;
      continue;
    }
    static const char *two[] = {"==", "!=", "<=", ">=", "&&", "||"};
    int matched = 0;
    for (size_t i = 0; i < sizeof(two) / sizeof(*two); i++)
      if (!strncmp(p, two[i], 2)) {
        cur = new_token(T_PUNCT, cur, p, 2);
        p += 2;
        matched = 1;
        break;
      }
    if (matched) continue;
    if (strchr("+-*/%<>=!;(){},", *p)) {
      cur = new_token(T_PUNCT, cur, p, 1);
      p++;
      continue;
    }
    error("invalid character '%c'", *p);
  }
  cur = new_token(T_EOF, cur, p, 0);
  tok = head.next;
}

static int at(const char *s) {
  return (tok->kind == T_PUNCT || tok->kind == T_KW) && !strcmp(tok->str, s);
}
static int consume(const char *s) {
  if (!at(s)) return 0;
  tok = tok->next;
  return 1;
}
static void expect(const char *s) {
  if (!consume(s)) error("expected '%s' but got '%s'", s, tok->str);
}
static char *expect_ident(void) {
  if (tok->kind != T_IDENT) error("expected identifier, got '%s'", tok->str);
  char *s = tok->str;
  tok = tok->next;
  return s;
}

// ---------- AST ----------
typedef enum {
  N_NUM, N_STR, N_VAR, N_ASSIGN, N_CALL,
  N_ADD, N_SUB, N_MUL, N_DIV, N_MOD,
  N_EQ, N_NE, N_LT, N_LE,
  N_AND, N_OR, N_NOT, N_NEG,
  N_RETURN, N_IF, N_WHILE, N_FOR, N_BLOCK, N_EXPR
} NodeKind;

typedef struct Var Var;
struct Var { Var *next; char *name; int offset; };

typedef struct Node Node;
struct Node {
  NodeKind kind;
  Node *next;               // next statement / next argument
  Node *lhs, *rhs;
  Node *cond, *then, *els;  // if / while / for
  Node *init, *inc;         // for
  Node *body;               // block: first stmt
  Node *args;               // call: first arg
  int nargs;
  int val;                  // N_NUM
  Var *var;                 // N_VAR
  char *name;               // N_CALL
};

typedef struct Func Func;
struct Func {
  Func *next;
  char *name;
  Var *locals;
  int nparams;
  int stack_size;
  Node *body;
};

static Var *locals;
static int nlocals;

static Var *find_var(const char *name) {
  for (Var *v = locals; v; v = v->next)
    if (!strcmp(v->name, name)) return v;
  return NULL;
}
static Var *new_var(char *name) {
  if (find_var(name)) error("redefinition of '%s'", name);
  Var *v = calloc(1, sizeof(Var));
  v->name = name;
  v->next = locals;
  v->offset = ++nlocals * 8;  // address = rbp - offset
  locals = v;
  return v;
}

typedef struct Str Str;
struct Str { Str *next; int id; char *data; int len; };
static Str *strings;
static int nstrings;

static Node *new_node(NodeKind k) {
  Node *n = calloc(1, sizeof(Node));
  n->kind = k;
  return n;
}
static Node *new_bin(NodeKind k, Node *l, Node *r) {
  Node *n = new_node(k);
  n->lhs = l;
  n->rhs = r;
  return n;
}

// ---------- Parser (recursive descent) ----------
static Node *expr(void);
static Node *stmt(void);

static Node *primary(void) {
  if (consume("(")) {
    Node *n = expr();
    expect(")");
    return n;
  }
  if (tok->kind == T_NUM) {
    Node *n = new_node(N_NUM);
    n->val = tok->val;
    tok = tok->next;
    return n;
  }
  if (tok->kind == T_STR) {
    Str *s = calloc(1, sizeof(Str));
    s->id = nstrings++;
    s->data = tok->str;
    s->len = tok->val;
    s->next = strings;
    strings = s;
    Node *n = new_node(N_STR);
    n->val = s->id;
    tok = tok->next;
    return n;
  }
  if (tok->kind == T_IDENT) {
    char *name = expect_ident();
    if (consume("(")) {
      Node *n = new_node(N_CALL);
      n->name = name;
      Node head = {0}, *cur = &head;
      if (!consume(")")) {
        do {
          cur = cur->next = expr();
          n->nargs++;
        } while (consume(","));
        expect(")");
      }
      if (n->nargs > 6) error("too many arguments (max 6)");
      n->args = head.next;
      return n;
    }
    Var *v = find_var(name);
    if (!v) error("undefined variable '%s'", name);
    Node *n = new_node(N_VAR);
    n->var = v;
    return n;
  }
  error("unexpected token '%s'", tok->str);
  return NULL;
}

static Node *unary(void) {
  if (consume("+")) return unary();
  if (consume("-")) { Node *n = new_node(N_NEG); n->lhs = unary(); return n; }
  if (consume("!")) { Node *n = new_node(N_NOT); n->lhs = unary(); return n; }
  return primary();
}

static Node *mul(void) {
  Node *n = unary();
  for (;;) {
    if (consume("*")) n = new_bin(N_MUL, n, unary());
    else if (consume("/")) n = new_bin(N_DIV, n, unary());
    else if (consume("%")) n = new_bin(N_MOD, n, unary());
    else return n;
  }
}

static Node *add(void) {
  Node *n = mul();
  for (;;) {
    if (consume("+")) n = new_bin(N_ADD, n, mul());
    else if (consume("-")) n = new_bin(N_SUB, n, mul());
    else return n;
  }
}

static Node *relational(void) {
  Node *n = add();
  for (;;) {
    if (consume("<")) n = new_bin(N_LT, n, add());
    else if (consume("<=")) n = new_bin(N_LE, n, add());
    else if (consume(">")) n = new_bin(N_LT, add(), n);   // swap operands
    else if (consume(">=")) n = new_bin(N_LE, add(), n);
    else return n;
  }
}

static Node *equality(void) {
  Node *n = relational();
  for (;;) {
    if (consume("==")) n = new_bin(N_EQ, n, relational());
    else if (consume("!=")) n = new_bin(N_NE, n, relational());
    else return n;
  }
}

static Node *logand(void) {
  Node *n = equality();
  while (consume("&&")) n = new_bin(N_AND, n, equality());
  return n;
}

static Node *logor(void) {
  Node *n = logand();
  while (consume("||")) n = new_bin(N_OR, n, logand());
  return n;
}

static Node *assign(void) {
  Node *n = logor();
  if (consume("=")) {
    if (n->kind != N_VAR) error("left side of '=' is not assignable");
    n = new_bin(N_ASSIGN, n, assign());
  }
  return n;
}

static Node *expr(void) { return assign(); }

static Node *expr_stmt(void) {
  Node *n = new_node(N_EXPR);
  n->lhs = expr();
  return n;
}

static Node *stmt(void) {
  if (consume("return")) {
    Node *n = new_node(N_RETURN);
    n->lhs = expr();
    expect(";");
    return n;
  }
  if (consume("if")) {
    Node *n = new_node(N_IF);
    expect("(");
    n->cond = expr();
    expect(")");
    n->then = stmt();
    if (consume("else")) n->els = stmt();
    return n;
  }
  if (consume("while")) {
    Node *n = new_node(N_WHILE);
    expect("(");
    n->cond = expr();
    expect(")");
    n->then = stmt();
    return n;
  }
  if (consume("for")) {
    Node *n = new_node(N_FOR);
    expect("(");
    if (at("int")) n->init = stmt();              // declaration (consumes ';')
    else if (!consume(";")) { n->init = expr_stmt(); expect(";"); }
    if (!consume(";")) { n->cond = expr(); expect(";"); }
    if (!consume(")")) { n->inc = expr_stmt(); expect(")"); }
    n->then = stmt();
    return n;
  }
  if (consume("{")) {
    Node *n = new_node(N_BLOCK);
    Node head = {0}, *cur = &head;
    while (!consume("}")) {
      if (tok->kind == T_EOF) error("unclosed '{'");
      cur = cur->next = stmt();
    }
    n->body = head.next;
    return n;
  }
  if (consume("int")) {              // declaration: int x; or int x = expr;
    char *name = expect_ident();
    Var *v = new_var(name);
    Node *n;
    if (consume("=")) {
      Node *var = new_node(N_VAR);
      var->var = v;
      n = new_node(N_EXPR);
      n->lhs = new_bin(N_ASSIGN, var, expr());
    } else {
      n = new_node(N_BLOCK);         // empty statement
    }
    expect(";");
    return n;
  }
  Node *n = expr_stmt();
  expect(";");
  return n;
}

static Func *function(void) {
  expect("int");
  Func *f = calloc(1, sizeof(Func));
  f->name = expect_ident();
  locals = NULL;
  nlocals = 0;
  expect("(");
  if (!consume(")")) {
    do {
      expect("int");
      new_var(expect_ident());
      f->nparams++;
    } while (consume(","));
    expect(")");
  }
  if (f->nparams > 6) error("too many parameters (max 6)");
  expect("{");
  Node head = {0}, *cur = &head;
  while (!consume("}")) {
    if (tok->kind == T_EOF) error("unclosed '{' in function '%s'", f->name);
    cur = cur->next = stmt();
  }
  f->body = head.next;
  f->locals = locals;
  f->stack_size = (nlocals * 8 + 15) / 16 * 16;
  return f;
}

// ---------- Code generator ----------
static int label_count;
static int depth;            // current number of 8-byte pushes (for alignment)
static const char *cur_fn;
static const char *argreg[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};

static void push(void) { printf("  push rax\n"); depth++; }
static void pop(const char *r) { printf("  pop %s\n", r); depth--; }

static void gen_expr(Node *n);

static void gen_stmt(Node *n) {
  switch (n->kind) {
  case N_RETURN:
    gen_expr(n->lhs);
    printf("  jmp .L.return.%s\n", cur_fn);
    return;
  case N_EXPR:
    gen_expr(n->lhs);
    return;
  case N_BLOCK:
    for (Node *s = n->body; s; s = s->next) gen_stmt(s);
    return;
  case N_IF: {
    int c = label_count++;
    gen_expr(n->cond);
    printf("  cmp rax, 0\n  je .L.else.%d\n", c);
    gen_stmt(n->then);
    printf("  jmp .L.end.%d\n.L.else.%d:\n", c, c);
    if (n->els) gen_stmt(n->els);
    printf(".L.end.%d:\n", c);
    return;
  }
  case N_WHILE: {
    int c = label_count++;
    printf(".L.begin.%d:\n", c);
    gen_expr(n->cond);
    printf("  cmp rax, 0\n  je .L.end.%d\n", c);
    gen_stmt(n->then);
    printf("  jmp .L.begin.%d\n.L.end.%d:\n", c, c);
    return;
  }
  case N_FOR: {
    int c = label_count++;
    if (n->init) gen_stmt(n->init);
    printf(".L.begin.%d:\n", c);
    if (n->cond) {
      gen_expr(n->cond);
      printf("  cmp rax, 0\n  je .L.end.%d\n", c);
    }
    gen_stmt(n->then);
    if (n->inc) gen_stmt(n->inc);
    printf("  jmp .L.begin.%d\n.L.end.%d:\n", c, c);
    return;
  }
  default:
    error("internal: bad statement node");
  }
}

static void gen_expr(Node *n) {
  switch (n->kind) {
  case N_NUM:
    printf("  mov rax, %d\n", n->val);
    return;
  case N_STR:
    printf("  lea rax, [rip + .L.str.%d]\n", n->val);
    return;
  case N_VAR:
    printf("  mov rax, [rbp-%d]\n", n->var->offset);
    return;
  case N_ASSIGN:
    gen_expr(n->rhs);
    printf("  mov [rbp-%d], rax\n", n->lhs->var->offset);
    return;
  case N_NEG:
    gen_expr(n->lhs);
    printf("  neg rax\n");
    return;
  case N_NOT:
    gen_expr(n->lhs);
    printf("  cmp rax, 0\n  sete al\n  movzx rax, al\n");
    return;
  case N_AND: {
    int c = label_count++;
    gen_expr(n->lhs);
    printf("  cmp rax, 0\n  je .L.false.%d\n", c);
    gen_expr(n->rhs);
    printf("  cmp rax, 0\n  je .L.false.%d\n", c);
    printf("  mov rax, 1\n  jmp .L.end.%d\n.L.false.%d:\n  mov rax, 0\n.L.end.%d:\n", c, c, c);
    return;
  }
  case N_OR: {
    int c = label_count++;
    gen_expr(n->lhs);
    printf("  cmp rax, 0\n  jne .L.true.%d\n", c);
    gen_expr(n->rhs);
    printf("  cmp rax, 0\n  jne .L.true.%d\n", c);
    printf("  mov rax, 0\n  jmp .L.end.%d\n.L.true.%d:\n  mov rax, 1\n.L.end.%d:\n", c, c, c);
    return;
  }
  case N_CALL: {
    for (Node *a = n->args; a; a = a->next) { gen_expr(a); push(); }
    for (int i = n->nargs - 1; i >= 0; i--) pop(argreg[i]);
    int pad = depth % 2;     // keep rsp 16-byte aligned at the call
    if (pad) printf("  sub rsp, 8\n");
    printf("  mov rax, 0\n  call %s\n", n->name);
    if (pad) printf("  add rsp, 8\n");
    return;
  }
  default:
    break;
  }

  // binary operators: rhs -> rdi, lhs -> rax
  gen_expr(n->rhs);
  push();
  gen_expr(n->lhs);
  pop("rdi");
  switch (n->kind) {
  case N_ADD: printf("  add rax, rdi\n"); break;
  case N_SUB: printf("  sub rax, rdi\n"); break;
  case N_MUL: printf("  imul rax, rdi\n"); break;
  case N_DIV: printf("  cqo\n  idiv rdi\n"); break;
  case N_MOD: printf("  cqo\n  idiv rdi\n  mov rax, rdx\n"); break;
  case N_EQ:  printf("  cmp rax, rdi\n  sete al\n  movzx rax, al\n"); break;
  case N_NE:  printf("  cmp rax, rdi\n  setne al\n  movzx rax, al\n"); break;
  case N_LT:  printf("  cmp rax, rdi\n  setl al\n  movzx rax, al\n"); break;
  case N_LE:  printf("  cmp rax, rdi\n  setle al\n  movzx rax, al\n"); break;
  default: error("internal: bad expression node");
  }
}

static void gen_func(Func *f) {
  cur_fn = f->name;
  depth = 0;
  printf("  .globl %s\n%s:\n", f->name, f->name);
  printf("  push rbp\n  mov rbp, rsp\n  sub rsp, %d\n", f->stack_size);
  // spill register params into their stack slots (params are the first vars declared)
  for (int i = 0; i < f->nparams; i++)
    printf("  mov [rbp-%d], %s\n", (i + 1) * 8, argreg[i]);
  for (Node *s = f->body; s; s = s->next) gen_stmt(s);
  printf("  mov rax, 0\n");   // falling off the end returns 0
  printf(".L.return.%s:\n  mov rsp, rbp\n  pop rbp\n  ret\n", f->name);
}

// ---------- Main ----------
static char *read_file(const char *path) {
  FILE *fp = fopen(path, "r");
  if (!fp) error("cannot open %s", path);
  fseek(fp, 0, SEEK_END);
  long size = ftell(fp);
  rewind(fp);
  char *buf = calloc(1, size + 1);
  if (fread(buf, 1, size, fp) != (size_t)size) error("cannot read %s", path);
  fclose(fp);
  return buf;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <file.c>\n", argv[0]);
    return 1;
  }
  src = read_file(argv[1]);
  tokenize(src);

  Func head = {0}, *cur = &head;
  while (tok->kind != T_EOF) cur = cur->next = function();

  printf(".intel_syntax noprefix\n.text\n");
  for (Func *f = head.next; f; f = f->next) gen_func(f);
  printf(".section .rodata\n");
  for (Str *s = strings; s; s = s->next) {
    printf(".L.str.%d:\n  .byte ", s->id);
    for (int i = 0; i < s->len; i++) printf("%d,", (unsigned char)s->data[i]);
    printf("0\n");
  }
  printf(".text\n.section .note.GNU-stack,\"\",@progbits\n");
  return 0;
}
