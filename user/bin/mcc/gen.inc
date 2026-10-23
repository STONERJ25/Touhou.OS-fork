/* gen.inc -- code generation: the AST is turned straight into x86-64 machine code.
 *
 * A simple accumulator machine as in chibicc: an expression leaves its value in rax (integers, pointers, the address
 * of a struct) or xmm0 (float, double); binary operations push the right operand and pop it into rdi.  A few cheap
 * peepholes avoid the push/pop when the right operand is a constant or a local variable, and loads and stores of
 * variables with a known frame/RIP address use that address directly.  Calls follow the System V AMD64 ABI.
 */

#define GP_MAX 6
#define FP_MAX 8

static const int argreg[GP_MAX] = {RDI, RSI, RDX, RCX, R8, R9};

static int depth;
static Obj *current_fn_gen;
static int ret_label;

static void gen_expr(Node *node);
static void gen_stmt(Node *node);
static void gen_asm(Node *node);

static void push(void) {
    push_r(RAX);
    depth++;
}

static void pop(int reg) {
    pop_r(reg);
    depth--;
}

static void pushf(void) {
    sub_rsp(8);
    movsd_store(mem_reg(RSP, 0), 0);
    depth++;
}

static void popf(int x) {
    movsd_load(x, mem_reg(RSP, 0));
    add_rsp(8);
    depth--;
}

static Mem mem_rbp(int off) {
    return mem_reg(RBP, off);
}

/* ---- struct classification for the System V ABI */

static bool has_flonum(Type *ty, int lo, int hi, int offset) {
    if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
        for (Member *mem = ty->members; mem; mem = mem->next)
            if (!has_flonum(mem->ty, lo, hi, offset + mem->offset)) return false;
        return true;
    }

    if (ty->kind == TY_ARRAY) {
        for (int i = 0; i < ty->array_len; i++)
            if (!has_flonum(ty->base, lo, hi, offset + ty->base->size * i)) return false;
        return true;
    }

    return offset < lo || hi <= offset || ty->kind == TY_FLOAT || ty->kind == TY_DOUBLE;
}

static bool has_flonum1(Type *ty) {
    return has_flonum(ty, 0, 8, 0);
}

static bool has_flonum2(Type *ty) {
    return has_flonum(ty, 8, 16, 0);
}

/* ---- frame layout */

static void assign_lvar_offsets(Obj *fn) {
    /* parameters passed on the stack live in the caller's frame: the first one is at rbp+16 */
    int top = 16;
    int bottom = 0;
    int gp = 0, fp = 0;

    for (Obj *var = fn->params; var; var = var->next) {
        Type *ty = var->ty;

        switch (ty->kind) {
        case TY_STRUCT:
        case TY_UNION:
            if (ty->size <= 16) {
                bool fp1 = has_flonum1(ty);
                bool fp2 = has_flonum2(ty);
                int need_fp = (ty->size > 8 ? fp1 + fp2 : fp1), need_gp = (ty->size > 8 ? !fp1 + !fp2 : !fp1);
                if (fp + need_fp <= FP_MAX && gp + need_gp <= GP_MAX) {
                    fp += need_fp;
                    gp += need_gp;
                    continue;
                }
            }
            break;
        case TY_FLOAT:
        case TY_DOUBLE:
            if (fp++ < FP_MAX) continue;
            break;
        default:
            if (gp++ < GP_MAX) continue;
        }

        top = align_to(top, 8);
        var->offset = top;
        top += var->ty->size;
    }
    fn->va_stack_top = align_to(top, 8);

    /* the parser gave every local variable its slot (blocks that do not overlap share them) */
    for (Obj *var = fn->locals; var; var = var->next) {
        if (var->offset) continue;
        int align = (var->ty->kind == TY_ARRAY && var->ty->size >= 16) ? MAX(16, var->align) : var->align;
        bottom = MAX(bottom, fn->stack_size);
        bottom += var->ty->size > 0 ? var->ty->size : 1;
        bottom = align_to(bottom, align ? align : 1);
        var->offset = -bottom;
    }
    fn->stack_size = align_to(MAX(bottom, fn->stack_size), 16);
}

/* ---- loads, stores, conversions */

static void copy_bytes(int src, int dst, int dst_off, int size) {
    /* copies `size` bytes from [src] to [dst + dst_off]; clobbers r8 (and rsi/rdi/rcx for big blocks) */
    if (size > 96) {
        if (src != RSI) mov_rr(RSI, src);
        lea(RDI, mem_reg(dst, dst_off));
        mov_ri(RCX, size);
        rep_movsb();
        return;
    }
    int off = 0;
    while (off < size) {
        int n = size - off >= 8 ? 8 : size - off >= 4 ? 4 : size - off >= 2 ? 2 : 1;
        load_mem(R8, mem_reg(src, off), n, true);
        store_mem(mem_reg(dst, dst_off + off), R8, n);
        off += n;
    }
}

static void gen_addr(Node *node);

/* Is the address of this lvalue known as [rbp+disp] or [rip+sym+disp]?  Then it can be used as it is. */
static bool simple_addr(Node *node, Mem *m) {
    if (node->kind == ND_VAR) {
        Obj *var = node->var;
        if (var->is_local) {
            *m = mem_rbp(var->offset);
            return true;
        }
        if (var->is_tls) return false;
        *m = mem_sym(var, 0);
        return true;
    }
    if (node->kind == ND_MEMBER && !node->member->is_bitfield) {
        if (node->lhs->ty->kind != TY_STRUCT && node->lhs->ty->kind != TY_UNION) return false;
        if (!simple_addr(node->lhs, m)) return false;
        m->disp += node->member->offset;
        return true;
    }
    return false;
}

static void load_typed(Mem m, Type *ty) {
    switch (ty->kind) {
    case TY_ARRAY:
    case TY_STRUCT:
    case TY_UNION:
    case TY_FUNC:
        lea(RAX, m);
        return;
    case TY_FLOAT:
        movss_load(0, m);
        return;
    case TY_DOUBLE:
        movsd_load(0, m);
        return;
    default:
        load_mem(RAX, m, ty->size, ty->is_unsigned);
    }
}

static void store_typed(Mem m, Type *ty) {
    switch (ty->kind) {
    case TY_FLOAT:
        movss_store(m, 0);
        return;
    case TY_DOUBLE:
        movsd_store(m, 0);
        return;
    default:
        store_mem(m, RAX, ty->size);
    }
}

/* Load a value from where rax is pointing to. */
static void load(Type *ty) {
    switch (ty->kind) {
    case TY_ARRAY:
    case TY_STRUCT:
    case TY_UNION:
    case TY_FUNC:
        /* the value of an array or struct is its address */
        return;
    case TY_FLOAT:
        movss_load(0, mem_reg(RAX, 0));
        return;
    case TY_DOUBLE:
        movsd_load(0, mem_reg(RAX, 0));
        return;
    default:
        load_mem(RAX, mem_reg(RAX, 0), ty->size, ty->is_unsigned);
    }
}

/* Store rax (or xmm0) to the address on the top of the stack. */
static void store(Type *ty) {
    pop(RDI);

    switch (ty->kind) {
    case TY_STRUCT:
    case TY_UNION:
        copy_bytes(RAX, RDI, 0, ty->size);
        return;
    case TY_FLOAT:
        movss_store(mem_reg(RDI, 0), 0);
        return;
    case TY_DOUBLE:
        movsd_store(mem_reg(RDI, 0), 0);
        return;
    default:
        store_mem(mem_reg(RDI, 0), RAX, ty->size);
    }
}

static void cmp_zero(Type *ty) {
    if (ty->kind == TY_FLOAT) {
        xorp(false, 1, 1);
        ucomi(false, 0, 1);
        return;
    }
    if (ty->kind == TY_DOUBLE) {
        xorp(true, 1, 1);
        ucomi(true, 0, 1);
        return;
    }
    if (is_integer(ty) && ty->size <= 4) test_rr(0, RAX, RAX);
    else test_rr(1, RAX, RAX);
}

enum { I8, I16, I32, I64, U8, U16, U32, U64, F32, F64 };

static int type_id(Type *ty) {
    switch (ty->kind) {
    case TY_BOOL:
        return U8;
    case TY_CHAR:
        return ty->is_unsigned ? U8 : I8;
    case TY_SHORT:
        return ty->is_unsigned ? U16 : I16;
    case TY_INT:
    case TY_ENUM:
        return ty->is_unsigned ? U32 : I32;
    case TY_LONG:
        return ty->is_unsigned ? U64 : I64;
    case TY_FLOAT:
        return F32;
    case TY_DOUBLE:
        return F64;
    default:
        return U64;
    }
}

static void gen_cast(Type *from, Type *to) {
    if (to->kind == TY_VOID) return;

    if (from->kind == TY_STRUCT || from->kind == TY_UNION || to->kind == TY_STRUCT || to->kind == TY_UNION) return;

    if (to->kind == TY_BOOL) {
        if (is_flonum(from)) {
            bool dbl = from->kind == TY_DOUBLE;
            xorp(dbl, 1, 1);
            ucomi(dbl, 0, 1);
            setcc(CC_NE, RAX);
            setcc(CC_P, RDX);
            i_rr(0, 0, 0x08, RDX, RAX, 0); /* or al, dl */
        } else {
            cmp_zero(from);
            setcc(CC_NE, RAX);
        }
        movzx_rr8(RAX, RAX);
        return;
    }

    int t1 = type_id(from), t2 = type_id(to);

    if (t1 == F32 || t1 == F64) {
        bool src_dbl = t1 == F64;
        if (t2 == F32 || t2 == F64) {
            if (t1 == t2) return;
            if (t1 == F32) sse_rr(0xF3, 0x0F5A, 0, 0); /* cvtss2sd */
            else sse_rr(0xF2, 0x0F5A, 0, 0);          /* cvtsd2ss */
            return;
        }
        /* float to integer */
        if (t2 == U64) {
            /* values >= 2^63 do not fit the signed conversion */
            if (!src_dbl) sse_rr(0xF3, 0x0F5A, 0, 0); /* to double first */
            mov_ri(RAX, 0x43E0000000000000LL);
            movq_xmm_r(1, RAX);
            ucomi(true, 0, 1);
            int big = next_label(), done = next_label();
            jcc_label(CC_AE, big);
            cvttfp2si(true, 1, RAX, 0);
            jmp_label(done);
            label_set(big);
            sse_rr(0xF2, 0x0F5C, 0, 1); /* subsd xmm0, xmm1 */
            cvttfp2si(true, 1, RAX, 0);
            mov_ri(RCX, (i64)0x8000000000000000ULL);
            alu_rr(A_XOR, 1, RAX, RCX);
            label_set(done);
            return;
        }
        int w = (t2 == I64 || t2 == U32) ? 1 : 0;
        cvttfp2si(src_dbl, w, RAX, 0);
        switch (t2) {
        case I8:
            movsx_rr8(RAX, RAX, 0);
            break;
        case U8:
            movzx_rr8(RAX, RAX);
            break;
        case I16:
            movsx_rr16(RAX, RAX, 0);
            break;
        case U16:
            movzx_rr16(RAX, RAX);
            break;
        default:
            break;
        }
        return;
    }

    if (t2 == F32 || t2 == F64) {
        bool dbl = t2 == F64;
        switch (t1) {
        case I8:
        case I16:
        case I32:
        case U8:
        case U16:
            cvtsi2fp(dbl, 0, 0, RAX);
            return;
        case I64:
            cvtsi2fp(dbl, 1, 0, RAX);
            return;
        case U32:
            mov_rr32(RAX, RAX);
            cvtsi2fp(dbl, 1, 0, RAX);
            return;
        default: { /* U64 and pointers */
            int neg = next_label(), done = next_label();
            test_rr(1, RAX, RAX);
            jcc_label(CC_S, neg);
            cvtsi2fp(dbl, 1, 0, RAX);
            jmp_label(done);
            label_set(neg);
            mov_rr(RDI, RAX);
            alu_ri(A_AND, 0, RAX, 1);
            shift_ri(1, 5, RDI, 1);
            alu_rr(A_OR, 1, RDI, RAX);
            cvtsi2fp(dbl, 1, 0, RDI);
            sse_rr(dbl ? 0xF2 : 0xF3, 0x0F58, 0, 0); /* addsd/addss xmm0, xmm0 */
            label_set(done);
            return;
        }
        }
    }

    /* integer to integer */
    switch (t2) {
    case I8:
        movsx_rr8(RAX, RAX, 0);
        return;
    case U8:
        movzx_rr8(RAX, RAX);
        return;
    case I16:
        movsx_rr16(RAX, RAX, 0);
        return;
    case U16:
        movzx_rr16(RAX, RAX);
        return;
    case I32:
    case U32:
        return;
    default: /* 64-bit target */
        switch (t1) {
        case I8:
        case I16:
        case I32:
            movsxd_rr(RAX, RAX);
            return;
        case U32:
            mov_rr32(RAX, RAX);
            return;
        default:
            return;
        }
    }
}

/* ---- addresses */

static void tls_addr(Obj *var) {
    /* mov rax, fs:[0]; lea rax, [rax + var@tpoff] */
    e1(0x64);
    e1(0x48);
    e1(0x8B);
    e1(0x04);
    e1(0x25);
    e4(0);
    e1(0x48);
    e1(0x8D);
    e1(0x80);
    add_reloc(SEC_TEXT, (u32)cur_pos(), R_X86_64_TPOFF32, var, 0);
    e4(0);
}

static void gen_addr(Node *node) {
    switch (node->kind) {
    case ND_VAR:
        if (node->var->is_local) {
            lea(RAX, mem_rbp(node->var->offset));
            return;
        }
        if (node->var->is_tls) {
            tls_addr(node->var);
            return;
        }
        lea(RAX, mem_sym(node->var, 0));
        return;
    case ND_DEREF:
        gen_expr(node->lhs);
        return;
    case ND_COMMA:
        gen_expr(node->lhs);
        gen_addr(node->rhs);
        return;
    case ND_MEMBER:
        gen_addr(node->lhs);
        if (node->member->offset) alu_ri(A_ADD, 1, RAX, node->member->offset);
        return;
    default:
        break;
    }

    /* any struct-valued expression evaluates to the address of its value */
    if (node->ty && (node->ty->kind == TY_STRUCT || node->ty->kind == TY_UNION)) {
        gen_expr(node);
        return;
    }

    error_tok(node->tok, "not an lvalue");
}

/* ---- calls */

static void push_struct(Type *ty) {
    int sz = align_to(ty->size, 8);
    sub_rsp(sz);
    depth += sz / 8;
    copy_bytes(RAX, RSP, 0, ty->size);
}

static void push_args2(Node *args, bool first_pass) {
    if (!args) return;

    push_args2(args->next, first_pass);

    if ((first_pass && !args->pass_by_stack) || (!first_pass && args->pass_by_stack)) return;

    gen_expr(args);

    switch (args->ty->kind) {
    case TY_STRUCT:
    case TY_UNION:
        push_struct(args->ty);
        break;
    case TY_FLOAT:
    case TY_DOUBLE:
        pushf();
        break;
    default:
        push();
    }
}

/* Arguments are all evaluated first and stored on the stack; then the ones that go to registers are popped into them.
 * Returns the number of 8-byte stack slots that stay on the stack for the callee. */
static int push_args(Node *node) {
    int stack = 0, gp = 0, fp = 0;

    /* a big struct is returned through a buffer whose address is passed as the first argument */
    if (node->ret_buffer && node->ty->size > 16) gp++;

    for (Node *arg = node->args; arg; arg = arg->next) {
        Type *ty = arg->ty;
        arg->pass_by_stack = false;

        switch (ty->kind) {
        case TY_STRUCT:
        case TY_UNION:
            if (ty->size > 16) {
                arg->pass_by_stack = true;
                stack += align_to(ty->size, 8) / 8;
            } else {
                bool fp1 = has_flonum1(ty);
                bool fp2 = has_flonum2(ty);
                int need_fp = (ty->size > 8 ? fp1 + fp2 : fp1), need_gp = (ty->size > 8 ? !fp1 + !fp2 : !fp1);
                if (fp + need_fp <= FP_MAX && gp + need_gp <= GP_MAX) {
                    fp += need_fp;
                    gp += need_gp;
                } else {
                    arg->pass_by_stack = true;
                    stack += align_to(ty->size, 8) / 8;
                }
            }
            break;
        case TY_FLOAT:
        case TY_DOUBLE:
            if (fp++ >= FP_MAX) {
                arg->pass_by_stack = true;
                stack++;
            }
            break;
        default:
            if (gp++ >= GP_MAX) {
                arg->pass_by_stack = true;
                stack++;
            }
        }
    }

    if ((depth + stack) % 2 == 1) {
        sub_rsp(8);
        depth++;
        stack++;
    }

    push_args2(node->args, true);
    push_args2(node->args, false);

    if (node->ret_buffer && node->ty->size > 16) {
        lea(RAX, mem_rbp(node->ret_buffer->offset));
        push();
    }

    return stack;
}

/* Gathers n (1..8) bytes at [src + off] into a register (little endian). */
static void load_piece(int reg, int src, int off, int n) {
    if (n == 8 || n == 4 || n == 2 || n == 1) {
        load_mem(reg, mem_reg(src, off), n, true);
        return;
    }
    mov_ri(reg, 0);
    for (int i = n - 1; i >= 0; i--) {
        shift_ri(1, 4, reg, 8);
        load_mem(R8, mem_reg(src, off + i), 1, true);
        alu_rr(A_OR, 1, reg, R8);
    }
}

/* Writes n (1..8) bytes of a register to [dst + off]; the register is shifted (clobbered) for odd sizes. */
static void store_piece(int dst, int off, int reg, int n) {
    if (n == 8 || n == 4 || n == 2 || n == 1) {
        store_mem(mem_reg(dst, off), reg, n);
        return;
    }
    for (int i = 0; i < n; i++) {
        store_mem(mem_reg(dst, off + i), reg, 1);
        shift_ri(1, 5, reg, 8);
    }
}

/* the value returned in registers by a callee is copied into the buffer of the caller */
static void copy_ret_buffer(Obj *var) {
    Type *ty = var->ty;
    int gp = 0, fp = 0;

    if (has_flonum1(ty)) {
        if (ty->size == 4) movss_store(mem_rbp(var->offset), 0);
        else movsd_store(mem_rbp(var->offset), 0);
        fp++;
    } else {
        store_piece(RBP, var->offset, RAX, MIN(8, ty->size));
        gp++;
    }

    if (ty->size > 8) {
        if (has_flonum2(ty)) {
            if (ty->size == 12) movss_store(mem_rbp(var->offset + 8), fp);
            else movsd_store(mem_rbp(var->offset + 8), fp);
        } else {
            int reg = gp == 0 ? RAX : RDX;
            store_piece(RBP, var->offset + 8, reg, ty->size - 8);
        }
    }
}

/* a callee returns a small struct in rax/rdx/xmm0/xmm1: rax holds the struct's address on entry */
static void copy_struct_reg(void) {
    Type *ty = current_fn_gen->ty->return_ty;
    int gp = 0, fp = 0;

    mov_rr(RSI, RAX);

    if (has_flonum1(ty)) {
        if (ty->size == 4) movss_load(0, mem_reg(RSI, 0));
        else movsd_load(0, mem_reg(RSI, 0));
        fp++;
    } else {
        load_piece(RAX, RSI, 0, MIN(8, ty->size));
        gp++;
    }

    if (ty->size > 8) {
        if (has_flonum2(ty)) {
            if (ty->size == 12) movss_load(fp, mem_reg(RSI, 8));
            else movsd_load(fp, mem_reg(RSI, 8));
        } else {
            int reg = gp == 0 ? RAX : RDX;
            load_piece(reg, RSI, 8, ty->size - 8);
        }
    }
}

static void copy_struct_mem(void) {
    Type *ty = current_fn_gen->ty->return_ty;
    Obj *var = current_fn_gen->params; /* the hidden first parameter */

    load_mem(RDI, mem_rbp(var->offset), 8, true);
    mov_rr(RSI, RAX);
    copy_bytes(RSI, RDI, 0, ty->size);
    /* copy_bytes may have moved rdi (rep movsb); reload the destination address as the result */
    load_mem(RAX, mem_rbp(var->offset), 8, true);
}

static void gen_funcall(Node *node) {
    int stack_args = push_args(node);

    bool direct = node->lhs->kind == ND_VAR && node->lhs->var->is_function && !node->lhs->var->is_local;
    if (!direct) gen_expr(node->lhs);

    int gp = 0, fp = 0;

    if (node->ret_buffer && node->ty->size > 16) pop(argreg[gp++]);

    for (Node *arg = node->args; arg; arg = arg->next) {
        Type *ty = arg->ty;

        switch (ty->kind) {
        case TY_STRUCT:
        case TY_UNION:
            if (arg->pass_by_stack) continue;
            if (ty->size > 16) continue;
            if (has_flonum1(ty)) popf(fp++);
            else pop(argreg[gp++]);
            if (ty->size > 8) {
                if (has_flonum2(ty)) popf(fp++);
                else pop(argreg[gp++]);
            }
            break;
        case TY_FLOAT:
        case TY_DOUBLE:
            if (!arg->pass_by_stack) popf(fp++);
            break;
        default:
            if (!arg->pass_by_stack) pop(argreg[gp++]);
        }
    }

    if (direct) {
        mov_ri(RAX, fp);
        call_sym(node->lhs->var);
    } else {
        mov_rr(R10, RAX);
        mov_ri(RAX, fp);
        call_r(R10);
    }

    add_rsp(stack_args * 8);
    depth -= stack_args;

    /* the upper bits of rax are undefined for returns shorter than an int: clean them up */
    switch (node->ty->kind) {
    case TY_BOOL:
        movzx_rr8(RAX, RAX);
        return;
    case TY_CHAR:
        if (node->ty->is_unsigned) movzx_rr8(RAX, RAX);
        else movsx_rr8(RAX, RAX, 0);
        return;
    case TY_SHORT:
        if (node->ty->is_unsigned) movzx_rr16(RAX, RAX);
        else movsx_rr16(RAX, RAX, 0);
        return;
    default:
        break;
    }

    if (node->ret_buffer && node->ty->size <= 16) {
        copy_ret_buffer(node->ret_buffer);
        lea(RAX, mem_rbp(node->ret_buffer->offset));
    }
}

/* ---- variadic functions */

static void gen_va_start(Node *node) {
    gen_expr(node->lhs); /* rax = &ap */
    Obj *fn = current_fn_gen;
    int gp = 0, fp = 0;
    for (Obj *var = fn->params; var; var = var->next) {
        Type *ty = var->ty;
        if (var->offset > 0) continue; /* passed on the stack */
        if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
            if (has_flonum1(ty)) fp++;
            else gp++;
            if (ty->size > 8) {
                if (has_flonum2(ty)) fp++;
                else gp++;
            }
        } else if (is_flonum(ty)) {
            fp++;
        } else {
            gp++;
        }
    }
    mov_mi32(mem_reg(RAX, 0), 4, gp * 8);
    mov_mi32(mem_reg(RAX, 4), 4, 48 + fp * 16);
    lea(RCX, mem_rbp(fn->va_stack_top));
    store_mem(mem_reg(RAX, 8), RCX, 8);
    lea(RCX, mem_rbp(fn->va_area->offset));
    store_mem(mem_reg(RAX, 16), RCX, 8);
}

static void gen_va_arg(Node *node) {
    Type *ty = node->ty;
    gen_expr(node->lhs); /* rax = &ap (the va_list array decays) */
    mov_rr(RCX, RAX);

    int overflow = next_label(), done = next_label();

    if (is_flonum(ty)) {
        load_mem(RDX, mem_reg(RCX, 4), 4, true);
        alu_ri(A_CMP, 0, RDX, 160);
        jcc_label(CC_A, overflow);
        load_mem(RAX, mem_reg(RCX, 16), 8, true);
        alu_rr(A_ADD, 1, RAX, RDX);
        alu_ri(A_ADD, 0, RDX, 16);
        store_mem(mem_reg(RCX, 4), RDX, 4);
        jmp_label(done);
        label_set(overflow);
        load_mem(RAX, mem_reg(RCX, 8), 8, true);
        lea(RDX, mem_reg(RAX, 8));
        store_mem(mem_reg(RCX, 8), RDX, 8);
        label_set(done);
        if (ty->kind == TY_FLOAT) {
            /* a float argument was promoted to double by the caller */
            movsd_load(0, mem_reg(RAX, 0));
            sse_rr(0xF2, 0x0F5A, 0, 0);
        } else {
            load(ty);
        }
        return;
    }

    if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
        int sz8 = align_to(ty->size, 8);
        bool in_regs = ty->size <= 16 && !has_flonum1(ty) ? true : false;
        if (ty->size <= 16 && (has_flonum1(ty) || (ty->size > 8 && has_flonum2(ty))))
            error_tok(node->tok, "va_arg of a struct passed in SSE registers is not supported");
        if (in_regs) {
            int n = ty->size > 8 ? 2 : 1;
            load_mem(RDX, mem_reg(RCX, 0), 4, true);
            alu_ri(A_CMP, 0, RDX, 48 - 8 * n);
            jcc_label(CC_A, overflow);
            load_mem(RAX, mem_reg(RCX, 16), 8, true);
            alu_rr(A_ADD, 1, RAX, RDX);
            alu_ri(A_ADD, 0, RDX, 8 * n);
            store_mem(mem_reg(RCX, 0), RDX, 4);
            jmp_label(done);
        }
        label_set(overflow);
        load_mem(RAX, mem_reg(RCX, 8), 8, true);
        lea(RDX, mem_reg(RAX, sz8));
        store_mem(mem_reg(RCX, 8), RDX, 8);
        label_set(done);
        return; /* the value of a struct is its address */
    }

    load_mem(RDX, mem_reg(RCX, 0), 4, true);
    alu_ri(A_CMP, 0, RDX, 40);
    jcc_label(CC_A, overflow);
    load_mem(RAX, mem_reg(RCX, 16), 8, true);
    alu_rr(A_ADD, 1, RAX, RDX);
    alu_ri(A_ADD, 0, RDX, 8);
    store_mem(mem_reg(RCX, 0), RDX, 4);
    jmp_label(done);
    label_set(overflow);
    load_mem(RAX, mem_reg(RCX, 8), 8, true);
    lea(RDX, mem_reg(RAX, 8));
    store_mem(mem_reg(RCX, 8), RDX, 8);
    label_set(done);
    load(ty);
}

/* ---- builtins */

static void gen_builtin(Node *node) {
    switch (node->op) {
    case BI_UNREACHABLE:
    case BI_TRAP:
        ud2();
        return;
    case BI_BSWAP16:
        gen_expr(node->lhs);
        e1(0x66);
        e1(0xC1);
        e1(0xC0);
        e1(8); /* rol ax, 8 */
        movzx_rr16(RAX, RAX);
        return;
    case BI_BSWAP32:
        gen_expr(node->lhs);
        e1(0x0F);
        e1(0xC8);
        return;
    case BI_BSWAP64:
        gen_expr(node->lhs);
        e1(0x48);
        e1(0x0F);
        e1(0xC8);
        return;
    case BI_CLZ:
    case BI_CLZ64: {
        bool w = node->op == BI_CLZ64;
        gen_expr(node->lhs);
        i_rr(0, w, 0x0FBD, RAX, RAX, 0); /* bsr */
        alu_ri(A_XOR, 0, RAX, w ? 63 : 31);
        return;
    }
    case BI_CTZ:
    case BI_CTZ64:
        gen_expr(node->lhs);
        i_rr(0, node->op == BI_CTZ64, 0x0FBC, RAX, RAX, 0); /* bsf */
        return;
    case BI_POPCOUNT:
    case BI_POPCOUNT64:
        gen_expr(node->lhs);
        i_rr(0xF3, node->op == BI_POPCOUNT64, 0x0FB8, RAX, RAX, 0); /* popcnt */
        return;
    case BI_FFS:
    case BI_FFS64: {
        bool w = node->op == BI_FFS64;
        int zero = next_label(), done = next_label();
        gen_expr(node->lhs);
        test_rr(w, RAX, RAX);
        jcc_label(CC_E, zero);
        i_rr(0, w, 0x0FBC, RAX, RAX, 0);
        alu_ri(A_ADD, 0, RAX, 1);
        jmp_label(done);
        label_set(zero);
        mov_ri(RAX, 0);
        label_set(done);
        return;
    }
    case BI_FRAME_ADDRESS:
        mov_rr(RAX, RBP);
        return;
    case BI_RETURN_ADDRESS:
        load_mem(RAX, mem_rbp(8), 8, true);
        return;
    case BI_FABS:
        gen_expr(node->lhs);
        if (node->ty->kind == TY_FLOAT) {
            mov_ri(RAX, 0x7FFFFFFF);
            movd_xmm_r(1, RAX);
            sse_rr(0, 0x0F54, 0, 1); /* andps */
        } else {
            mov_ri(RAX, 0x7FFFFFFFFFFFFFFFLL);
            movq_xmm_r(1, RAX);
            sse_rr(0x66, 0x0F54, 0, 1); /* andpd */
        }
        return;
    case BI_SQRT:
        gen_expr(node->lhs);
        sse_rr(node->ty->kind == TY_FLOAT ? 0xF3 : 0xF2, 0x0F51, 0, 0);
        return;
    default:
        error_tok(node->tok, "builtin not supported");
    }
}

/* ---- atomics (x86 is strongly ordered; lock-prefixed instructions give the read-modify-write operations) */

static void extend_rax(Type *ty) {
    switch (ty->size) {
    case 1:
        if (ty->is_unsigned) movzx_rr8(RAX, RAX);
        else movsx_rr8(RAX, RAX, 0);
        break;
    case 2:
        if (ty->is_unsigned) movzx_rr16(RAX, RAX);
        else movsx_rr16(RAX, RAX, 0);
        break;
    case 4:
        if (ty->is_unsigned || ty->kind == TY_PTR) mov_rr32(RAX, RAX);
        else movsxd_rr(RAX, RAX);
        break;
    default:
        break;
    }
}

/* [lock] op [mem], reg  for sizes 1, 2, 4 and 8; opw is the opcode for the wider forms, opb the byte form */
static void locked_rm(bool lock, int opw, int opb, int size, Mem m, int reg) {
    if (lock) e1(0xF0);
    i_rm(size == 2 ? 0x66 : 0, size == 8, size == 1 ? opb : opw, reg, m, 0, size == 1 && reg >= 4 && reg < 8);
}

static void gen_atomic(Node *node) {
    int size = (int)node->val;
    int regs[6] = {RDI, RSI, RDX, RCX, R8, R9};
    int n = 0;

    if (node->op == AT_FENCE) {
        e1(0x0F);
        e1(0xAE);
        e1(0xF0); /* mfence */
        return;
    }

    for (Node *a = node->args; a; a = a->next) {
        gen_expr(a);
        push();
        n++;
    }
    for (int i = n - 1; i >= 0; i--) pop(regs[i]);

    Mem m = mem_reg(RDI, 0);
    switch (node->op) {
    case AT_LOAD:
        load_mem(RAX, m, size, node->ty->is_unsigned || node->ty->kind == TY_PTR);
        return;
    case AT_STORE:
        mov_rr(RAX, RSI);
        locked_rm(false, 0x87, 0x86, size, m, RAX);
        return;
    case AT_LOCK_RELEASE:
        mov_mi32(m, size, 0);
        return;
    case AT_CLEAR:
        mov_mi32(m, 1, 0);
        return;
    case AT_XCHG:
        mov_rr(RAX, RSI);
        locked_rm(false, 0x87, 0x86, size, m, RAX);
        extend_rax(node->ty);
        return;
    case AT_TEST_AND_SET:
        mov_ri(RAX, 1);
        locked_rm(false, 0x87, 0x86, 1, m, RAX);
        movzx_rr8(RAX, RAX);
        alu_ri(A_CMP, 0, RAX, 0);
        setcc(CC_NE, RAX);
        movzx_rr8(RAX, RAX);
        return;
    case AT_FETCH_ADD:
    case AT_ADD_FETCH:
        mov_rr(RAX, RSI);
        locked_rm(true, 0x0FC1, 0x0FC0, size, m, RAX); /* lock xadd: rax = old value */
        if (node->op == AT_ADD_FETCH) alu_rr(A_ADD, 1, RAX, RSI);
        extend_rax(node->ty);
        return;
    case AT_FETCH_SUB:
    case AT_SUB_FETCH:
        mov_rr(RAX, RSI);
        unary_r(1, 3, RAX); /* neg */
        locked_rm(true, 0x0FC1, 0x0FC0, size, m, RAX);
        if (node->op == AT_SUB_FETCH) alu_rr(A_SUB, 1, RAX, RSI);
        extend_rax(node->ty);
        return;
    case AT_FETCH_AND:
    case AT_FETCH_OR:
    case AT_FETCH_XOR:
    case AT_FETCH_NAND:
    case AT_AND_FETCH:
    case AT_OR_FETCH:
    case AT_XOR_FETCH:
    case AT_NAND_FETCH: {
        int kind = node->op >= AT_ADD_FETCH ? node->op - AT_ADD_FETCH : node->op - AT_FETCH_ADD; /* 0 add, 1 sub, 2 and, 3 or, 4 xor, 5 nand */
        bool new_value = node->op >= AT_ADD_FETCH;
        int loop = next_label();
        load_mem(RAX, m, size, true);
        label_set(loop);
        mov_rr(RDX, RAX);
        if (kind == 2 || kind == 5) alu_rr(A_AND, 1, RDX, RSI);
        else if (kind == 3) alu_rr(A_OR, 1, RDX, RSI);
        else alu_rr(A_XOR, 1, RDX, RSI);
        if (kind == 5) unary_r(1, 2, RDX);
        locked_rm(true, 0x0FB1, 0x0FB0, size, m, RDX); /* lock cmpxchg [rdi], rdx */
        jcc_label(CC_NE, loop);
        if (new_value) mov_rr(RAX, RDX);
        extend_rax(node->ty);
        return;
    }
    case AT_CAS: {
        /* rdi = ptr, rsi = &expected, rdx = desired */
        int done = next_label();
        load_mem(RAX, mem_reg(RSI, 0), size, true);
        locked_rm(true, 0x0FB1, 0x0FB0, size, m, RDX);
        setcc(CC_E, RCX);
        jcc_label(CC_E, done);
        store_mem(mem_reg(RSI, 0), RAX, size);
        label_set(done);
        movzx_rr8(RAX, RCX);
        return;
    }
    case AT_SYNC_VAL_CAS:
    case AT_SYNC_BOOL_CAS:
        /* rdi = ptr, rsi = old, rdx = new */
        mov_rr(RAX, RSI);
        locked_rm(true, 0x0FB1, 0x0FB0, size, m, RDX);
        if (node->op == AT_SYNC_BOOL_CAS) {
            setcc(CC_E, RAX);
            movzx_rr8(RAX, RAX);
        } else {
            extend_rax(node->ty);
        }
        return;
    default:
        error_tok(node->tok, "atomic builtin not supported");
    }
}

/* ---- expressions */

/* an operand that can be put into rdi without touching rax */
static bool is_simple_operand(Node *n) {
    if (n->kind == ND_NUM && is_integer(n->ty) ) return true;
    if (n->kind == ND_NUM && n->ty->base == NULL && !is_flonum(n->ty)) return true;
    if (n->kind == ND_VAR && n->var->is_local && !is_flonum(n->ty) && n->ty->kind != TY_STRUCT && n->ty->kind != TY_UNION &&
        n->ty->kind != TY_ARRAY && n->ty->kind != TY_FUNC)
        return true;
    return false;
}

static void load_simple_to_rdi(Node *n) {
    if (n->kind == ND_NUM) {
        mov_ri(RDI, n->val);
        return;
    }
    load_mem(RDI, mem_rbp(n->var->offset), n->ty->size, n->ty->is_unsigned);
}

/* lhs -> rax, rhs -> rdi */
static void gen_int_operands(Node *node) {
    if (is_simple_operand(node->rhs)) {
        gen_expr(node->lhs);
        load_simple_to_rdi(node->rhs);
        return;
    }
    gen_expr(node->rhs);
    push();
    gen_expr(node->lhs);
    pop(RDI);
}

static bool is_64bit(Type *ty) {
    return ty->size == 8 || ty->base != NULL || ty->kind == TY_PTR;
}

static int cc_for(Node *node) {
    bool uns = node->lhs->ty->is_unsigned;
    switch (node->kind) {
    case ND_EQ:
        return CC_E;
    case ND_NE:
        return CC_NE;
    case ND_LT:
        return uns ? CC_B : CC_L;
    default:
        return uns ? CC_BE : CC_LE;
    }
}

static void gen_branch(Node *n, bool when_true, int label) {
    switch (n->kind) {
    case ND_NOT:
        gen_branch(n->lhs, !when_true, label);
        return;
    case ND_LOGAND:
        if (!when_true) {
            gen_branch(n->lhs, false, label);
            gen_branch(n->rhs, false, label);
        } else {
            int skipl = next_label();
            gen_branch(n->lhs, false, skipl);
            gen_branch(n->rhs, true, label);
            label_set(skipl);
        }
        return;
    case ND_LOGOR:
        if (when_true) {
            gen_branch(n->lhs, true, label);
            gen_branch(n->rhs, true, label);
        } else {
            int skipl = next_label();
            gen_branch(n->lhs, true, skipl);
            gen_branch(n->rhs, false, label);
            label_set(skipl);
        }
        return;
    case ND_EQ:
    case ND_NE:
    case ND_LT:
    case ND_LE:
        if (!is_flonum(n->lhs->ty)) {
            gen_int_operands(n);
            alu_rr(A_CMP, is_64bit(n->lhs->ty), RAX, RDI);
            int cc = cc_for(n);
            jcc_label(when_true ? cc : (cc ^ 1), label);
            return;
        }
        break;
    case ND_NUM:
        if (!is_flonum(n->ty)) {
            if ((n->val != 0) == when_true) jmp_label(label);
            return;
        }
        break;
    default:
        break;
    }
    gen_expr(n);
    cmp_zero(n->ty);
    jcc_label(when_true ? CC_NE : CC_E, label);
}

static void and_imm(int reg, u64 mask, int tmp) {
    if (mask <= 0x7FFFFFFFULL) {
        alu_ri(A_AND, 1, reg, (i64)mask);
    } else {
        mov_ri(tmp, (i64)mask);
        alu_rr(A_AND, 1, reg, tmp);
    }
}

static void gen_assign_bitfield(Node *node) {
    Member *mem = node->lhs->member;
    u64 wmask = mem->bit_width >= 64 ? ~0ULL : ((1ULL << mem->bit_width) - 1);
    gen_addr(node->lhs->lhs);
    if (mem->offset) alu_ri(A_ADD, 1, RAX, mem->offset);
    push();
    gen_expr(node->rhs);
    mov_rr(R8, RAX);          /* r8 = the new value */
    mov_rr(RDI, RAX);
    and_imm(RDI, wmask, R11); /* rdi = the value masked and shifted into place */
    if (mem->bit_offset) shift_ri(1, 4, RDI, mem->bit_offset);
    load_mem(RAX, mem_reg(RSP, 0), 8, true); /* rax = address */
    load(mem->ty);                           /* rax = old storage unit */
    mov_ri(R9, (i64)~(wmask << mem->bit_offset));
    alu_rr(A_AND, 1, RAX, R9);
    alu_rr(A_OR, 1, RAX, RDI);
    store(node->ty); /* pops the address */
    mov_rr(RAX, R8);
    shift_ri(1, 4, RAX, 64 - mem->bit_width);
    shift_ri(1, mem->ty->is_unsigned ? 5 : 7, RAX, 64 - mem->bit_width);
}

static void gen_expr(Node *node) {
    switch (node->kind) {
    case ND_NULL_EXPR:
        return;
    case ND_NUM:
        if (node->ty->kind == TY_FLOAT) {
            float f = (float)node->fval;
            u32 bits;
            memcpy(&bits, &f, 4);
            mov_ri(RAX, bits);
            movd_xmm_r(0, RAX);
        } else if (node->ty->kind == TY_DOUBLE) {
            u64 bits;
            double d = node->fval;
            memcpy(&bits, &d, 8);
            mov_ri(RAX, (i64)bits);
            movq_xmm_r(0, RAX);
        } else {
            mov_ri(RAX, node->val);
        }
        return;
    case ND_NEG:
        gen_expr(node->lhs);
        if (node->ty->kind == TY_FLOAT) {
            mov_ri(RAX, 0x80000000LL);
            movd_xmm_r(1, RAX);
            xorp(false, 0, 1);
        } else if (node->ty->kind == TY_DOUBLE) {
            mov_ri(RAX, (i64)0x8000000000000000ULL);
            movq_xmm_r(1, RAX);
            xorp(true, 0, 1);
        } else {
            unary_r(is_64bit(node->ty), 3, RAX);
        }
        return;
    case ND_VAR: {
        Mem m;
        if (simple_addr(node, &m)) {
            load_typed(m, node->ty);
            return;
        }
        gen_addr(node);
        load(node->ty);
        return;
    }
    case ND_MEMBER: {
        Mem m;
        if (simple_addr(node, &m)) {
            load_typed(m, node->ty);
            return;
        }
        gen_addr(node);
        load(node->ty);
        Member *mem = node->member;
        if (mem->is_bitfield) {
            shift_ri(1, 4, RAX, 64 - mem->bit_width - mem->bit_offset);
            shift_ri(1, mem->ty->is_unsigned ? 5 : 7, RAX, 64 - mem->bit_width);
        }
        return;
    }
    case ND_DEREF:
        gen_expr(node->lhs);
        load(node->ty);
        return;
    case ND_ADDR:
        gen_addr(node->lhs);
        return;
    case ND_ASSIGN: {
        if (node->lhs->kind == ND_MEMBER && node->lhs->member->is_bitfield) {
            gen_assign_bitfield(node);
            return;
        }
        Mem m;
        Type *lty = node->lhs->ty;
        if (lty->kind != TY_STRUCT && lty->kind != TY_UNION && simple_addr(node->lhs, &m)) {
            gen_expr(node->rhs);
            store_typed(m, lty);
            return;
        }
        gen_addr(node->lhs);
        push();
        gen_expr(node->rhs);
        store(lty);
        return;
    }
    case ND_STMT_EXPR:
        for (Node *n = node->body; n; n = n->next) gen_stmt(n);
        return;
    case ND_COMMA:
        gen_expr(node->lhs);
        gen_expr(node->rhs);
        return;
    case ND_CAST:
        gen_expr(node->lhs);
        gen_cast(node->lhs->ty, node->ty);
        return;
    case ND_MEMZERO:
        mov_ri(RCX, node->var->ty->size);
        lea(RDI, mem_rbp(node->var->offset));
        mov_ri(RAX, 0);
        rep_stosb();
        return;
    case ND_COND: {
        int els = next_label(), end = next_label();
        gen_branch(node->cond, false, els);
        gen_expr(node->then);
        jmp_label(end);
        label_set(els);
        gen_expr(node->els);
        label_set(end);
        return;
    }
    case ND_NOT:
        gen_expr(node->lhs);
        cmp_zero(node->lhs->ty);
        setcc(CC_E, RAX);
        movzx_rr8(RAX, RAX);
        return;
    case ND_BITNOT:
        gen_expr(node->lhs);
        unary_r(is_64bit(node->ty), 2, RAX);
        return;
    case ND_LOGAND:
    case ND_LOGOR: {
        int fl = next_label(), end = next_label();
        bool is_and = node->kind == ND_LOGAND;
        /* and: jump to "false" when a side is zero; or: jump to "true" when a side is nonzero */
        gen_branch(node->lhs, !is_and, fl);
        gen_branch(node->rhs, !is_and, fl);
        mov_ri(RAX, is_and ? 1 : 0);
        jmp_label(end);
        label_set(fl);
        mov_ri(RAX, is_and ? 0 : 1);
        label_set(end);
        return;
    }
    case ND_FUNCALL:
        gen_funcall(node);
        return;
    case ND_VA_START:
        gen_va_start(node);
        return;
    case ND_VA_ARG:
        gen_va_arg(node);
        return;
    case ND_BUILTIN:
        gen_builtin(node);
        return;
    case ND_ATOMIC:
        gen_atomic(node);
        return;
    case ND_THREAD_PTR:
        e1(0x64);
        e1(0x48);
        e1(0x8B);
        e1(0x04);
        e1(0x25);
        e4(0);
        return;
    case ND_ALLOCA:
        error_tok(node->tok, "alloca is not supported by mcc");
        return;
    default:
        break;
    }

    if (!node->lhs || !node->rhs || !node->lhs->ty) error_tok(node->tok, "invalid expression");

    if (is_flonum(node->lhs->ty)) {
        bool dbl = node->lhs->ty->kind == TY_DOUBLE;
        int pfx = dbl ? 0xF2 : 0xF3;

        gen_expr(node->rhs);
        pushf();
        gen_expr(node->lhs);
        popf(1);

        switch (node->kind) {
        case ND_ADD:
            sse_rr(pfx, 0x0F58, 0, 1);
            return;
        case ND_SUB:
            sse_rr(pfx, 0x0F5C, 0, 1);
            return;
        case ND_MUL:
            sse_rr(pfx, 0x0F59, 0, 1);
            return;
        case ND_DIV:
            sse_rr(pfx, 0x0F5E, 0, 1);
            return;
        case ND_EQ:
        case ND_NE:
        case ND_LT:
        case ND_LE:
            ucomi(dbl, 1, 0);
            if (node->kind == ND_EQ) {
                setcc(CC_E, RAX);
                setcc(CC_NP, RDX);
                i_rr(0, 0, 0x20, RDX, RAX, 0); /* and al, dl */
            } else if (node->kind == ND_NE) {
                setcc(CC_NE, RAX);
                setcc(CC_P, RDX);
                i_rr(0, 0, 0x08, RDX, RAX, 0); /* or al, dl */
            } else if (node->kind == ND_LT) {
                setcc(CC_A, RAX);
            } else {
                setcc(CC_AE, RAX);
            }
            movzx_rr8(RAX, RAX);
            return;
        default:
            break;
        }
        error_tok(node->tok, "invalid expression");
    }

    gen_int_operands(node);

    bool w = is_64bit(node->lhs->ty);

    switch (node->kind) {
    case ND_ADD:
        alu_rr(A_ADD, w, RAX, RDI);
        return;
    case ND_SUB:
        alu_rr(A_SUB, w, RAX, RDI);
        return;
    case ND_MUL:
        imul_rr(w, RAX, RDI);
        return;
    case ND_DIV:
    case ND_MOD:
        if (node->ty->is_unsigned) {
            mov_ri(RDX, 0);
            unary_r(w, 6, RDI);
        } else {
            if (w) cqo();
            else cdq();
            unary_r(w, 7, RDI);
        }
        if (node->kind == ND_MOD) mov_rr(RAX, RDX);
        return;
    case ND_BITAND:
        alu_rr(A_AND, w, RAX, RDI);
        return;
    case ND_BITOR:
        alu_rr(A_OR, w, RAX, RDI);
        return;
    case ND_BITXOR:
        alu_rr(A_XOR, w, RAX, RDI);
        return;
    case ND_EQ:
    case ND_NE:
    case ND_LT:
    case ND_LE:
        alu_rr(A_CMP, w, RAX, RDI);
        setcc(cc_for(node), RAX);
        movzx_rr8(RAX, RAX);
        return;
    case ND_SHL:
        mov_rr(RCX, RDI);
        shift_cl(w, 4, RAX);
        return;
    case ND_SHR:
        mov_rr(RCX, RDI);
        shift_cl(w, node->lhs->ty->is_unsigned ? 5 : 7, RAX);
        return;
    default:
        break;
    }

    error_tok(node->tok, "invalid expression");
}

/* ---- statements */

static void gen_stmt(Node *node) {
    switch (node->kind) {
    case ND_IF: {
        int els = next_label(), end = next_label();
        gen_branch(node->cond, false, els);
        gen_stmt(node->then);
        if (node->els) {
            jmp_label(end);
            label_set(els);
            gen_stmt(node->els);
            label_set(end);
        } else {
            label_set(els);
        }
        return;
    }
    case ND_FOR: {
        int begin = next_label();
        if (node->init) gen_stmt(node->init);
        label_set(begin);
        if (node->cond) gen_branch(node->cond, false, node->brk_label);
        gen_stmt(node->then);
        label_set(node->cont_label);
        if (node->inc) gen_expr(node->inc);
        jmp_label(begin);
        label_set(node->brk_label);
        return;
    }
    case ND_DO: {
        int begin = next_label();
        label_set(begin);
        gen_stmt(node->then);
        label_set(node->cont_label);
        gen_branch(node->cond, true, begin);
        label_set(node->brk_label);
        return;
    }
    case ND_SWITCH: {
        gen_expr(node->cond);
        bool w = node->cond->ty->size == 8;
        for (Node *n = node->case_next; n; n = n->case_next) {
            if (n->val == n->val2) {
                if (w && (n->val < -2147483648LL || n->val > 2147483647LL)) {
                    mov_ri(RDI, n->val);
                    alu_rr(A_CMP, 1, RAX, RDI);
                } else {
                    alu_ri(A_CMP, w, RAX, (i32)n->val);
                }
                jcc_label(CC_E, n->label_id);
                continue;
            }
            /* [GNU] case ranges: (x - begin) <= (end - begin) as unsigned numbers */
            mov_rr(RDI, RAX);
            if (w) {
                mov_ri(RDX, n->val);
                alu_rr(A_SUB, 1, RDI, RDX);
                mov_ri(RDX, n->val2 - n->val);
                alu_rr(A_CMP, 1, RDI, RDX);
            } else {
                alu_ri(A_SUB, 0, RDI, (i32)n->val);
                alu_ri(A_CMP, 0, RDI, (i32)(n->val2 - n->val));
            }
            jcc_label(CC_BE, n->label_id);
        }
        if (node->default_case) jmp_label(node->default_case->label_id);
        else jmp_label(node->brk_label);
        gen_stmt(node->then);
        label_set(node->brk_label);
        return;
    }
    case ND_CASE:
        label_set(node->label_id);
        gen_stmt(node->lhs);
        return;
    case ND_BLOCK:
        for (Node *n = node->body; n; n = n->next) gen_stmt(n);
        return;
    case ND_GOTO:
        jmp_label(node->label_id);
        return;
    case ND_LABEL:
        label_set(node->label_id);
        gen_stmt(node->lhs);
        return;
    case ND_RETURN:
        if (node->lhs) {
            gen_expr(node->lhs);
            Type *ty = node->lhs->ty;
            if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
                if (ty->size <= 16) copy_struct_reg();
                else copy_struct_mem();
            }
        }
        jmp_label(ret_label);
        return;
    case ND_EXPR_STMT:
        gen_expr(node->lhs);
        return;
    case ND_ASM:
        gen_asm(node);
        return;
    default:
        break;
    }

    error_tok(node->tok, "invalid statement");
}

/* ---- functions */

static void store_gp(int r, int offset, int sz) {
    switch (sz) {
    case 1:
    case 2:
    case 4:
    case 8:
        store_mem(mem_rbp(offset), argreg[r], sz);
        return;
    default:
        /* odd sizes (pieces of a struct): byte by byte */
        mov_rr(R8, argreg[r]);
        for (int i = 0; i < sz; i++) {
            store_mem(mem_rbp(offset + i), R8, 1);
            shift_ri(1, 5, R8, 8);
        }
    }
}

static void store_fp(int r, int offset, int sz) {
    if (sz == 4) movss_store(mem_rbp(offset), r);
    else movsd_store(mem_rbp(offset), r);
}

static void gen_function(Obj *fn) {
    current_fn_gen = fn;
    OutSec *text = &osec[SEC_TEXT];
    while (text->data.len % 16) buf_u8(&text->data, 0xCC);
    if (text->align < 16) text->align = 16;
    fn->sec = SEC_TEXT;
    fn->sec_off = cur_pos();

    assign_lvar_offsets(fn);
    depth = 0;
    nfixups = 0;
    ret_label = next_label();

    push_r(RBP);
    mov_rr(RBP, RSP);
    sub_rsp(fn->stack_size);

    /* variadic functions keep their register arguments in a save area (the va_list points into it) */
    if (fn->va_area) {
        int off = fn->va_area->offset;
        for (int i = 0; i < GP_MAX; i++) store_mem(mem_rbp(off + 8 * i), argreg[i], 8);
        for (int i = 0; i < FP_MAX; i++) sse_rm(0, 0x0F11, i, mem_rbp(off + 48 + 16 * i)); /* movups */
    }

    /* the register arguments of the function go to their slots in the frame */
    int gp = 0, fp = 0;
    for (Obj *var = fn->params; var; var = var->next) {
        if (var->offset > 0) continue;

        Type *ty = var->ty;

        switch (ty->kind) {
        case TY_STRUCT:
        case TY_UNION:
            if (has_flonum1(ty)) store_fp(fp++, var->offset, MIN(8, ty->size));
            else store_gp(gp++, var->offset, MIN(8, ty->size));

            if (ty->size > 8) {
                if (has_flonum2(ty)) store_fp(fp++, var->offset + 8, ty->size - 8);
                else store_gp(gp++, var->offset + 8, ty->size - 8);
            }
            break;
        case TY_FLOAT:
        case TY_DOUBLE:
            store_fp(fp++, var->offset, ty->size);
            break;
        default:
            store_gp(gp++, var->offset, ty->size);
        }
    }

    gen_stmt(fn->body);
    if (depth != 0) fatal("internal error: unbalanced stack in %s (depth %d)", fn->name, depth);

    /* main returns 0 when control reaches its closing brace */
    if (!strcmp(fn->name, "main")) mov_ri(RAX, 0);

    label_set(ret_label);
    leave_();
    ret_();
    resolve_fixups();
    fn->size_out = cur_pos() - fn->sec_off;
    fn->emitted = true;
}
