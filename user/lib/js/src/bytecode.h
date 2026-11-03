/*
 * bytecode.h: the instruction set as an enum plus the operand conventions shared by compile.c and vm.c.
 */
#ifndef KZ_BYTECODE_H
#define KZ_BYTECODE_H

#include "jsint.h"

enum {
#define OP(name, size, pop, push) OP_##name,
#include "opcodes.h"
#undef OP
    OP_COUNT
};

typedef struct KzOpInfo { uint8_t size; int8_t pop, push; const char *name; } KzOpInfo;
extern const KzOpInfo kz_op_info[OP_COUNT];

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline int32_t rdi32(const uint8_t *p) { return (int32_t)rd32(p); }

/* special_object kinds */
enum { SO_ARGUMENTS = 1, SO_MAPPED_ARGUMENTS, SO_IMPORT_META };

/* define_method kinds (low bits) and flags */
#define DM_METHOD   0
#define DM_GETTER   1
#define DM_SETTER   2
#define DM_ENUM     4       /* enumerable (object literals) */
#define DM_FIELD    8       /* class element record: append to the constructor's field list */
#define DM_PRIVATE  16

/* with_* kinds */
#define WK_WITH 0           /* a with statement's object: Symbol.unscopables applies */
#define WK_EVAL 1           /* a sloppy direct eval's var object */

/* generator resume modes */
enum { RESUME_NEXT = 0, RESUME_THROW = 1, RESUME_RETURN = 2 };

/* completion record types (finally_end) */
#define COMPLETION_NORMAL 0
#define COMPLETION_THROW 1

/* class field records (the constructor's field list) */
enum { FR_FIELD = 0, FR_PRIVATE_METHOD = 1, FR_PRIVATE_GETTER = 2, FR_PRIVATE_SETTER = 3 };

/* the eval scope descriptor (KzVarName.kind) */
enum { VK_VAR = 0, VK_LET, VK_CONST, VK_CALLEE, VK_THIS, VK_NEW_TARGET, VK_HOME, VK_ARGUMENTS, VK_WITH, VK_EVAL_OBJ,
       VK_FUNC, VK_CLASS_INNER, VK_PRIVATE, VK_PARAM };

/* global declaration lists (OP_global_decls): an array [kind, name, kind, name, ...] */
enum { GD_VAR = 0, GD_FUNC, GD_LET, GD_CONST, GD_CLASS };

#endif
