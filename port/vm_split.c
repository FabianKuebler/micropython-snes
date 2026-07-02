// Split-VM: a per-opcode decomposition of py/vm.c's mp_execute_bytecode.
//
// WHY THIS FILE EXISTS (see DECISIONS.md, 2026-06 entries): both Calypsi and
// vbcc miscompile the original 21KB mp_execute_bytecode in a layout-sensitive
// way -- the register allocator loses the cached `ip` across calls somewhere
// in the giant re-entrant switch, and *where* depends on code layout. Every
// in-function workaround (volatile, -O levels, switch strategy) only shuffled
// the failure around. This file removes the giant function entirely:
//
//  - every opcode is its own tiny static function ("handler"), dispatched
//    through a 256-entry function table;
//  - ALL VM registers (ip, sp, exc_sp, ...) live in a vm_ctx_t struct that is
//    only ever accessed through a pointer, so they are memory-resident and no
//    allocator can lose them across calls;
//  - the function containing nlr_push (setjmp) keeps NO mutable locals, so
//    Calypsi bug 2 (dropped volatile stores to stack locals in setjmp
//    functions) cannot bite;
//  - shared jump targets of the original (exception_handler, unwind_return,
//    unwind_jump, load_check, yield) become helper functions; handlers signal
//    control transfers via a small status enum.
//
// This is a line-by-line transformation of py/vm.c (v1.28.0), specialised to
// this port's configuration. Build selects it INSTEAD of py/vm.c (make
// VM_SPLIT=1); py/vm.c itself is untouched. Config assumptions are enforced
// below. Correctness beats speed everywhere here: the table call per opcode
// costs cycles we happily pay.

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "py/emitglue.h"
#include "py/objtype.h"
#include "py/objfun.h"
#include "py/runtime.h"
#include "py/bc.h"
#include "py/bc0.h"

#if MICROPY_STACKLESS || MICROPY_PY_SYS_SETTRACE || MICROPY_PY_THREAD \
    || MICROPY_OPT_COMPUTED_GOTO || MICROPY_ENABLE_VM_ABORT || MICROPY_ENABLE_SCHEDULER \
    || MICROPY_PY_SYS_EXC_INFO || MICROPY_OPT_LOAD_ATTR_FAST_PATH || MICROPY_ENABLE_PYSTACK
#error "vm_split.c only supports the SNES port configuration (see py/vm.c for the general VM)"
#endif

#ifdef VM_TRACE
// Debug builds only (-DVM_TRACE): stream each dispatched opcode byte to the
// mailbox as hex so a diverging run can be lined up against mpy-tool -d.
#include "../snes/mailbox.h"
static void vm_trace(char tag, unsigned v) {
    static const char hexdig[] = "0123456789abcdef";
    mb_putc(tag);
    mb_putc(hexdig[(v >> 4) & 15]);
    mb_putc(hexdig[v & 15]);
    mb_putc(' ');
}
#define VM_TRACE_HOOK(tag, v) vm_trace((tag), (unsigned)(v))
// dump sp low byte and the top three value-stack words (low 16 bits each)
static void vm_trace_stack(void *sp_in) {
    mp_obj_t *sp = (mp_obj_t *)sp_in;
    union { mp_obj_t o; unsigned char b[4]; } u;
    int i;
    u.o = (mp_obj_t)sp_in;
    mb_putc('s');
    vm_trace(':', u.b[0]);
    for (i = 0; i > -3; i--) {
        u.o = sp[i];
        mb_putc('[');
        vm_trace(' ', u.b[1]);
        vm_trace(' ', u.b[0]);
        mb_putc(']');
    }
    mb_putc('\n');
}
#define VM_TRACE_STACK(sp) vm_trace_stack(sp)
#else
#define VM_TRACE_HOOK(tag, v)
#define VM_TRACE_STACK(sp)
#endif

// All mutable VM state. Lives in mp_execute_bytecode's frame but is accessed
// exclusively through this pointer (by handlers and helpers in other stack
// frames), which forces it into memory.
typedef struct _vm_ctx_t {
    mp_code_state_t *code_state;
    const byte *ip;             // live bytecode pointer (code_state->ip marks the executing opcode)
    byte opcode;                // opcode being executed (MULTI handlers decode it)
    mp_obj_t *sp;               // live value-stack pointer (points at top element)
    mp_obj_t *fastn;            // fastn[0] is local 0, fastn[-1] is local 1, ...
    mp_exc_stack_t *exc_stack;  // base of exception-block stack
    mp_exc_stack_t *exc_sp;     // top of exception-block stack (grows up)
    #if MICROPY_EMIT_BYTECODE_USES_QSTR_TABLE
    const qstr_short_t *qstr_table;
    #endif
    mp_obj_t inject_exc;        // pending injected exception (generator throw)
    mp_obj_t cur_exc;           // exception being RAISEd via VM_ST_RAISE
} vm_ctx_t;

// Handler return statuses; these replace the shared goto labels of py/vm.c.
typedef enum {
    VM_ST_DISPATCH,       // execute next opcode (== DISPATCH())
    VM_ST_DISPATCH_CHECK, // ditto plus pending-exception check (branches)
    VM_ST_RAISE,          // c->cur_exc holds the exception (== RAISE(o))
    VM_ST_RETURN,         // function returned; nlr already popped
    VM_ST_YIELD,          // generator yielded; state saved, nlr popped
    VM_ST_EXC_RETURN,     // fatal: exception in state[0], nlr popped
} vm_status_t;

typedef vm_status_t (*vm_op_fun_t)(vm_ctx_t *c);

// ---- decode/stack macros, transformed from py/vm.c to ctx form --------------
// Every handler's parameter is named `c`, so bodies below stay near-verbatim.

#define DECODE_UINT \
    mp_uint_t unum = 0; \
    do { \
        unum = (unum << 7) + (*c->ip & 0x7f); \
    } while ((*c->ip++ & 0x80) != 0)

#define DECODE_ULABEL \
    size_t ulab; \
    do { \
        if (c->ip[0] & 0x80) { \
            ulab = ((c->ip[0] & 0x7f) | (c->ip[1] << 7)); \
            c->ip += 2; \
        } else { \
            ulab = c->ip[0]; \
            c->ip += 1; \
        } \
    } while (0)

#define DECODE_SLABEL \
    size_t slab; \
    do { \
        if (c->ip[0] & 0x80) { \
            slab = ((c->ip[0] & 0x7f) | (c->ip[1] << 7)) - 0x4000; \
            c->ip += 2; \
        } else { \
            slab = c->ip[0] - 0x40; \
            c->ip += 1; \
        } \
    } while (0)

#if MICROPY_EMIT_BYTECODE_USES_QSTR_TABLE
#define DECODE_QSTR \
    DECODE_UINT; \
    qstr qst = c->qstr_table[unum]
#else
#define DECODE_QSTR \
    DECODE_UINT; \
    qstr qst = unum;
#endif

#define DECODE_PTR \
    DECODE_UINT; \
    void *ptr = (void *)(uintptr_t)c->code_state->fun_bc->child_table[unum]

#define DECODE_OBJ \
    DECODE_UINT; \
    mp_obj_t obj = (mp_obj_t)c->code_state->fun_bc->context->constants.obj_table[unum]

#define PUSH(val) (*++c->sp = (val))
#define POP() (*c->sp--)
#define TOP() (*c->sp)
#define SET_TOP(val) (*c->sp = (val))

// Dereferencing BELOW a far pointer must never compile to a negative Y index:
// Calypsi emits `ldy ##-N` + `lda/sta [dp],y`, and the 65816 adds Y as an
// UNSIGNED 16-bit value across the whole 24-bit address — the access lands in
// the NEXT bank (reads garbage, writes vanish into ROM). Forcing the adjusted
// pointer through a volatile temp materializes a real pointer decrement
// (low-word arithmetic, bank preserved) that the optimizer cannot fold back
// into an indexed form. Use SP_AT(c, -n) for every c->sp[-n].
static inline mp_obj_t *vm_ptr_at(mp_obj_t *base, int off) {
    mp_obj_t *volatile p = base + off;
    return p;
}
#define SP_AT(c, off) (*vm_ptr_at((c)->sp, (off)))

#define PUSH_EXC_BLOCK(with_or_finally) do { \
    DECODE_ULABEL; /* except labels are always forward */ \
    ++c->exc_sp; \
    c->exc_sp->handler = c->ip + ulab; \
    c->exc_sp->val_sp = MP_TAGPTR_MAKE(c->sp, ((with_or_finally) << 1)); \
    c->exc_sp->prev_exc = NULL; \
} while (0)

#define POP_EXC_BLOCK() \
    c->exc_sp-- /* pop back to previous exception handler */

#define CANCEL_ACTIVE_FINALLY(sp) do { \
    if (mp_obj_is_small_int(*vm_ptr_at(sp, -1))) { \
        /* Stack: (..., prev_dest_ip, prev_cause, dest_ip) */ \
        /* Cancel the unwind through the previous finally, replace with current one */ \
        *vm_ptr_at(sp, -2) = sp[0]; \
        sp -= 2; \
    } else { \
        /* Stack: (..., None/exception, dest_ip) */ \
        /* Silence the finally's exception value (may be None or an exception) */ \
        *vm_ptr_at(sp, -1) = sp[0]; \
        --sp; \
    } \
} while (0)

// ---- shared-label helpers ----------------------------------------------------

// original label: local_name_error
static vm_status_t vm_local_name_error(vm_ctx_t *c) {
    c->cur_exc = mp_obj_new_exception_msg(&mp_type_NameError,
        MP_ERROR_TEXT("local variable referenced before assignment"));
    return VM_ST_RAISE;
}

// original label: load_check
static vm_status_t vm_load_check(vm_ctx_t *c, mp_obj_t obj_shared) {
    if (obj_shared == MP_OBJ_NULL) {
        return vm_local_name_error(c);
    }
    PUSH(obj_shared);
    return VM_ST_DISPATCH;
}

// original label: unwind_return (entered from RETURN_VALUE and END_FINALLY).
// Search for and execute finally handlers that aren't already active.
static vm_status_t vm_unwind_return(vm_ctx_t *c) {
    while (c->exc_sp >= c->exc_stack) {
        if (MP_TAGPTR_TAG1(c->exc_sp->val_sp)) {
            if (c->exc_sp->handler >= c->ip) {
                // Found a finally handler that isn't active; run it.
                // Getting here the stack looks like: (..., X, [iter0, ...,] ret_val)
                // where X is pointed to by exc_sp->val_sp. Copy ret_val down over
                // any loop iterators, then run the finally as a coroutine with a
                // small-int sentinel telling END_FINALLY this is an unwind return.
                mp_obj_t *finally_sp = MP_TAGPTR_PTR(c->exc_sp->val_sp);
                finally_sp[1] = c->sp[0];
                c->sp = &finally_sp[1];
                PUSH(MP_OBJ_NEW_SMALL_INT(-1));
                c->ip = c->exc_sp->handler;
                return VM_ST_DISPATCH;
            } else {
                // Found a finally handler that is already active; cancel it.
                CANCEL_ACTIVE_FINALLY(c->sp);
            }
        }
        POP_EXC_BLOCK();
    }
    nlr_pop();
    c->code_state->sp = c->sp;
    assert(c->exc_sp == c->exc_stack - 1);
    return VM_ST_RETURN;
}

// original label: unwind_jump (entered from UNWIND_JUMP and END_FINALLY).
// Stack on entry: (..., dest_ip, unum) where unum is the number of exception
// handlers to unwind (0x80 bit set: also pop an exhausted iterator).
static vm_status_t vm_unwind_jump(vm_ctx_t *c) {
    mp_uint_t unum = (mp_uint_t)POP();
    while ((unum & 0x7f) > 0) {
        unum -= 1;
        assert(c->exc_sp >= c->exc_stack);
        if (MP_TAGPTR_TAG1(c->exc_sp->val_sp)) {
            if (c->exc_sp->handler >= c->ip) {
                // Found a finally handler that isn't active; run it as a
                // coroutine with the remaining unwind count as sentinel.
                assert(&c->sp[-1] == MP_TAGPTR_PTR(c->exc_sp->val_sp));
                PUSH(MP_OBJ_NEW_SMALL_INT(unum));
                c->ip = c->exc_sp->handler;
                return VM_ST_DISPATCH;
            } else {
                // Found a finally handler that is already active; cancel it.
                CANCEL_ACTIVE_FINALLY(c->sp);
            }
        }
        POP_EXC_BLOCK();
    }
    c->ip = (const byte *)MP_OBJ_TO_PTR(POP()); // pop destination ip for jump
    if (unum != 0) {
        // pop the exhausted iterator
        c->sp -= MP_OBJ_ITER_BUF_NSLOTS;
    }
    return VM_ST_DISPATCH_CHECK;
}

// original label: yield
static vm_status_t vm_yield(vm_ctx_t *c) {
    nlr_pop();
    c->code_state->ip = c->ip;
    c->code_state->sp = c->sp;
    c->code_state->exc_sp_idx = MP_CODE_STATE_EXC_SP_IDX_FROM_PTR(c->exc_stack, c->exc_sp);
    return VM_ST_YIELD;
}

// ---- opcode handlers ---------------------------------------------------------

static vm_status_t op_load_const_false(vm_ctx_t *c) {
    PUSH(mp_const_false);
    return VM_ST_DISPATCH;
}

static vm_status_t op_load_const_none(vm_ctx_t *c) {
    PUSH(mp_const_none);
    return VM_ST_DISPATCH;
}

static vm_status_t op_load_const_true(vm_ctx_t *c) {
    PUSH(mp_const_true);
    return VM_ST_DISPATCH;
}

static vm_status_t op_load_const_small_int(vm_ctx_t *c) {
    mp_uint_t num = 0;
    if ((c->ip[0] & 0x40) != 0) {
        // Number is negative
        num--;
    }
    do {
        num = (num << 7) | (*c->ip & 0x7f);
    } while ((*c->ip++ & 0x80) != 0);
    PUSH(MP_OBJ_NEW_SMALL_INT(num));
    return VM_ST_DISPATCH;
}

static vm_status_t op_load_const_string(vm_ctx_t *c) {
    DECODE_QSTR;
    PUSH(MP_OBJ_NEW_QSTR(qst));
    return VM_ST_DISPATCH;
}

static vm_status_t op_load_const_obj(vm_ctx_t *c) {
    DECODE_OBJ;
    PUSH(obj);
    return VM_ST_DISPATCH;
}

static vm_status_t op_load_null(vm_ctx_t *c) {
    PUSH(MP_OBJ_NULL);
    return VM_ST_DISPATCH;
}

static vm_status_t op_load_fast_n(vm_ctx_t *c) {
    DECODE_UINT;
    return vm_load_check(c, *vm_ptr_at(c->fastn, -(int)unum));
}

static vm_status_t op_load_deref(vm_ctx_t *c) {
    DECODE_UINT;
    return vm_load_check(c, mp_obj_cell_get(*vm_ptr_at(c->fastn, -(int)unum)));
}

static vm_status_t op_load_name(vm_ctx_t *c) {
    DECODE_QSTR;
    PUSH(mp_load_name(qst));
    return VM_ST_DISPATCH;
}

static vm_status_t op_load_global(vm_ctx_t *c) {
    DECODE_QSTR;
    PUSH(mp_load_global(qst));
    return VM_ST_DISPATCH;
}

static vm_status_t op_load_attr(vm_ctx_t *c) {
    DECODE_QSTR;
    SET_TOP(mp_load_attr(TOP(), qst));
    return VM_ST_DISPATCH;
}

static vm_status_t op_load_method(vm_ctx_t *c) {
    DECODE_QSTR;
    mp_load_method(*c->sp, qst, c->sp);
    c->sp += 1;
    return VM_ST_DISPATCH;
}

static vm_status_t op_load_super_method(vm_ctx_t *c) {
    DECODE_QSTR;
    c->sp -= 1;
    mp_load_super_method(qst, c->sp - 1);
    return VM_ST_DISPATCH;
}

static vm_status_t op_load_build_class(vm_ctx_t *c) {
    PUSH(mp_load_build_class());
    return VM_ST_DISPATCH;
}

static vm_status_t op_load_subscr(vm_ctx_t *c) {
    mp_obj_t index = POP();
    SET_TOP(mp_obj_subscr(TOP(), index, MP_OBJ_SENTINEL));
    return VM_ST_DISPATCH;
}

static vm_status_t op_store_fast_n(vm_ctx_t *c) {
    DECODE_UINT;
    *vm_ptr_at(c->fastn, -(int)unum) = POP();
    return VM_ST_DISPATCH;
}

static vm_status_t op_store_deref(vm_ctx_t *c) {
    DECODE_UINT;
    mp_obj_cell_set(*vm_ptr_at(c->fastn, -(int)unum), POP());
    return VM_ST_DISPATCH;
}

static vm_status_t op_store_name(vm_ctx_t *c) {
    DECODE_QSTR;
    mp_store_name(qst, POP());
    return VM_ST_DISPATCH;
}

static vm_status_t op_store_global(vm_ctx_t *c) {
    DECODE_QSTR;
    mp_store_global(qst, POP());
    return VM_ST_DISPATCH;
}

static vm_status_t op_store_attr(vm_ctx_t *c) {
    DECODE_QSTR;
    mp_store_attr(c->sp[0], qst, SP_AT(c, -1));
    c->sp -= 2;
    return VM_ST_DISPATCH;
}

static vm_status_t op_store_subscr(vm_ctx_t *c) {
    mp_obj_subscr(SP_AT(c, -1), c->sp[0], SP_AT(c, -2));
    c->sp -= 3;
    return VM_ST_DISPATCH;
}

static vm_status_t op_delete_fast(vm_ctx_t *c) {
    DECODE_UINT;
    if (*vm_ptr_at(c->fastn, -(int)unum) == MP_OBJ_NULL) {
        return vm_local_name_error(c);
    }
    *vm_ptr_at(c->fastn, -(int)unum) = MP_OBJ_NULL;
    return VM_ST_DISPATCH;
}

static vm_status_t op_delete_deref(vm_ctx_t *c) {
    DECODE_UINT;
    if (mp_obj_cell_get(*vm_ptr_at(c->fastn, -(int)unum)) == MP_OBJ_NULL) {
        return vm_local_name_error(c);
    }
    mp_obj_cell_set(*vm_ptr_at(c->fastn, -(int)unum), MP_OBJ_NULL);
    return VM_ST_DISPATCH;
}

static vm_status_t op_delete_name(vm_ctx_t *c) {
    DECODE_QSTR;
    mp_delete_name(qst);
    return VM_ST_DISPATCH;
}

static vm_status_t op_delete_global(vm_ctx_t *c) {
    DECODE_QSTR;
    mp_delete_global(qst);
    return VM_ST_DISPATCH;
}

static vm_status_t op_dup_top(vm_ctx_t *c) {
    mp_obj_t top = TOP();
    PUSH(top);
    return VM_ST_DISPATCH;
}

static vm_status_t op_dup_top_two(vm_ctx_t *c) {
    c->sp += 2;
    c->sp[0] = SP_AT(c, -2);
    SP_AT(c, -1) = SP_AT(c, -3);
    return VM_ST_DISPATCH;
}

static vm_status_t op_pop_top(vm_ctx_t *c) {
    c->sp -= 1;
    return VM_ST_DISPATCH;
}

static vm_status_t op_rot_two(vm_ctx_t *c) {
    mp_obj_t top = c->sp[0];
    c->sp[0] = SP_AT(c, -1);
    SP_AT(c, -1) = top;
    return VM_ST_DISPATCH;
}

static vm_status_t op_rot_three(vm_ctx_t *c) {
    mp_obj_t top = c->sp[0];
    c->sp[0] = SP_AT(c, -1);
    SP_AT(c, -1) = SP_AT(c, -2);
    SP_AT(c, -2) = top;
    return VM_ST_DISPATCH;
}

static vm_status_t op_jump(vm_ctx_t *c) {
    DECODE_SLABEL;
    c->ip += slab;
    return VM_ST_DISPATCH_CHECK;
}

static vm_status_t op_pop_jump_if_true(vm_ctx_t *c) {
    DECODE_SLABEL;
    if (mp_obj_is_true(POP())) {
        c->ip += slab;
    }
    return VM_ST_DISPATCH_CHECK;
}

static vm_status_t op_pop_jump_if_false(vm_ctx_t *c) {
    DECODE_SLABEL;
    if (!mp_obj_is_true(POP())) {
        c->ip += slab;
    }
    return VM_ST_DISPATCH_CHECK;
}

static vm_status_t op_jump_if_true_or_pop(vm_ctx_t *c) {
    DECODE_ULABEL;
    if (mp_obj_is_true(TOP())) {
        c->ip += ulab;
    } else {
        c->sp--;
    }
    return VM_ST_DISPATCH_CHECK;
}

static vm_status_t op_jump_if_false_or_pop(vm_ctx_t *c) {
    DECODE_ULABEL;
    if (mp_obj_is_true(TOP())) {
        c->sp--;
    } else {
        c->ip += ulab;
    }
    return VM_ST_DISPATCH_CHECK;
}

static vm_status_t op_setup_with(vm_ctx_t *c) {
    // stack: (..., ctx_mgr)
    mp_obj_t obj = TOP();
    mp_load_method(obj, MP_QSTR___exit__, c->sp);
    mp_load_method(obj, MP_QSTR___enter__, c->sp + 2);
    mp_obj_t ret = mp_call_method_n_kw(0, 0, c->sp + 2);
    c->sp += 1;
    PUSH_EXC_BLOCK(1);
    PUSH(ret);
    // stack: (..., __exit__, ctx_mgr, as_value)
    return VM_ST_DISPATCH;
}

static vm_status_t op_with_cleanup(vm_ctx_t *c) {
    // Arriving here, there's an "exception control block" on top of stack,
    // and the __exit__ method (with self) underneath it; see py/vm.c.
    if (TOP() == mp_const_none) {
        // stack: (..., __exit__, ctx_mgr, None)
        c->sp[1] = mp_const_none;
        c->sp[2] = mp_const_none;
        c->sp -= 2;
        mp_call_method_n_kw(3, 0, c->sp);
        SET_TOP(mp_const_none);
    } else if (mp_obj_is_small_int(TOP())) {
        // unwind return or unwind jump; same handling for both
        mp_obj_t data = SP_AT(c, -1);
        mp_obj_t cause = c->sp[0];
        SP_AT(c, -1) = mp_const_none;
        c->sp[0] = mp_const_none;
        c->sp[1] = mp_const_none;
        mp_call_method_n_kw(3, 0, c->sp - 3);
        SP_AT(c, -3) = data;
        SP_AT(c, -2) = cause;
        c->sp -= 2; // we removed (__exit__, ctx_mgr)
    } else {
        assert(mp_obj_is_exception_instance(TOP()));
        // stack: (..., __exit__, ctx_mgr, exc_instance)
        // Need to pass (exc_type, exc_instance, None) as arguments to __exit__.
        c->sp[1] = c->sp[0];
        c->sp[0] = MP_OBJ_FROM_PTR(mp_obj_get_type(c->sp[0]));
        c->sp[2] = mp_const_none;
        c->sp -= 2;
        mp_obj_t ret_value = mp_call_method_n_kw(3, 0, c->sp);
        if (mp_obj_is_true(ret_value)) {
            // Swallow the exception: END_FINALLY sees None and runs normally.
            SET_TOP(mp_const_none);
        } else {
            // Re-raise: copy the exception instance down to the new TOS.
            c->sp[0] = c->sp[3];
        }
    }
    return VM_ST_DISPATCH;
}

static vm_status_t op_unwind_jump(vm_ctx_t *c) {
    DECODE_SLABEL;
    PUSH((mp_obj_t)(mp_uint_t)(uintptr_t)(c->ip + slab)); // push destination ip for jump
    PUSH((mp_obj_t)(mp_uint_t)(*c->ip)); // push number of exception handlers to unwind (0x80 bit: also pop iterator)
    return vm_unwind_jump(c);
}

static vm_status_t op_setup_except_or_finally(vm_ctx_t *c) {
    // code_state->ip still points at the opcode itself (marked by dispatch)
    PUSH_EXC_BLOCK((c->code_state->ip[0] == MP_BC_SETUP_FINALLY) ? 1 : 0);
    return VM_ST_DISPATCH;
}

static vm_status_t op_end_finally(vm_ctx_t *c) {
    // if TOS is None, just pops it and continues
    // if TOS is an integer, finishes coroutine and returns control to caller
    // if TOS is an exception, reraises the exception
    assert(c->exc_sp >= c->exc_stack);
    POP_EXC_BLOCK();
    if (TOP() == mp_const_none) {
        c->sp--;
    } else if (mp_obj_is_small_int(TOP())) {
        // We finished a "finally" coroutine; dispatch back to our caller
        mp_int_t cause = MP_OBJ_SMALL_INT_VALUE(POP());
        if (cause < 0) {
            // A negative cause indicates unwind return
            return vm_unwind_return(c);
        } else {
            // Otherwise it's an unwind jump: push back the raw unwind count
            PUSH((mp_obj_t)cause);
            return vm_unwind_jump(c);
        }
    } else {
        assert(mp_obj_is_exception_instance(TOP()));
        c->cur_exc = TOP();
        return VM_ST_RAISE;
    }
    return VM_ST_DISPATCH;
}

static vm_status_t op_get_iter(vm_ctx_t *c) {
    SET_TOP(mp_getiter(TOP(), NULL));
    return VM_ST_DISPATCH;
}

// An iterator for a for-loop takes MP_OBJ_ITER_BUF_NSLOTS slots on the Python
// value stack; either the iterator itself lives there, or slot 0 is
// MP_OBJ_NULL and slot 1 references the (heap) iterator.
static vm_status_t op_get_iter_stack(vm_ctx_t *c) {
    mp_obj_t obj = TOP();
    mp_obj_iter_buf_t *iter_buf = (mp_obj_iter_buf_t *)c->sp;
    c->sp += MP_OBJ_ITER_BUF_NSLOTS - 1;
    obj = mp_getiter(obj, iter_buf);
    if (obj != MP_OBJ_FROM_PTR(iter_buf)) {
        // Iterator didn't use the stack so indicate that with MP_OBJ_NULL.
        *(c->sp - MP_OBJ_ITER_BUF_NSLOTS + 1) = MP_OBJ_NULL;
        *(c->sp - MP_OBJ_ITER_BUF_NSLOTS + 2) = obj;
    }
    return VM_ST_DISPATCH;
}

static vm_status_t op_for_iter(vm_ctx_t *c) {
    DECODE_ULABEL; // the jump offset if iteration finishes; for labels are always forward
    c->code_state->sp = c->sp;
    mp_obj_t obj;
    if (*(c->sp - MP_OBJ_ITER_BUF_NSLOTS + 1) == MP_OBJ_NULL) {
        obj = *(c->sp - MP_OBJ_ITER_BUF_NSLOTS + 2);
    } else {
        obj = MP_OBJ_FROM_PTR(vm_ptr_at(c->sp, -MP_OBJ_ITER_BUF_NSLOTS + 1));
    }
    mp_obj_t value = mp_iternext_allow_raise(obj);
    if (value == MP_OBJ_STOP_ITERATION) {
        c->sp -= MP_OBJ_ITER_BUF_NSLOTS; // pop the exhausted iterator
        c->ip += ulab; // jump to after for-block
    } else {
        PUSH(value); // push the next iteration value
    }
    return VM_ST_DISPATCH;
}

static vm_status_t op_pop_except_jump(vm_ctx_t *c) {
    assert(c->exc_sp >= c->exc_stack);
    POP_EXC_BLOCK();
    DECODE_ULABEL;
    c->ip += ulab;
    return VM_ST_DISPATCH_CHECK;
}

static vm_status_t op_build_tuple(vm_ctx_t *c) {
    DECODE_UINT;
    c->sp -= unum - 1;
    SET_TOP(mp_obj_new_tuple(unum, c->sp));
    return VM_ST_DISPATCH;
}

static vm_status_t op_build_list(vm_ctx_t *c) {
    DECODE_UINT;
    c->sp -= unum - 1;
    SET_TOP(mp_obj_new_list(unum, c->sp));
    return VM_ST_DISPATCH;
}

static vm_status_t op_build_map(vm_ctx_t *c) {
    DECODE_UINT;
    PUSH(mp_obj_new_dict(unum));
    return VM_ST_DISPATCH;
}

static vm_status_t op_store_map(vm_ctx_t *c) {
    c->sp -= 2;
    mp_obj_dict_store(c->sp[0], c->sp[2], c->sp[1]);
    return VM_ST_DISPATCH;
}

#if MICROPY_PY_BUILTINS_SET
static vm_status_t op_build_set(vm_ctx_t *c) {
    DECODE_UINT;
    c->sp -= unum - 1;
    SET_TOP(mp_obj_new_set(unum, c->sp));
    return VM_ST_DISPATCH;
}
#endif

#if MICROPY_PY_BUILTINS_SLICE
static vm_status_t op_build_slice(vm_ctx_t *c) {
    mp_obj_t step = mp_const_none;
    if (*c->ip++ == 3) {
        // 3-argument slice includes step
        step = POP();
    }
    mp_obj_t stop = POP();
    mp_obj_t start = TOP();
    SET_TOP(mp_obj_new_slice(start, stop, step));
    return VM_ST_DISPATCH;
}
#endif

static vm_status_t op_store_comp(vm_ctx_t *c) {
    DECODE_UINT;
    mp_obj_t obj = *vm_ptr_at(c->sp, -(int)(unum >> 2));
    if ((unum & 3) == 0) {
        mp_obj_list_append(obj, c->sp[0]);
        c->sp--;
    } else if (!MICROPY_PY_BUILTINS_SET || (unum & 3) == 1) {
        mp_obj_dict_store(obj, c->sp[0], SP_AT(c, -1));
        c->sp -= 2;
    #if MICROPY_PY_BUILTINS_SET
    } else {
        mp_obj_set_store(obj, c->sp[0]);
        c->sp--;
    #endif
    }
    return VM_ST_DISPATCH;
}

static vm_status_t op_unpack_sequence(vm_ctx_t *c) {
    DECODE_UINT;
    mp_unpack_sequence(c->sp[0], unum, c->sp);
    c->sp += unum - 1;
    return VM_ST_DISPATCH;
}

static vm_status_t op_unpack_ex(vm_ctx_t *c) {
    DECODE_UINT;
    mp_unpack_ex(c->sp[0], unum, c->sp);
    c->sp += (unum & 0xff) + ((unum >> 8) & 0xff);
    return VM_ST_DISPATCH;
}

static vm_status_t op_make_function(vm_ctx_t *c) {
    DECODE_PTR;
    PUSH(mp_make_function_from_proto_fun(ptr, c->code_state->fun_bc->context, NULL));
    return VM_ST_DISPATCH;
}

static vm_status_t op_make_function_defargs(vm_ctx_t *c) {
    DECODE_PTR;
    // Stack layout: def_tuple def_dict <- TOS
    c->sp -= 1;
    SET_TOP(mp_make_function_from_proto_fun(ptr, c->code_state->fun_bc->context, c->sp));
    return VM_ST_DISPATCH;
}

static vm_status_t op_make_closure(vm_ctx_t *c) {
    DECODE_PTR;
    size_t n_closed_over = *c->ip++;
    // Stack layout: closed_overs <- TOS
    c->sp -= n_closed_over - 1;
    SET_TOP(mp_make_closure_from_proto_fun(ptr, c->code_state->fun_bc->context, n_closed_over, c->sp));
    return VM_ST_DISPATCH;
}

static vm_status_t op_make_closure_defargs(vm_ctx_t *c) {
    DECODE_PTR;
    size_t n_closed_over = *c->ip++;
    // Stack layout: def_tuple def_dict closed_overs <- TOS
    c->sp -= 2 + n_closed_over - 1;
    SET_TOP(mp_make_closure_from_proto_fun(ptr, c->code_state->fun_bc->context, 0x100 | n_closed_over, c->sp));
    return VM_ST_DISPATCH;
}

static vm_status_t op_call_function(vm_ctx_t *c) {
    DECODE_UINT;
    VM_TRACE_HOOK('u', unum);
    // unum & 0xff == n_positional, (unum >> 8) & 0xff == n_keyword
    c->sp -= (unum & 0xff) + ((unum >> 7) & 0x1fe);
    SET_TOP(mp_call_function_n_kw(*c->sp, unum & 0xff, (unum >> 8) & 0xff, c->sp + 1));
    return VM_ST_DISPATCH;
}

static vm_status_t op_call_function_var_kw(vm_ctx_t *c) {
    DECODE_UINT;
    // Stack: fun arg0 arg1 ... kw0 val0 kw1 val1 ... bitmap <- TOS
    c->sp -= (unum & 0xff) + ((unum >> 7) & 0x1fe) + 1;
    SET_TOP(mp_call_method_n_kw_var(false, unum, c->sp));
    return VM_ST_DISPATCH;
}

static vm_status_t op_call_method(vm_ctx_t *c) {
    DECODE_UINT;
    c->sp -= (unum & 0xff) + ((unum >> 7) & 0x1fe) + 1;
    SET_TOP(mp_call_method_n_kw(unum & 0xff, (unum >> 8) & 0xff, c->sp));
    return VM_ST_DISPATCH;
}

static vm_status_t op_call_method_var_kw(vm_ctx_t *c) {
    DECODE_UINT;
    // Stack: fun self arg0 arg1 ... kw0 val0 kw1 val1 ... bitmap <- TOS
    c->sp -= (unum & 0xff) + ((unum >> 7) & 0x1fe) + 2;
    SET_TOP(mp_call_method_n_kw_var(true, unum, c->sp));
    return VM_ST_DISPATCH;
}

static vm_status_t op_return_value(vm_ctx_t *c) {
    return vm_unwind_return(c);
}

static vm_status_t op_raise_last(vm_ctx_t *c) {
    // search for the inner-most previous exception, to reraise it
    mp_obj_t obj = MP_OBJ_NULL;
    mp_exc_stack_t *e;
    for (e = c->exc_sp; e >= c->exc_stack; --e) {
        if (e->prev_exc != NULL) {
            obj = MP_OBJ_FROM_PTR(e->prev_exc);
            break;
        }
    }
    if (obj == MP_OBJ_NULL) {
        obj = mp_obj_new_exception_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("no active exception to reraise"));
    }
    c->cur_exc = obj;
    return VM_ST_RAISE;
}

static vm_status_t op_raise_obj(vm_ctx_t *c) {
    c->cur_exc = mp_make_raise_obj(TOP());
    return VM_ST_RAISE;
}

static vm_status_t op_raise_from(vm_ctx_t *c) {
    mp_obj_t from_value = POP();
    if (from_value != mp_const_none) {
        mp_warning(NULL, "exception chaining not supported");
    }
    c->cur_exc = mp_make_raise_obj(TOP());
    return VM_ST_RAISE;
}

static vm_status_t op_yield_value(vm_ctx_t *c) {
    return vm_yield(c);
}

static vm_status_t op_yield_from(vm_ctx_t *c) {
    mp_vm_return_kind_t ret_kind;
    mp_obj_t send_value = POP();
    mp_obj_t t_exc = MP_OBJ_NULL;
    mp_obj_t ret_value;
    c->code_state->sp = c->sp; // Save sp because it's needed if mp_resume raises StopIteration
    if (c->inject_exc != MP_OBJ_NULL) {
        t_exc = c->inject_exc;
        c->inject_exc = MP_OBJ_NULL;
        ret_kind = mp_resume(TOP(), MP_OBJ_NULL, t_exc, &ret_value);
    } else {
        ret_kind = mp_resume(TOP(), send_value, MP_OBJ_NULL, &ret_value);
    }
    if (ret_kind == MP_VM_RETURN_YIELD) {
        c->ip--;
        PUSH(ret_value);
        return vm_yield(c);
    } else if (ret_kind == MP_VM_RETURN_NORMAL) {
        // The generator has finished; replace it with the value it returned
        SET_TOP(ret_value);
        // If we injected GeneratorExit downstream, then even if it was
        // swallowed, we re-raise GeneratorExit
        if (t_exc != MP_OBJ_NULL && mp_obj_exception_match(t_exc, MP_OBJ_FROM_PTR(&mp_type_GeneratorExit))) {
            c->cur_exc = mp_make_raise_obj(t_exc);
            return VM_ST_RAISE;
        }
        return VM_ST_DISPATCH;
    } else {
        assert(ret_kind == MP_VM_RETURN_EXCEPTION);
        assert(!mp_obj_exception_match(ret_value, MP_OBJ_FROM_PTR(&mp_type_StopIteration)));
        // Pop exhausted gen
        c->sp--;
        c->cur_exc = ret_value;
        return VM_ST_RAISE;
    }
}

static vm_status_t op_import_name(vm_ctx_t *c) {
    DECODE_QSTR;
    mp_obj_t obj = POP();
    SET_TOP(mp_import_name(qst, obj, TOP()));
    return VM_ST_DISPATCH;
}

static vm_status_t op_import_from(vm_ctx_t *c) {
    DECODE_QSTR;
    mp_obj_t obj = mp_import_from(TOP(), qst);
    PUSH(obj);
    return VM_ST_DISPATCH;
}

static vm_status_t op_import_star(vm_ctx_t *c) {
    mp_import_all(POP());
    return VM_ST_DISPATCH;
}

static vm_status_t op_load_const_small_int_multi(vm_ctx_t *c) {
    PUSH(MP_OBJ_NEW_SMALL_INT((mp_int_t)c->opcode - MP_BC_LOAD_CONST_SMALL_INT_MULTI - MP_BC_LOAD_CONST_SMALL_INT_MULTI_EXCESS));
    return VM_ST_DISPATCH;
}

static vm_status_t op_load_fast_multi(vm_ctx_t *c) {
    return vm_load_check(c, *vm_ptr_at(c->fastn, MP_BC_LOAD_FAST_MULTI - (int)c->opcode));
}

static vm_status_t op_store_fast_multi(vm_ctx_t *c) {
    *vm_ptr_at(c->fastn, MP_BC_STORE_FAST_MULTI - (int)c->opcode) = POP();
    return VM_ST_DISPATCH;
}

static vm_status_t op_unary_op_multi(vm_ctx_t *c) {
    SET_TOP(mp_unary_op(c->opcode - MP_BC_UNARY_OP_MULTI, TOP()));
    return VM_ST_DISPATCH;
}

static vm_status_t op_binary_op_multi(vm_ctx_t *c) {
    mp_obj_t rhs = POP();
    mp_obj_t lhs = TOP();
    SET_TOP(mp_binary_op(c->opcode - MP_BC_BINARY_OP_MULTI, lhs, rhs));
    return VM_ST_DISPATCH;
}

static vm_status_t op_not_implemented(vm_ctx_t *c) {
    mp_obj_t obj = mp_obj_new_exception_msg(&mp_type_NotImplementedError, MP_ERROR_TEXT("opcode"));
    nlr_pop();
    c->code_state->state[0] = obj;
    return VM_ST_EXC_RETURN;
}

// ---- dispatch table ----------------------------------------------------------

static vm_op_fun_t vm_op_table[256];
static byte vm_op_table_ready;

static void vm_op_table_init(void) {
    int i;
    for (i = 0; i < 256; ++i) {
        vm_op_table[i] = op_not_implemented;
    }
    vm_op_table[MP_BC_LOAD_CONST_FALSE] = op_load_const_false;
    vm_op_table[MP_BC_LOAD_CONST_NONE] = op_load_const_none;
    vm_op_table[MP_BC_LOAD_CONST_TRUE] = op_load_const_true;
    vm_op_table[MP_BC_LOAD_CONST_SMALL_INT] = op_load_const_small_int;
    vm_op_table[MP_BC_LOAD_CONST_STRING] = op_load_const_string;
    vm_op_table[MP_BC_LOAD_CONST_OBJ] = op_load_const_obj;
    vm_op_table[MP_BC_LOAD_NULL] = op_load_null;
    vm_op_table[MP_BC_LOAD_FAST_N] = op_load_fast_n;
    vm_op_table[MP_BC_LOAD_DEREF] = op_load_deref;
    vm_op_table[MP_BC_LOAD_NAME] = op_load_name;
    vm_op_table[MP_BC_LOAD_GLOBAL] = op_load_global;
    vm_op_table[MP_BC_LOAD_ATTR] = op_load_attr;
    vm_op_table[MP_BC_LOAD_METHOD] = op_load_method;
    vm_op_table[MP_BC_LOAD_SUPER_METHOD] = op_load_super_method;
    vm_op_table[MP_BC_LOAD_BUILD_CLASS] = op_load_build_class;
    vm_op_table[MP_BC_LOAD_SUBSCR] = op_load_subscr;
    vm_op_table[MP_BC_STORE_FAST_N] = op_store_fast_n;
    vm_op_table[MP_BC_STORE_DEREF] = op_store_deref;
    vm_op_table[MP_BC_STORE_NAME] = op_store_name;
    vm_op_table[MP_BC_STORE_GLOBAL] = op_store_global;
    vm_op_table[MP_BC_STORE_ATTR] = op_store_attr;
    vm_op_table[MP_BC_STORE_SUBSCR] = op_store_subscr;
    vm_op_table[MP_BC_DELETE_FAST] = op_delete_fast;
    vm_op_table[MP_BC_DELETE_DEREF] = op_delete_deref;
    vm_op_table[MP_BC_DELETE_NAME] = op_delete_name;
    vm_op_table[MP_BC_DELETE_GLOBAL] = op_delete_global;
    vm_op_table[MP_BC_DUP_TOP] = op_dup_top;
    vm_op_table[MP_BC_DUP_TOP_TWO] = op_dup_top_two;
    vm_op_table[MP_BC_POP_TOP] = op_pop_top;
    vm_op_table[MP_BC_ROT_TWO] = op_rot_two;
    vm_op_table[MP_BC_ROT_THREE] = op_rot_three;
    vm_op_table[MP_BC_JUMP] = op_jump;
    vm_op_table[MP_BC_POP_JUMP_IF_TRUE] = op_pop_jump_if_true;
    vm_op_table[MP_BC_POP_JUMP_IF_FALSE] = op_pop_jump_if_false;
    vm_op_table[MP_BC_JUMP_IF_TRUE_OR_POP] = op_jump_if_true_or_pop;
    vm_op_table[MP_BC_JUMP_IF_FALSE_OR_POP] = op_jump_if_false_or_pop;
    vm_op_table[MP_BC_SETUP_WITH] = op_setup_with;
    vm_op_table[MP_BC_WITH_CLEANUP] = op_with_cleanup;
    vm_op_table[MP_BC_UNWIND_JUMP] = op_unwind_jump;
    vm_op_table[MP_BC_SETUP_EXCEPT] = op_setup_except_or_finally;
    vm_op_table[MP_BC_SETUP_FINALLY] = op_setup_except_or_finally;
    vm_op_table[MP_BC_END_FINALLY] = op_end_finally;
    vm_op_table[MP_BC_GET_ITER] = op_get_iter;
    vm_op_table[MP_BC_GET_ITER_STACK] = op_get_iter_stack;
    vm_op_table[MP_BC_FOR_ITER] = op_for_iter;
    vm_op_table[MP_BC_POP_EXCEPT_JUMP] = op_pop_except_jump;
    vm_op_table[MP_BC_BUILD_TUPLE] = op_build_tuple;
    vm_op_table[MP_BC_BUILD_LIST] = op_build_list;
    vm_op_table[MP_BC_BUILD_MAP] = op_build_map;
    vm_op_table[MP_BC_STORE_MAP] = op_store_map;
    #if MICROPY_PY_BUILTINS_SET
    vm_op_table[MP_BC_BUILD_SET] = op_build_set;
    #endif
    #if MICROPY_PY_BUILTINS_SLICE
    vm_op_table[MP_BC_BUILD_SLICE] = op_build_slice;
    #endif
    vm_op_table[MP_BC_STORE_COMP] = op_store_comp;
    vm_op_table[MP_BC_UNPACK_SEQUENCE] = op_unpack_sequence;
    vm_op_table[MP_BC_UNPACK_EX] = op_unpack_ex;
    vm_op_table[MP_BC_MAKE_FUNCTION] = op_make_function;
    vm_op_table[MP_BC_MAKE_FUNCTION_DEFARGS] = op_make_function_defargs;
    vm_op_table[MP_BC_MAKE_CLOSURE] = op_make_closure;
    vm_op_table[MP_BC_MAKE_CLOSURE_DEFARGS] = op_make_closure_defargs;
    vm_op_table[MP_BC_CALL_FUNCTION] = op_call_function;
    vm_op_table[MP_BC_CALL_FUNCTION_VAR_KW] = op_call_function_var_kw;
    vm_op_table[MP_BC_CALL_METHOD] = op_call_method;
    vm_op_table[MP_BC_CALL_METHOD_VAR_KW] = op_call_method_var_kw;
    vm_op_table[MP_BC_RETURN_VALUE] = op_return_value;
    vm_op_table[MP_BC_RAISE_LAST] = op_raise_last;
    vm_op_table[MP_BC_RAISE_OBJ] = op_raise_obj;
    vm_op_table[MP_BC_RAISE_FROM] = op_raise_from;
    vm_op_table[MP_BC_YIELD_VALUE] = op_yield_value;
    vm_op_table[MP_BC_YIELD_FROM] = op_yield_from;
    vm_op_table[MP_BC_IMPORT_NAME] = op_import_name;
    vm_op_table[MP_BC_IMPORT_FROM] = op_import_from;
    vm_op_table[MP_BC_IMPORT_STAR] = op_import_star;
    for (i = 0; i < MP_BC_LOAD_CONST_SMALL_INT_MULTI_NUM; ++i) {
        vm_op_table[MP_BC_LOAD_CONST_SMALL_INT_MULTI + i] = op_load_const_small_int_multi;
    }
    for (i = 0; i < MP_BC_LOAD_FAST_MULTI_NUM; ++i) {
        vm_op_table[MP_BC_LOAD_FAST_MULTI + i] = op_load_fast_multi;
    }
    for (i = 0; i < MP_BC_STORE_FAST_MULTI_NUM; ++i) {
        vm_op_table[MP_BC_STORE_FAST_MULTI + i] = op_store_fast_multi;
    }
    for (i = 0; i < MP_BC_UNARY_OP_MULTI_NUM; ++i) {
        vm_op_table[MP_BC_UNARY_OP_MULTI + i] = op_unary_op_multi;
    }
    for (i = 0; i < MP_BC_BINARY_OP_MULTI_NUM; ++i) {
        vm_op_table[MP_BC_BINARY_OP_MULTI + i] = op_binary_op_multi;
    }
    vm_op_table_ready = 1;
}

// ---- exception handler (original: the nlr else-branch / exception_handler) ----

// Returns true to resume dispatching (a bytecode handler caught it), false to
// propagate the exception to the caller (state[0] holds it, like the original).
static bool vm_except(vm_ctx_t *c, mp_obj_t exc) {
    mp_code_state_t *cs = c->code_state;

    if (mp_obj_is_subclass_fast(MP_OBJ_FROM_PTR(((mp_obj_base_t *)MP_OBJ_TO_PTR(exc))->type), MP_OBJ_FROM_PTR(&mp_type_StopIteration))) {
        // check if it's a StopIteration within a for block
        if (*cs->ip == MP_BC_FOR_ITER) {
            c->ip = cs->ip + 1;
            DECODE_ULABEL; // the jump offset if iteration finishes
            cs->ip = c->ip + ulab; // jump to after for-block
            cs->sp -= MP_OBJ_ITER_BUF_NSLOTS; // pop the exhausted iterator
            return true; // continue with dispatch loop
        } else if (*cs->ip == MP_BC_YIELD_FROM) {
            // StopIteration inside yield from call means return a value of
            // yield from, so inject exception's value as yield from's result.
            *cs->sp = mp_obj_exception_get_value(exc);
            cs->ip++; // yield from is over, move to next instruction
            return true; // continue with dispatch loop
        }
    }

    // Set traceback info (file and line number) where the exception occurred,
    // but not for constant GeneratorExit or re-raises (see py/vm.c).
    if (MP_OBJ_TO_PTR(exc) != &mp_const_GeneratorExit_obj
        && *cs->ip != MP_BC_END_FINALLY
        && *cs->ip != MP_BC_RAISE_LAST) {
        const byte *ip = cs->fun_bc->bytecode;
        MP_BC_PRELUDE_SIG_DECODE(ip);
        MP_BC_PRELUDE_SIZE_DECODE(ip);
        const byte *line_info_top = ip + n_info;
        const byte *bytecode_start = ip + n_info + n_cell;
        size_t bc = cs->ip - bytecode_start;
        qstr block_name = mp_decode_uint_value(ip);
        for (size_t i = 0; i < 1 + n_pos_args + n_kwonly_args; ++i) {
            ip = mp_decode_uint_skip(ip);
        }
        #if MICROPY_EMIT_BYTECODE_USES_QSTR_TABLE
        block_name = cs->fun_bc->context->constants.qstr_table[block_name];
        qstr source_file = cs->fun_bc->context->constants.qstr_table[0];
        #else
        qstr source_file = cs->fun_bc->context->constants.source_file;
        #endif
        size_t source_line = mp_bytecode_get_source_line(ip, line_info_top, bc);
        mp_obj_exception_add_traceback(exc, source_file, source_line, block_name);
    }

    while (c->exc_sp >= c->exc_stack && c->exc_sp->handler <= cs->ip) {
        // nested exception: move up to previous exception handler
        POP_EXC_BLOCK();
    }

    if (c->exc_sp >= c->exc_stack) {
        // catch exception and pass to byte code
        cs->ip = c->exc_sp->handler;
        c->sp = MP_TAGPTR_PTR(c->exc_sp->val_sp);
        // save this exception in the stack so it can be used in a reraise, if needed
        c->exc_sp->prev_exc = (mp_obj_base_t *)MP_OBJ_TO_PTR(exc);
        // push exception object so it can be handled by bytecode
        PUSH(exc);
        cs->sp = c->sp;
        return true;
    }

    // propagate exception to higher level
    // Note: ip and sp don't have usable values at this point
    cs->state[0] = exc; // put exception here because sp is invalid
    return false;
}

// ---- dispatch loop -------------------------------------------------------------

// Runs bytecode until a handler signals anything other than "next opcode".
// Called inside the nlr_push region of vm_run but contains no setjmp itself,
// and holds no VM state in locals across handler calls -- everything is in *c.
static vm_status_t vm_dispatch(vm_ctx_t *c) {
    mp_code_state_t *cs = c->code_state;
    c->ip = cs->ip;
    c->sp = cs->sp;

    // If we have an exception to inject, raise it now (as if MP_BC_RAISE_OBJ
    // executed). Injecting into "yield from" is handled by MP_BC_YIELD_FROM.
    if (c->inject_exc != MP_OBJ_NULL && *c->ip != MP_BC_YIELD_FROM) {
        mp_obj_t exc = c->inject_exc;
        c->inject_exc = MP_OBJ_NULL;
        c->cur_exc = mp_make_raise_obj(exc);
        return VM_ST_RAISE;
    }

    for (;;) {
        // Mark the opcode being executed for the exception handler, then
        // advance past it (handlers use c->ip[-1] to recover the opcode).
        cs->ip = c->ip;
        VM_TRACE_HOOK('.', *c->ip);
        VM_TRACE_STACK(c->sp);
        c->opcode = *c->ip++;
        vm_status_t st = vm_op_table[c->opcode](c);
        if (st == VM_ST_DISPATCH) {
            continue;
        }
        if (st == VM_ST_DISPATCH_CHECK) {
            // We've just done a branch: convenient point to check for
            // pending exceptions (KeyboardInterrupt etc.).
            MICROPY_VM_HOOK_LOOP
            if (MP_STATE_THREAD(mp_pending_exception) != MP_OBJ_NULL) {
                mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);
            }
            continue;
        }
        return st;
    }
}

// The only function containing nlr_push (setjmp). Deliberately keeps NO
// mutable locals whose values must survive a longjmp: everything the VM
// mutates lives in *c (whose storage belongs to mp_execute_bytecode's frame,
// which this function never unwinds past).
static mp_vm_return_kind_t vm_run(vm_ctx_t *c) {
    for (;;) {
        nlr_buf_t nlr;
        mp_obj_t exc;
        if (nlr_push(&nlr) == 0) {
            vm_status_t st = vm_dispatch(c);
            if (st == VM_ST_RETURN) {
                return MP_VM_RETURN_NORMAL; // nlr popped by vm_unwind_return
            }
            if (st == VM_ST_YIELD) {
                return MP_VM_RETURN_YIELD; // nlr popped by vm_yield
            }
            if (st == VM_ST_EXC_RETURN) {
                return MP_VM_RETURN_EXCEPTION; // nlr popped by op_not_implemented
            }
            // VM_ST_RAISE: a handler raised c->cur_exc (original RAISE macro)
            nlr_pop();
            exc = c->cur_exc;
        } else {
            // exception occurred in a called function
            exc = MP_OBJ_FROM_PTR(nlr.ret_val);
        }
        if (!vm_except(c, exc)) {
            return MP_VM_RETURN_EXCEPTION;
        }
        // handler found: code_state->ip/sp updated; loop re-pushes nlr and
        // vm_dispatch reloads from code_state (original: goto outer_dispatch_loop)
    }
}

// fastn has items in reverse order (fastn[0] is local[0], fastn[-1] is local[1], etc)
// sp points to bottom of stack which grows up
// returns:
//  MP_VM_RETURN_NORMAL, sp valid, return value in *sp
//  MP_VM_RETURN_YIELD, ip, sp valid, yielded value in *sp
//  MP_VM_RETURN_EXCEPTION, exception in state[0]
mp_vm_return_kind_t mp_execute_bytecode(mp_code_state_t *code_state, volatile mp_obj_t inject_exc) {
    vm_ctx_t ctx;
    if (!vm_op_table_ready) {
        vm_op_table_init();
    }
    ctx.code_state = code_state;
    {
        size_t n_state = code_state->n_state;
        ctx.fastn = &code_state->state[n_state - 1];
        ctx.exc_stack = (mp_exc_stack_t *)(code_state->state + n_state);
    }
    ctx.exc_sp = MP_CODE_STATE_EXC_SP_IDX_TO_PTR(ctx.exc_stack, code_state->exc_sp_idx);
    #if MICROPY_EMIT_BYTECODE_USES_QSTR_TABLE
    ctx.qstr_table = code_state->fun_bc->context->constants.qstr_table;
    #endif
    ctx.inject_exc = inject_exc;
    ctx.cur_exc = MP_OBJ_NULL;
    ctx.ip = code_state->ip;
    ctx.sp = code_state->sp;
    return vm_run(&ctx);
}
