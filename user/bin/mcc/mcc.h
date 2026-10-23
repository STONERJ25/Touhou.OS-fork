/* mcc -- Marisa's compiler for TouhouOS.  Shared declarations.
 *
 * mcc is built as one translation unit: main.c includes every *.inc file after this header.  That keeps the
 * program free of symbol clashes with libgap and makes it trivial for mcc to compile itself (stage 2).
 * The source is plain C11 without VLAs, so it builds with gcc/clang on a host and with mcc itself.
 */
#ifndef MCC_H
#define MCC_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed char i8;
typedef short i16;
typedef int i32;
typedef long long i64;

#define MCC_VERSION "0.1"
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#define MAX(x, y) ((x) < (y) ? (y) : (x))

/* ------------------------------------------------------------------ util.inc */

void *xmalloc(size_t n);
void *xcalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xformat(const char *fmt, ...);
char *xvformat(const char *fmt, va_list ap);
void fatal(const char *fmt, ...) __attribute__((noreturn));

typedef struct {
    u8 *p;
    size_t len, cap;
} Buf;

typedef struct {
    char *key;
    int keylen;
    void *val;
} MapEntry;

typedef struct {
    MapEntry *buckets;
    int capacity;
    int used;
} Map;

/* ------------------------------------------------------------------ ELF */

#define EI_NIDENT 16
typedef struct {
    u8 e_ident[EI_NIDENT];
    u16 e_type, e_machine;
    u32 e_version;
    u64 e_entry, e_phoff, e_shoff;
    u32 e_flags;
    u16 e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Ehdr;

typedef struct {
    u32 sh_name, sh_type;
    u64 sh_flags, sh_addr, sh_offset, sh_size;
    u32 sh_link, sh_info;
    u64 sh_addralign, sh_entsize;
} Shdr;

typedef struct {
    u32 p_type, p_flags;
    u64 p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Phdr;

typedef struct {
    u32 st_name;
    u8 st_info, st_other;
    u16 st_shndx;
    u64 st_value, st_size;
} ElfSym;

typedef struct {
    u64 r_offset, r_info;
    i64 r_addend;
} Rela;

enum { ET_REL = 1, ET_EXEC = 2 };
enum { SHT_NULL = 0, SHT_PROGBITS = 1, SHT_SYMTAB = 2, SHT_STRTAB = 3, SHT_RELA = 4, SHT_NOBITS = 8, SHT_GROUP = 17 };
enum { SHF_WRITE = 1, SHF_ALLOC = 2, SHF_EXECINSTR = 4, SHF_MERGE = 0x10, SHF_STRINGS = 0x20, SHF_INFO_LINK = 0x40, SHF_TLS = 0x400 };
enum { STB_LOCAL = 0, STB_GLOBAL = 1, STB_WEAK = 2 };
enum { STT_NOTYPE = 0, STT_OBJECT = 1, STT_FUNC = 2, STT_SECTION = 3, STT_FILE = 4, STT_COMMON = 5, STT_TLS = 6 };
enum { SHN_UNDEF = 0, SHN_ABS = 0xfff1, SHN_COMMON = 0xfff2 };
enum { PT_LOAD = 1, PT_TLS = 7, PT_GNU_STACK = 0x6474e551 };
enum { PF_X = 1, PF_W = 2, PF_R = 4 };
enum {
    R_X86_64_NONE = 0, R_X86_64_64 = 1, R_X86_64_PC32 = 2, R_X86_64_GOT32 = 3, R_X86_64_PLT32 = 4,
    R_X86_64_GOTPCREL = 9, R_X86_64_32 = 10, R_X86_64_32S = 11, R_X86_64_16 = 12, R_X86_64_PC16 = 13,
    R_X86_64_8 = 14, R_X86_64_PC8 = 15, R_X86_64_TPOFF64 = 18, R_X86_64_GOTTPOFF = 22, R_X86_64_TPOFF32 = 23,
    R_X86_64_PC64 = 24, R_X86_64_GOTPCRELX = 41, R_X86_64_REX_GOTPCRELX = 42
};

#define ELF64_R_SYM(i) ((u32)((i) >> 32))
#define ELF64_R_TYPE(i) ((u32)((i) & 0xffffffffu))
#define ELF64_R_INFO(s, t) (((u64)(s) << 32) | (u64)(t))
#define ELF64_ST_BIND(i) ((i) >> 4)
#define ELF64_ST_TYPE(i) ((i) & 0xf)
#define ELF64_ST_INFO(b, t) (((b) << 4) | ((t) & 0xf))

/* ------------------------------------------------------------------ the front end */

typedef struct Type Type;
typedef struct Node Node;
typedef struct Member Member;
typedef struct Token Token;
typedef struct Obj Obj;
typedef struct Relocation Relocation;
typedef struct Hideset Hideset;
typedef struct File File;

struct File {
    char *name;
    int file_no;
    char *contents;
    char *display_name; /* the name __FILE__ gives (changed by #line) */
    int line_delta;
    int inc_idx; /* index of the include directory it was found in (-1: not via the search path) */
    int depth;   /* #include nesting level */
    bool is_system;
};

typedef enum {
    TK_IDENT,   /* identifiers */
    TK_PUNCT,   /* punctuators */
    TK_KEYWORD, /* keywords */
    TK_STR,     /* string literals */
    TK_NUM,     /* numeric literals */
    TK_PP_NUM,  /* preprocessing numbers */
    TK_EOF,     /* end-of-file markers */
} TokenKind;

struct Token {
    TokenKind kind;
    Token *next;
    i64 val;
    double fval;
    Type *ty;
    char *loc;
    int len;
    char *str;
    File *file;
    char *filename;
    int line_no;
    int line_delta;
    bool at_bol;
    bool has_space;
    Hideset *hideset;
    Token *origin;
};

typedef enum {
    TY_VOID, TY_BOOL, TY_CHAR, TY_SHORT, TY_INT, TY_LONG, TY_FLOAT, TY_DOUBLE, TY_ENUM, TY_PTR, TY_FUNC, TY_ARRAY,
    TY_STRUCT, TY_UNION,
} TypeKind;

struct Type {
    TypeKind kind;
    int size;      /* sizeof() value; -1 while incomplete */
    int align;
    bool is_unsigned;
    bool is_const;
    bool is_volatile;
    Type *origin; /* the type this one was copied from (for compatibility checks) */
    Type *base;   /* pointer-to / array-of / enum: the integer type */
    Token *name;
    Token *name_pos;
    int array_len; /* -1: unknown size */
    Member *members;
    bool is_flexible;
    bool is_packed;
    int attr_align; /* __attribute__((aligned(n))) on the type, 0 if none */
    Type *return_ty;
    Type *params;
    bool is_variadic;
    bool has_proto;
    Type *next;
};

struct Member {
    Member *next;
    Type *ty;
    Token *tok;
    Token *name;
    int idx;
    int align;
    int offset;
    bool is_bitfield;
    int bit_offset;
    int bit_width;
};

struct Relocation {
    Relocation *next;
    int offset;
    Obj *target;
    i64 addend;
};

struct Obj {
    Obj *next;
    char *name;
    Type *ty;
    Token *tok;
    bool is_local;
    int align;
    int offset; /* locals: rbp offset */
    char *asm_reg; /* register long r10 __asm__("r10"): the register an asm operand wants */
    /* globals and functions */
    bool is_function;
    bool is_definition;
    bool is_static;
    bool is_tentative;
    bool is_tls;
    bool is_weak;
    bool is_inline;
    bool decl_static;    /* declared `static` somewhere */
    bool decl_noninline; /* declared without `inline` (or with `extern`) somewhere */
    bool is_live;
    bool is_const_data;
    bool is_string; /* anonymous string literal */
    char *init_data;
    Relocation *rel;
    /* functions */
    Obj *params;
    Node *body;
    Obj *locals;
    Obj *va_area;
    int stack_size;
    int va_stack_top; /* rbp offset of the first stack-passed variadic argument */
    Obj **refs;       /* functions and static data this definition uses (for dropping unused statics) */
    int nrefs;
    /* code generation */
    int sec;
    int sec_off;
    int size_out;
    int sym_index;
    bool emitted;
};

typedef enum {
    ND_NULL_EXPR, ND_ADD, ND_SUB, ND_MUL, ND_DIV, ND_NEG, ND_MOD, ND_BITAND, ND_BITOR, ND_BITXOR, ND_SHL, ND_SHR,
    ND_EQ, ND_NE, ND_LT, ND_LE, ND_ASSIGN, ND_COND, ND_COMMA, ND_MEMBER, ND_ADDR, ND_DEREF, ND_NOT, ND_BITNOT,
    ND_LOGAND, ND_LOGOR, ND_RETURN, ND_IF, ND_FOR, ND_DO, ND_SWITCH, ND_CASE, ND_BLOCK, ND_GOTO, ND_LABEL,
    ND_FUNCALL, ND_EXPR_STMT, ND_STMT_EXPR, ND_VAR, ND_NUM, ND_CAST, ND_MEMZERO, ND_ASM,
    ND_VA_START, ND_VA_ARG, ND_VA_COPY, ND_ALLOCA, ND_THREAD_PTR, ND_BUILTIN, ND_ATOMIC,
} NodeKind;

struct Node {
    NodeKind kind;
    Node *next;
    Type *ty;
    Token *tok;
    Node *lhs;
    Node *rhs;
    /* if / for / do / switch */
    Node *cond;
    Node *then;
    Node *els;
    Node *init;
    Node *inc;
    int brk_label;
    int cont_label;
    /* block or statement expression */
    Node *body;
    /* struct member access */
    Member *member;
    /* function call */
    Type *func_ty;
    Node *args;
    Obj *ret_buffer;
    bool pass_by_stack;
    /* goto and labels */
    char *label;
    int label_id;
    Node *goto_next;
    /* switch / case */
    Node *case_next;
    Node *default_case;
    i64 val2; /* end of a case range */
    /* variable / numeric literal */
    Obj *var;
    i64 val;
    double fval;
    /* inline asm, builtins */
    char *str;
    int op;
    void *asm_info;
};

/* inline asm operands (parse.inc fills them in, asm.inc uses them) */
typedef struct {
    char *cons;
    Node *expr;
    char *name;
    int kind; /* 1 register, 2 immediate, 3 memory */
    int reg;
    bool rw;       /* "+": read and written */
    bool is_out;
    int match;     /* input tied to output #match, or -1 */
    int mem_disp;  /* a frame variable used as a memory operand: its rbp offset */
    bool direct_mem;
} AsmOperand;

typedef struct {
    AsmOperand *ops;
    int nout, nin;
    char **clobbers;
    int nclob;
} AsmInfo;

/* type.inc */
extern Type *ty_void, *ty_bool, *ty_char, *ty_short, *ty_int, *ty_long, *ty_uchar, *ty_ushort, *ty_uint, *ty_ulong,
    *ty_float, *ty_double;
bool is_integer(Type *ty);
bool is_flonum(Type *ty);
bool is_numeric(Type *ty);
bool is_compatible(Type *t1, Type *t2);
Type *copy_type(Type *ty);
Type *pointer_to(Type *base);
Type *func_type(Type *return_ty);
Type *array_of(Type *base, int len);
Type *enum_type(void);
Type *struct_type(void);
void add_type(Node *node);
Node *new_cast(Node *expr, Type *ty);

/* lex.inc, pp.inc, parse.inc */
void error_at(char *loc, const char *fmt, ...) __attribute__((noreturn));
void error_tok(Token *tok, const char *fmt, ...) __attribute__((noreturn));
void warn_tok(Token *tok, const char *fmt, ...);
bool equal(Token *tok, const char *op);
Token *skip(Token *tok, const char *op);
bool consume(Token **rest, Token *tok, const char *str);
void convert_pp_tokens(Token *tok);
File **get_input_files(void);
File *new_file(char *name, int file_no, char *contents);
Token *tokenize_string_literal(Token *tok, Type *basety);
Token *tokenize(File *file);
Token *tokenize_file(char *path);
Token *preprocess(Token *tok);
Obj *parse(Token *tok);
void pp_add_include_path(char *path);
void define_macro(char *name, char *buf);
void undef_macro(char *name);

#define ICE() fatal("internal compiler error at %s:%d (Marisa dropped something)", __FILE__, __LINE__)

#endif
