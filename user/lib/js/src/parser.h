/*
 * parser.h: the front end's data: tokens (lexer.c), the syntax tree and the scopes (parser.c), shared with the
 * resolver and the bytecode compiler (compile.c).
 *
 * The parser builds a tree for a whole script, module or function body, declares every binding in its scope while
 * parsing (so that early errors about redeclarations come out with the right position), and records for each
 * identifier reference the scope it appears in.  compile.c then resolves references (marking variables that inner
 * functions capture) and emits bytecode.  All tree memory comes from an arena freed after compilation; JavaScript
 * values the tree holds (names, literals) are kept alive by the parser's rooted value list.
 */
#ifndef KZ_PARSER_H
#define KZ_PARSER_H

#include "jsint.h"

/* ------------------------------------------------------------------ tokens */

enum {
    T_EOF = 0,
    T_NAME,             /* identifier or keyword (kw says which) */
    T_PRIVATE,          /* #name */
    T_NUMBER,
    T_BIGINT,
    T_STRING,
    T_TEMPLATE,         /* a template chunk (cooked, raw, tail) */
    T_REGEXP,
    /* punctuators */
    T_LBRACE, T_RBRACE, T_LPAREN, T_RPAREN, T_LBRACKET, T_RBRACKET, T_DOT, T_ELLIPSIS, T_SEMI, T_COMMA,
    T_LT, T_GT, T_LE, T_GE, T_EQ, T_NE, T_SEQ, T_SNE, T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_STARSTAR,
    T_INC, T_DEC, T_SHL, T_SAR, T_SHR, T_AMP, T_PIPE, T_CARET, T_BANG, T_TILDE, T_ANDAND, T_OROR, T_QQ,
    T_QUESTION, T_QDOT, T_COLON, T_ASSIGN, T_PLUS_ASSIGN, T_MINUS_ASSIGN, T_STAR_ASSIGN, T_SLASH_ASSIGN,
    T_PERCENT_ASSIGN, T_STARSTAR_ASSIGN, T_SHL_ASSIGN, T_SAR_ASSIGN, T_SHR_ASSIGN, T_AMP_ASSIGN, T_PIPE_ASSIGN,
    T_CARET_ASSIGN, T_ANDAND_ASSIGN, T_OROR_ASSIGN, T_QQ_ASSIGN, T_ARROW, T_AT,
    T_COUNT
};

/* keywords (T_NAME tokens spelled without escapes) */
enum {
    KW_NONE = 0,
    KW_await, KW_break, KW_case, KW_catch, KW_class, KW_const, KW_continue, KW_debugger, KW_default, KW_delete,
    KW_do, KW_else, KW_enum, KW_export, KW_extends, KW_false, KW_finally, KW_for, KW_function, KW_if, KW_import,
    KW_in, KW_instanceof, KW_new, KW_null, KW_return, KW_super, KW_switch, KW_this, KW_throw, KW_true, KW_try,
    KW_typeof, KW_var, KW_void, KW_while, KW_with, KW_yield,
    /* strict-mode reserved */
    KW_let, KW_static, KW_implements, KW_interface, KW_package, KW_private, KW_protected, KW_public,
    /* contextual (never reserved) */
    KW_async, KW_of, KW_get, KW_set, KW_target, KW_meta, KW_as, KW_from, KW_eval, KW_arguments,
    KW_COUNT
};
#define KW_FIRST_STRICT KW_let
#define KW_FIRST_CONTEXTUAL KW_async

typedef struct Token {
    int type;
    int kw;
    bool nl_before;     /* a line terminator between the previous token and this one */
    bool escaped;       /* a name spelled with \u escapes */
    bool legacy_octal;  /* a string with an octal escape / a legacy octal number (strict-mode errors) */
    bool tail;          /* templates: ends with ` */
    uint32_t pos, end;  /* byte offsets */
    uint32_t line, col;
    double num;
    JSValue val;        /* names: atom; strings: value; templates: cooked (undefined if invalid); regexp: body */
    JSValue raw;        /* templates: raw; regexp: flags; bigint: digit text */
} Token;

typedef struct Lexer {
    JSContext *ctx;
    const uint8_t *src;
    uint32_t len;
    uint32_t pos, line, line_start;   /* line_start: byte offset where the current line begins */
    uint32_t col_cache_pos, col_cache_col, col_cache_line;
    Token tok;              /* the current token */
    uint32_t prev_end;      /* end of the previous token */
    uint32_t prev_pos;      /* start of the previous token (`async` when a function or arrow that follows it begins) */
    uint32_t prev_line;
    bool html_comments;     /* scripts allow <!-- and --> comments (Annex B) */
    struct Parser *p;
} Lexer;

/* ------------------------------------------------------------------ the tree */

typedef struct Node Node;
typedef struct NodeList { Node **v; int n, cap; } NodeList;
typedef struct Scope Scope;
typedef struct Binding Binding;
typedef struct FuncInfo FuncInfo;

enum {
    N_PROGRAM = 1,
    /* statements */
    N_VAR,              /* op: V_VAR / V_LET / V_CONST; list: N_DECLARATOR */
    N_DECLARATOR,       /* a: target (name or pattern), b: initialiser */
    N_FUNC_DECL,        /* func */
    N_CLASS_DECL,       /* a: N_CLASS */
    N_EXPR_STMT,        /* a */
    N_BLOCK,            /* list, scope */
    N_EMPTY,
    N_IF,               /* a test, b then, c else */
    N_FOR,              /* a init, b test, c update, d body; scope (for the let/const head) */
    N_FOR_IN,           /* a left (N_VAR or target), b right, d body; scope */
    N_FOR_OF,           /* same; flags F_AWAIT */
    N_WHILE,            /* a test, d body */
    N_DO_WHILE,         /* a test, d body */
    N_CONTINUE,         /* val: label or 0 */
    N_BREAK,
    N_RETURN,           /* a */
    N_WITH,             /* a object, b body; scope */
    N_SWITCH,           /* a discriminant, list: N_CASE; scope */
    N_CASE,             /* a test (NULL: default), list: statements */
    N_LABELED,          /* val label, a statement */
    N_THROW,            /* a */
    N_TRY,              /* a block, b catch parameter (or NULL), c catch block (or NULL), d finally (or NULL); scope: catch */
    N_DEBUGGER,
    N_IMPORT,           /* module: val specifier; list: N_IMPORT_SPEC */
    N_IMPORT_SPEC,      /* val imported name ("*" for namespace, "default"), val2 local name */
    N_EXPORT,           /* op: EX_NAMED / EX_DEFAULT_EXPR / EX_DECL / EX_ALL / EX_FROM; a: declaration/expression; list: N_EXPORT_SPEC; val: from */
    N_EXPORT_SPEC,      /* val local (or imported), val2 exported */
    /* expressions */
    N_IDENT,            /* val: atom; binding filled in by the resolver */
    N_NUMBER,           /* num */
    N_BIGINT,           /* val: digits text, ival: radix */
    N_STRING,           /* val */
    N_TEMPLATE,         /* list: alternating N_TEMPLATE_PART and expressions */
    N_TEMPLATE_PART,    /* val: cooked (undefined if invalid), val2: raw */
    N_TAGGED,           /* a: tag, b: N_TEMPLATE */
    N_REGEXP,           /* val: body, val2: flags */
    N_NULL, N_TRUE, N_FALSE,
    N_THIS,
    N_SUPER,            /* only as the object of N_MEMBER or the callee of N_CALL */
    N_NEW_TARGET,
    N_IMPORT_META,
    N_ARRAY,            /* list: elements, NULL for holes, N_SPREAD */
    N_OBJECT,           /* list: N_PROP */
    N_PROP,             /* op: P_INIT P_GET P_SET P_METHOD P_SPREAD P_SHORTHAND P_PROTO; a: key, b: value; flags F_COMPUTED */
    N_FUNC,             /* func (expression) */
    N_ARROW,            /* func */
    N_CLASS,            /* val: name (or 0), a: heritage, list: N_CLASS_ELEM; scope: class scope; func: constructor */
    N_CLASS_ELEM,       /* op: CE_METHOD CE_GET CE_SET CE_FIELD CE_STATIC_BLOCK; a: key, b: value/function; flags F_STATIC F_COMPUTED F_PRIVATE_KEY */
    N_UNARY,            /* op: token (T_MINUS T_PLUS T_BANG T_TILDE) or KW_typeof KW_void KW_delete (+ 1000) */
    N_UPDATE,           /* op: T_INC/T_DEC, flags F_PREFIX; a */
    N_BINARY,           /* op: token; a, b */
    N_LOGICAL,          /* op: T_ANDAND T_OROR T_QQ; a, b */
    N_ASSIGN,           /* op: T_ASSIGN or a compound token; a target, b value */
    N_COND,             /* a ? b : c */
    N_CALL,             /* a callee, list args; flags F_OPTIONAL (callee?.()), F_DIRECT_EVAL */
    N_NEW,              /* a callee, list args */
    N_MEMBER,           /* a object, b computed key (or NULL), val: name for dot access; flags F_COMPUTED F_OPTIONAL */
    N_PRIVATE_MEMBER,   /* a object, val: private name; flags F_OPTIONAL */
    N_OPT_CHAIN,        /* a: the chain (members and calls with F_OPTIONAL links) */
    N_SEQ,              /* list */
    N_SPREAD,           /* a */
    N_YIELD,            /* a (or NULL); flags F_DELEGATE */
    N_AWAIT,            /* a */
    N_IMPORT_CALL,      /* a specifier */
    N_PRIVATE_IN,       /* val: private name, b: object */
    N_PAREN,            /* a: a parenthesised expression (kept to tell (a) = 1 from patterns; removed later) */
    N_PRIVATE_NAME,     /* val (class element keys) */
    /* patterns */
    N_OBJ_PATTERN,      /* list: N_PAT_PROP, the last may be N_REST */
    N_ARR_PATTERN,      /* list: targets, NULL holes, last may be N_REST */
    N_PAT_PROP,         /* a key, b target (maybe N_ASSIGN_PAT); flags F_COMPUTED */
    N_ASSIGN_PAT,       /* a target, b default */
    N_REST,             /* a target */
    N_COUNT
};

enum { V_VAR = 1, V_LET, V_CONST };
enum { P_INIT = 1, P_GET, P_SET, P_METHOD, P_SPREAD, P_SHORTHAND, P_PROTO };
enum { CE_METHOD = 1, CE_GET, CE_SET, CE_FIELD, CE_STATIC_BLOCK };
enum { EX_NAMED = 1, EX_DEFAULT_EXPR, EX_DEFAULT_DECL, EX_DECL, EX_ALL, EX_FROM };
#define OP_KW 1000

/* node flags */
#define F_COMPUTED   0x0001
#define F_OPTIONAL   0x0002
#define F_PREFIX     0x0004
#define F_DELEGATE   0x0008
#define F_AWAIT      0x0010
#define F_STATIC     0x0020
#define F_DIRECT_EVAL 0x0040
#define F_PRIVATE_KEY 0x0080
#define F_PARENS     0x0100     /* written in parentheses (cover grammar) */
#define F_SHORTHAND_INIT 0x0200 /* { a = 1 } cover initialiser: only valid as a pattern */
#define F_NAMED_ANON 0x0400     /* an anonymous function/class definition (gets a name from its context) */
#define F_TAIL_EXPR  0x0800

struct Node {
    uint16_t type;
    uint16_t flags;
    int32_t op;
    uint32_t pos, end;
    uint32_t line, col;
    double num;
    JSValue val, val2;
    Node *a, *b, *c, *d;
    NodeList list;
    FuncInfo *func;
    Scope *scope;           /* scope-bearing nodes: their scope; identifiers: the scope they appear in */
    Binding *binding;       /* resolved identifier */
    int32_t ival;
};

/* ------------------------------------------------------------------ scopes and bindings */

enum {
    BK_VAR = 1, BK_LET, BK_CONST, BK_CLASS, BK_FUNC, BK_PARAM, BK_CATCH, BK_CALLEE, BK_IMPORT, BK_ARGUMENTS,
    BK_HIDDEN               /* this, new.target, home object, with objects, the eval var object */
};
#define BF_CAPTURED   0x01  /* used by an inner function: lives in a var cell */
#define BF_TDZ        0x02  /* reads must check for UNINIT */
#define BF_ASSIGNED   0x04
#define BF_ANNEXB     0x08  /* a block function also given a var binding (Annex B.3.3) */
#define BF_LOOP       0x10  /* declared in a loop head or body: fresh per iteration */
#define BF_SIMPLE_CATCH 0x20
#define BF_PRECELL    0x40  /* module scope: the variable cell is made by the linker (exported and imported bindings) */
#define BF_OUTER      0x80  /* eval's synthesized caller: a binding of a function outside the one eval code declares its vars in */
#define BF_PARAMENV   0x100 /* eval's synthesized caller: a binding of the parameter environment of a function with parameter expressions */

struct Binding {
    JSValue name;
    uint8_t kind;
    uint16_t flags;
    int16_t slot;           /* local slot in the frame (assigned by the compiler) */
    Scope *scope;
    Binding *next;
    uint32_t decl_pos;      /* source position of the declaration (TDZ check elision) */
    uint32_t decl_end;
    Node *decl;             /* the declaring node, if any */
    int32_t export_index;   /* module bindings */
};

enum { SC_FUNC = 1, SC_PARAMS, SC_BLOCK, SC_CATCH, SC_FOR, SC_SWITCH, SC_CLASS, SC_WITH, SC_SCRIPT, SC_MODULE, SC_EVAL, SC_CALLEE };

struct Scope {
    uint8_t kind;
    bool has_eval;          /* a direct eval directly inside (makes the scope's bindings visible by name) */
    bool needs_cells;       /* every binding must be a cell (eval, with) */
    Scope *parent;
    FuncInfo *func;         /* the function this scope belongs to */
    Binding *first, *last;
    NodeList var_names;     /* (as N_IDENT) names var-declared in or through this scope, for conflict checks */
    Binding *with_obj;      /* SC_WITH: the hidden binding holding the object */
    Binding *eval_obj;      /* function scopes with a sloppy direct eval: the object holding eval's vars */
    uint32_t start, end;
    Scope *next_in_func;    /* the function's scopes, as a list */
    Node *class_node;       /* SC_CLASS: its class */
};

/* function flags */
#define FN_ARROW       0x0001
#define FN_ASYNC       0x0002
#define FN_GENERATOR   0x0004
#define FN_METHOD      0x0008
#define FN_GETTER      0x0010
#define FN_SETTER      0x0020
#define FN_CTOR        0x0040   /* class constructor */
#define FN_DERIVED     0x0080
#define FN_STRICT      0x0100
#define FN_SIMPLE      0x0200   /* simple parameter list */
#define FN_EXPR_BODY   0x0400   /* arrow with an expression body */
#define FN_FIELD_INIT  0x0800   /* class field initialiser (synthetic) */
#define FN_STATIC_BLOCK 0x1000
#define FN_SCRIPT      0x2000
#define FN_MODULE      0x4000
#define FN_EVAL        0x8000
#define FN_DECL        0x10000  /* a function declaration */
#define FN_EXPR_NAMED  0x20000  /* a named function expression */
#define FN_CLASS_FIELDS 0x40000 /* the constructor must run field initialisers */
#define FN_GLOBAL_VARS 0x80000  /* eval code whose var and function declarations are global properties */
#define FN_DIRECT_EVAL 0x100000 /* direct eval code: this, new.target, super and arguments are the caller's */
#define FN_SYNTH_CALLER 0x200000 /* the stand-in for a direct eval's caller (its scopes come from the call site) */

typedef struct CapturedVar { Binding *b; int index; struct CapturedVar *next; } CapturedVar;

struct FuncInfo {
    uint32_t flags;
    JSValue name;           /* atom, or 0 */
    NodeList params;
    NodeList body;
    Scope *scope;           /* parameters (and the body when the list is simple) */
    Scope *body_scope;      /* the body's var scope (== scope for simple lists) */
    FuncInfo *parent;
    Node *node;
    Node *class_node;       /* constructors and field initialisers: their class */
    uint32_t src_start, src_end, line, col;
    int nparams_len;        /* the "length" */
    /* filled by the parser */
    bool uses_arguments, uses_this, uses_new_target, uses_super_call, uses_super_prop, has_direct_eval, has_yield;
    bool eval_in_params;    /* a direct eval inside the parameter expressions (its vars live in an environment of their own) */
    bool contains_await;
    bool arguments_shadowed;    /* a parameter or lexical named arguments */
    /* filled by the compiler */
    Binding *this_b, *new_target_b, *home_b, *arguments_b, *fields_b;
    CapturedVar *captured;  /* this function's closure variables, in index order */
    CapturedVar *captured_tail;
    int ncaptured;
    int nslots;
    struct FuncInfo *next_child, *first_child;
    Scope *scopes;          /* every scope whose bindings live in this function's frame */
    NodeList annexb;        /* sloppy block-level function declarations (Annex B.3.3 candidates) */
    Node *fields_class;     /* constructors: the class whose instance elements need initialising */
    bool has_instance_elements;
    int ival_synth;         /* synthesised default constructors: 1 base, 2 derived */
    JSValue field_name;     /* field initialisers: the name anonymous functions in it get */
    NodeList eval_vars;     /* sloppy direct eval code: var/function names that belong to the caller */
};

typedef struct Arena Arena;

typedef struct Parser {
    JSContext *ctx;
    Lexer lx;
    Arena *arena;
    KzVec *values;          /* rooted JavaScript values the tree holds */
    JSValue filename;
    JSValue source;         /* the text as a KZ_GC_SOURCE cell (JV_MKPTR), kept by the bytecode */
    bool strict, module;
    bool in_function, in_class_field;
    FuncInfo *fn;           /* the function being parsed */
    Scope *scope;           /* the innermost scope */
    int depth;              /* recursion guard */
    int labels_n;
    JSValue labels[64];     /* every label around the statement being parsed (within the function) */
    bool label_is_loop[64];
    int label_pending;      /* how many of the innermost labels label the statement being parsed directly */
    bool in_params;         /* parsing a formal parameter list: no yield/await expressions */
    int loop_depth, switch_depth;
    bool failed;            /* a SyntaxError is pending */
    uint32_t err_line, err_col;
    /* eval: the names visible from the caller, and how the eval may declare vars */
    int eval_kind;          /* 0 none, 1 direct sloppy, 2 direct strict, 3 indirect */
    bool eval_in_function, eval_new_target_ok, eval_super_prop_ok, eval_super_call_ok, eval_in_class_field;
    NodeList private_names; /* private names declared by the enclosing classes (stack), for early errors */
    int class_depth;
    struct PrivateRef { JSValue name; uint32_t line, col; int class_depth; } *private_refs;
    int nprivate_refs, private_refs_cap;
    JSValue *outer_private;     /* eval inside a class: private names visible from outside */
    int nouter_private;
} Parser;

/* lexer.c */
void kz_lex_init(Lexer *lx, JSContext *ctx, const uint8_t *src, uint32_t len);
int kz_lex_next(Lexer *lx);                         /* reads the next token into lx->tok; -1 on a syntax error */
int kz_lex_regexp(Lexer *lx);                       /* re-reads the current '/' or '/=' token as a regexp */
int kz_lex_template_continue(Lexer *lx);            /* after '}' of a substitution: the next template chunk */
uint32_t kz_lex_col(Lexer *lx, uint32_t pos, uint32_t *line);
int kz_keyword_of(JSValue atom, JSRuntime *rt);
void kz_lex_error(Lexer *lx, uint32_t pos, const char *fmt, ...);

/* parser.c */
void *kz_arena_alloc(Parser *p, size_t n);
Node *kz_parse_program(Parser *p);                  /* NULL on error (exception pending) */
int kz_parser_init(Parser *p, JSContext *ctx, const char *src, size_t len, JSValue filename, KzVec *values);
void kz_parser_free(Parser *p);
JSValue kz_parser_keep(Parser *p, JSValue v);       /* roots v for the parser's lifetime; JS_EXCEPTION on OOM */
Binding *kz_scope_find(Scope *s, JSValue name);
Binding *kz_declare_hidden(Parser *p, Scope *s, JSValue name);
int kz_list_push(Parser *p, NodeList *l, Node *n);

/* compile.c */
JSFunctionBytecode *kz_compile_program(Parser *p, Node *prog);

#endif
