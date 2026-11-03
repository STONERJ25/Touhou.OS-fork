/*
 * The bytecode instruction set: OP(name, size in bytes with operands, values popped, values pushed).
 * A pop count of -1 means "depends on the operand" (calls, array literals).  Operands follow the opcode byte,
 * little-endian: u8, u16 (slots, argument counts), u32 (constant-pool and inline-cache indices), i32 (jump offsets,
 * relative to the start of the instruction).  Stack pictures in comments list the top last.
 */
/* constants */
OP(nop, 1, 0, 0)
OP(push_undefined, 1, 0, 1)
OP(push_null, 1, 0, 1)
OP(push_true, 1, 0, 1)
OP(push_false, 1, 0, 1)
OP(push_i8, 2, 0, 1)
OP(push_i32, 5, 0, 1)
OP(push_const, 5, 0, 1)             /* u32 constant (number, string, bigint) */
OP(push_hole, 1, 0, 1)
OP(push_this, 1, 0, 1)              /* the frame's this (coerced at entry for sloppy functions) */
OP(push_new_target, 1, 0, 1)
OP(push_func, 1, 0, 1)              /* the running function object */
OP(push_home, 1, 0, 1)              /* the running function's [[HomeObject]] */
/* stack shuffles */
OP(dup, 1, 1, 2)
OP(dup2, 1, 2, 4)                   /* a b -> a b a b */
OP(dup_under, 1, 2, 3)              /* a b -> a a b */
OP(drop, 1, 1, 0)
OP(nip, 1, 2, 1)                    /* a b -> b */
OP(swap, 1, 2, 2)
OP(rot3l, 1, 3, 3)                  /* a b c -> b c a */
OP(rot3r, 1, 3, 3)                  /* a b c -> c a b */
OP(insert2, 1, 2, 3)                /* a b -> b a b */
OP(insert3, 1, 3, 4)                /* a b c -> c a b c */
OP(insert4, 1, 4, 5)                /* a b c d -> d a b c d */
OP(swap2, 1, 4, 4)                  /* a b c d -> c d a b */
OP(rot4l, 1, 4, 4)                  /* a b c d -> b c d a */
/* locals (u16 slot) */
OP(get_loc, 3, 0, 1)
OP(put_loc, 3, 1, 0)
OP(set_loc, 3, 1, 1)
OP(get_loc_check, 3, 0, 1)          /* ReferenceError if uninitialised */
OP(put_loc_check, 3, 1, 0)
OP(set_loc_uninit, 3, 0, 0)
OP(box_loc, 3, 0, 0)                /* the slot's value moves into a new var cell kept in the slot */
OP(get_lcell, 3, 0, 1)              /* the slot holds a var cell */
OP(put_lcell, 3, 1, 0)
OP(set_lcell, 3, 1, 1)
OP(get_lcell_check, 3, 0, 1)
OP(put_lcell_check, 3, 1, 0)
OP(init_lcell, 3, 1, 0)             /* store without the TDZ check (the declaration itself) */
OP(new_lcell, 3, 0, 0)              /* a fresh cell holding UNINIT */
OP(new_lcell_undef, 3, 0, 0)        /* a fresh cell holding undefined */
OP(copy_lcell, 3, 0, 0)             /* a fresh cell holding the old cell's value (per-iteration let) */
OP(get_cvar, 3, 0, 1)               /* the function's captured cell u16 */
OP(put_cvar, 3, 1, 0)
OP(set_cvar, 3, 1, 1)
OP(get_cvar_check, 3, 0, 1)
OP(put_cvar_check, 3, 1, 0)
OP(init_cvar, 3, 1, 0)
OP(get_cvar_raw, 3, 0, 1)           /* the cell itself (eval scopes) */
OP(throw_const, 5, 0, 0)            /* u32 name: TypeError "Assignment to constant variable." */
OP(throw_ref, 5, 0, 0)              /* u32 message constant: ReferenceError */
OP(throw_syntax, 5, 0, 0)           /* u32 message constant: SyntaxError (late errors) */
OP(throw_type, 5, 0, 0)             /* u32 message constant: TypeError */
/* globals (u32 name, u32 inline cache) */
OP(get_global, 9, 0, 1)
OP(get_global_typeof, 9, 0, 1)
OP(put_global, 9, 1, 0)
OP(set_global, 9, 1, 1)
OP(put_global_init, 9, 1, 0)        /* initialise a global let/const/class */
OP(delete_global, 5, 0, 1)
OP(global_decls, 5, 0, 0)           /* u32 constant: the script's declaration list (GlobalDeclarationInstantiation) */
/* dynamic scopes: the with object (or a direct eval's var object) is on the stack; u32 name, i32 offset, u8 kind */
OP(with_get, 10, 1, 0)              /* obj -> (found: value, jump) */
OP(with_put, 10, 2, 1)              /* value obj -> value (found: stored, jump) */
OP(with_call, 10, 1, 0)             /* obj -> (found: func this, jump) */
OP(with_typeof, 10, 1, 0)
OP(with_delete, 10, 1, 0)           /* obj -> (found: result, jump) */
OP(with_obj, 1, 1, 1)               /* value -> ToObject(value) (TypeError for null and undefined) */
OP(eval_var_obj, 3, 0, 1)           /* u16 slot: the function's eval var object (created on demand), as a value */
OP(eval_decl, 5, 2, 0)              /* u32 name: cell value -> a var (value: hole) or function of sloppy eval code in
                                       the caller's var object, made on demand in the cell */
/* properties */
OP(get_field, 9, 1, 1)              /* u32 name, u32 ic: obj -> value */
OP(get_field2, 9, 1, 2)             /* obj -> value obj (for method calls) */
OP(put_field, 9, 2, 0)              /* obj value -> */
OP(get_elem, 1, 2, 1)               /* obj key -> value */
OP(get_elem2, 1, 2, 2)              /* obj key -> value obj */
OP(put_elem, 1, 3, 0)               /* obj key value -> */
OP(get_length, 1, 1, 1)
OP(get_field_recv, 5, 2, 1)         /* u32 name: receiver obj -> value (super.x) */
OP(get_elem_recv, 1, 3, 1)          /* receiver obj key -> value */
OP(put_field_recv, 5, 3, 0)         /* u32 name: receiver obj value -> */
OP(put_elem_recv, 1, 4, 0)          /* receiver obj key value -> */
OP(get_super, 1, 1, 1)              /* home -> home.[[GetPrototypeOf]]() */
OP(delete_field, 5, 1, 1)           /* u32 name: obj -> bool */
OP(delete_elem, 1, 2, 1)            /* obj key -> bool */
OP(define_field, 5, 2, 1)           /* u32 name: obj value -> obj (CreateDataProperty) */
OP(define_elem, 1, 3, 1)            /* obj key value -> obj */
OP(define_method, 6, 2, 1)          /* u32 name, u8 kind: obj func -> obj (sets the home object) */
OP(define_method_elem, 2, 3, 1)     /* u8 kind: obj key func -> obj */
OP(set_proto, 1, 2, 1)              /* obj proto -> obj (__proto__: in a literal) */
OP(copy_data_props, 2, -1, 1)       /* u8 nexcl: target source excl... -> target (spread and rest) */
OP(get_private, 1, 2, 1)            /* obj name -> value */
OP(put_private, 1, 3, 0)            /* obj name value -> */
OP(define_private, 1, 3, 1)         /* obj name value -> obj (TypeError if present) */
OP(define_private_method, 2, 3, 1)  /* u8 kind: obj name func -> obj */
OP(private_in, 1, 2, 1)             /* name obj -> bool */
/* operators */
OP(add, 1, 2, 1)
OP(sub, 1, 2, 1)
OP(mul, 1, 2, 1)
OP(div, 1, 2, 1)
OP(mod, 1, 2, 1)
OP(pow, 1, 2, 1)
OP(band, 1, 2, 1)
OP(bor, 1, 2, 1)
OP(bxor, 1, 2, 1)
OP(shl, 1, 2, 1)
OP(sar, 1, 2, 1)
OP(shr, 1, 2, 1)
OP(lt, 1, 2, 1)
OP(le, 1, 2, 1)
OP(gt, 1, 2, 1)
OP(ge, 1, 2, 1)
OP(eq, 1, 2, 1)
OP(ne, 1, 2, 1)
OP(seq, 1, 2, 1)
OP(sne, 1, 2, 1)
OP(instanceof, 1, 2, 1)
OP(in, 1, 2, 1)
OP(neg, 1, 1, 1)
OP(plus, 1, 1, 1)                   /* ToNumber (unary +) */
OP(to_numeric, 1, 1, 1)
OP(not, 1, 1, 1)
OP(bnot, 1, 1, 1)
OP(typeof, 1, 1, 1)
OP(inc, 1, 1, 1)
OP(dec, 1, 1, 1)
OP(to_propkey, 1, 1, 1)
OP(to_string, 1, 1, 1)
OP(is_undefined, 1, 1, 1)
OP(is_nullish, 1, 1, 1)
/* control */
OP(goto, 5, 0, 0)
OP(if_true, 5, 1, 0)
OP(if_false, 5, 1, 0)
OP(if_nullish, 5, 0, 0)             /* jump when the top is null/undefined (keeps it) */
OP(if_not_nullish, 5, 0, 0)
OP(if_undefined, 5, 0, 0)
OP(if_not_undefined, 5, 0, 0)
OP(return, 1, 1, 0)
OP(return_undef, 1, 0, 0)
OP(throw, 1, 1, 0)
OP(finally_end, 3, 2, 0)            /* u16 n, then n i32 offsets: value type -> (0 continue, 1 rethrow, k jump k-2) */
OP(check_ctor_return, 1, 2, 1)      /* derived constructors: value this -> what to return */
OP(init_this_loc, 3, 1, 0)          /* u16: super()'s result into the this binding (ReferenceError if set) */
OP(init_this_lcell, 3, 1, 0)
OP(init_this_cvar, 3, 1, 0)
OP(debugger, 1, 0, 0)
/* iteration */
OP(for_in_start, 1, 1, 1)           /* obj -> iterator */
OP(for_in_next, 5, 0, 1)            /* iter -> iter key, or jump when done */
OP(for_of_start, 1, 1, 2)           /* iterable -> iter next */
OP(for_await_start, 1, 1, 2)
OP(for_of_next, 5, 0, 1)            /* iter next -> iter next value, or jump when done */
OP(iter_call_next, 1, 2, 3)         /* iter next -> iter next result (for-await: await the result next) */
OP(iter_result, 5, 1, 1)            /* result -> value, or jump (dropping it) when done */
OP(iter_close, 1, 2, 0)             /* iter next -> (iterator.return()) */
OP(iter_close_throw, 1, 3, 0)       /* iter next exc -> rethrows exc after return() (whose errors are ignored) */
OP(iter_step, 1, 3, 4)              /* iter next done -> iter next done value */
OP(iter_rest, 1, 3, 4)              /* iter next done -> iter next done array */
OP(iter_finish, 1, 3, 0)            /* iter next done -> (return() unless done) */
OP(iter_finish_throw, 1, 4, 0)      /* iter next done exc -> rethrow after closing if not done */
OP(get_iterator, 1, 1, 2)           /* iterable -> iter next */
OP(get_async_iterator, 1, 1, 2)
OP(push_done, 1, 0, 1)              /* the initial "done" flag (false) */
/* calls */
OP(call, 3, -1, 1)                  /* u16 argc: func this args... -> result */
OP(new, 3, -1, 1)                   /* u16 argc: ctor args... -> object */
OP(call_spread, 1, 3, 1)            /* func this array -> result */
OP(new_spread, 1, 2, 1)             /* ctor array -> object */
OP(eval, 7, -1, 1)                  /* u16 argc, u16 first visible name, u16 count: like call, direct eval if %eval% */
OP(super_call, 3, -1, 1)            /* u16 argc: args... -> this */
OP(super_call_spread, 1, 3, 1)      /* func new.target array -> this */
OP(import_call, 1, 2, 1)            /* specifier options -> promise */
/* functions and classes */
OP(closure, 5, 0, 1)                /* u32 constant (bytecode) */
OP(set_name, 5, 1, 1)               /* u32 name: func -> func (anonymous function definitions) */
OP(set_name_computed, 2, 2, 2)      /* u8 prefix (0, 1 get, 2 set): key func -> key func */
OP(set_home, 1, 2, 2)               /* home func -> home func */
OP(class_create, 6, 1, 2)           /* u32 constructor bytecode, u8 has_heritage: heritage -> ctor proto */
OP(private_name, 5, 0, 1)           /* u32 description: a new private name */
OP(set_fields, 1, 2, 1)             /* ctor fields-initialiser -> ctor */
OP(run_fields, 1, 2, 0)             /* func this -> (the class's field records applied to this) */
OP(set_class_name, 5, 1, 1)
/* generators and async functions */
OP(initial_yield, 1, 0, 0)
OP(yield, 1, 1, 2)                  /* value -> (suspends) -> received mode */
OP(resume_dispatch, 5, 2, 1)        /* received mode -> received (throw: throws it; return: jumps) */
OP(yield_star, 9, 4, 1)             /* i32 done, i32 return: iter next received mode -> value (jumps) or suspends */
OP(await, 1, 1, 1)                  /* value -> (suspends) -> result (throws a rejection) */
OP(async_yield_star, 5, 4, 3)       /* i32 return: iter next received mode -> iter next result-to-await (or jump) */
/* object and array literals */
OP(object, 1, 0, 1)
OP(array_from, 3, -1, 1)            /* u16 n: v1..vn -> array */
OP(array_append, 1, 2, 1)           /* array iterable -> array */
OP(array_push, 1, 2, 1)             /* array value -> array */
OP(array_hole, 1, 1, 1)             /* array -> array (one more hole) */
OP(template_object, 5, 0, 1)        /* u32 constant index of the [cooked, raw] pair; cached per site */
OP(regexp, 1, 2, 1)                 /* source flags -> regexp */
OP(special_object, 2, 0, 1)         /* u8: SO_* */
OP(rest, 3, 0, 1)                   /* u16 first: the rest parameter's array */
OP(check_coercible, 1, 1, 1)        /* TypeError for null/undefined (destructuring) */
OP(check_object, 1, 1, 1)           /* TypeError unless an object */
OP(check_this, 1, 1, 1)             /* derived constructors: ReferenceError if this is still uninitialised */
OP(bind_this, 1, 1, 1)              /* stores the super() result as the frame's this; ReferenceError if already set */
OP(push_import_meta, 1, 0, 1)
OP(throw_iterator_result, 1, 1, 0)
OP(line, 1, 0, 0)
