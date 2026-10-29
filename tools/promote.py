"""Register promotion (gen.py --promote): the guest registers a C function uses live in C locals (host registers)
instead of the Cpu struct, written back only where other code can read them.

Unpromoted, every write to x0..x30, sp or the flags is a store to *c that the C compiler can't drop: guest memory
accesses may alias anything (cpu.h may_alias), and the caller may read any register. Promoted, a function loads the
registers it uses at entry, and stores back before a call or a tail call only the ones the target reads (its LI, live
in), and before a return the ones its callers read after it (its OUT); after a call it reloads the target's OUT.

LI and OUT come from a liveness analysis over every function of the module at once (a fixpoint: a function's LI
depends on its callees' LI, its OUT on what its callers read after calling it). Exact for direct calls and tail calls
between functions of the module, whatever convention they follow (the compiler's own helpers, functions split by
discovery whose pieces hand temporaries to each other). Where the target isn't known (an indirect call or branch, a
call to another module, an HLE function, a system call) the AArch64 calling convention is assumed: the target reads
the argument and callee-saved registers and returns those (ABI_IN, ABI_OUT), never the temporaries x9..x17 or the
flags. Functions with code that touches *c by itself (tools/patches.py replacements, setjmp calls, inline asm, the
all-instruction fallback) stay unpromoted: LI and OUT all registers.
"""
import re

SP, NZ = 31, 32
ALL = (1 << 33) - 1
X30 = 1 << 30


def regs(*ranges):
    m = 0
    for lo, hi in ranges:
        for r in range(lo, hi + 1):
            m |= 1 << r
    return m


# an unknown callee reads the arguments (x0..x7, x8 the indirect result), the platform register, the callee-saved
# ones it saves, the frame and link registers and sp; it returns results in x0..x7 and the callee-saved ones intact
ABI_IN = regs((0, 8), (18, 31))
ABI_OUT = regs((0, 7), (18, 29), (31, 31))
SCRATCH = regs((9, 17), (32, 32))  # (the temporaries and the flags: never arguments or results)
SVC_IN = regs((0, 8))  # (runtime/kernel.c: system calls take x0..x7 and return in x0..x7)
SVC_OUT = regs((0, 7))

_REG = re.compile(r'c->x\[(\d+)\](\s*=(?!=))?|c->(sp|nzcv)\b(\s*=(?!=))?')
_FLAG_SET = re.compile(r'\b(?:adds|subs|logic)(?:32|64)\(c\b')
_FLAG_RW = re.compile(r'\b(?:adcs|sbcs)(?:32|64)\(c\b')
_FLAG_USE = re.compile(r'\b(?:(?:adc|sbc)(?:32|64)\(c\b|COND_[A-Z]{2}\(c\)|FLAG_[NZCV]\(c\))')
_FLAG_FN = re.compile(r'\b((?:adds|subs|logic|adcs|sbcs|adc|sbc)(?:32|64)|COND_[A-Z]{2}|FLAG_[NZCV])\(c\b')


def scan(text):
    """(use, write, kill) masks of a piece of lifted C: the registers it reads, writes, and surely overwrites (not
    under an if)."""
    use = wr = 0
    for m in _REG.finditer(text):
        if m.group(1) is not None:
            bit, is_wr = 1 << int(m.group(1)), m.group(2)
        else:
            bit, is_wr = 1 << (SP if m.group(3) == 'sp' else NZ), m.group(4)
        if is_wr:
            wr |= bit
        else:
            use |= bit
    if _FLAG_SET.search(text):
        wr |= 1 << NZ
    if _FLAG_RW.search(text):
        use |= 1 << NZ
        wr |= 1 << NZ
    if _FLAG_USE.search(text):
        use |= 1 << NZ
    return use, wr, (0 if 'if (' in text else wr)


def subst(text):
    """The same C reading and writing the locals."""
    text = _REG.sub(lambda m: (f'x{m.group(1)}_' if m.group(1) is not None else
                               'sp_' if m.group(3) == 'sp' else 'nz_->nzcv') + (m.group(2) or m.group(4) or ''), text)
    return _FLAG_FN.sub(lambda m: f'{m.group(1)}(nz_', text)


def _lval(r):
    return 'c->sp' if r == SP else 'c->nzcv' if r == NZ else f'c->x[{r}]'


def _local(r):
    return 'sp_' if r == SP else 'nz_->nzcv' if r == NZ else f'x{r}_'


def store(mask):
    """C storing the locals in mask to *c."""
    return ''.join(f'{_lval(r)} = {_local(r)}; ' for r in range(33) if mask >> r & 1)


def load(mask):
    """C loading the locals in mask from *c."""
    return ''.join(f'{_local(r)} = {_lval(r)}; ' for r in range(33) if mask >> r & 1)


def declare(mask):
    """The locals' declarations, loaded from *c."""
    out = []
    xs = [r for r in range(32) if mask >> r & 1]
    if xs:
        out.append('uint64_t ' + ', '.join(f'{_local(r)} = {_lval(r)}' for r in xs) + ';')
    if mask >> NZ & 1:
        out.append('struct { uint64_t nzcv; } nzl_ = {c->nzcv}, *const nz_ = &nzl_;')
    return ' '.join(out)


class Fn:
    """One C function's instructions for the analysis (build)."""
    __slots__ = ('entry', 'end', 'n', 'use', 'kill', 'succ', 'bnd', 'calls', 'tails', 'wr', 'ref')


def build(m, entry, end, tables):
    """The analysis view of a promotable function, or None when it must stay unpromoted. Per instruction: use and
    kill masks, internal successors, and a boundary: None, ('ret',), ('tail', target), ('mask', live) (an unknown
    target), ('call', target) or ('ucall', in_mask, out_mask) (calls fall through to the next instruction)."""
    if tables and 'all' in tables.values():
        return None
    f = Fn()
    f.entry, f.end = entry, end
    n = (end - entry) // 4
    f.n = n
    f.use, f.kill, f.succ, f.bnd = [0] * n, [0] * n, [()] * n, [None] * n
    f.calls, f.tails = set(), set()
    wr = ref = 0
    for i in range(n):
        a = entry + 4 * i
        if a in m.patches:
            return None
        ins = m.lift(a)
        k = ins.kind
        nxt = (i + 1,) if i + 1 < n else ()
        fall_out = i + 1 == n  # (falls out of the function: the next one's tail call)

        def edge(t):
            if entry <= t < end:
                return (t - entry) // 4, None
            if m.in_text(t):
                f.tails.add(t)
                return None, ('tail', t)
            return None, ('mask', ABI_IN)

        if k == 'asm':
            return None
        if k in ('op', 'trap'):
            u, w, kl = scan(ins.code)
            if k == 'trap' or 'rt_udf(' in ins.code:  # (it stops the game: nothing reads the registers after)
                f.use[i], f.bnd[i] = u, ('mask', 0)
            elif 'rt_svc(' in ins.code:
                if fall_out:
                    return None
                f.use[i], f.kill[i], f.succ[i], f.bnd[i] = u, 0, nxt, ('ucall', SVC_IN, SVC_OUT)
            else:
                f.use[i], f.kill[i], f.succ[i] = u, kl, nxt
                if fall_out:
                    f.bnd[i] = ('tail', end) if m.in_text(end) else ('mask', 0)
                    if m.in_text(end):
                        f.tails.add(end)
            wr |= w
            ref |= u | w
        elif k in ('b', 'bcond'):
            u = scan(ins.cond)[0] if k == 'bcond' else 0
            si, b = edge(ins.target)
            succ = ([si] if si is not None else []) + (list(nxt) if k == 'bcond' else [])
            f.use[i], f.succ[i], f.bnd[i] = u, tuple(succ), b
            if k == 'bcond' and fall_out:
                if f.bnd[i] is not None:  # (both edges leave the function: a second boundary)
                    return None
                f.bnd[i] = ('tail', end) if m.in_text(end) else ('mask', 0)
                if m.in_text(end):
                    f.tails.add(end)
            ref |= u
        elif k == 'bl':
            t = ins.target
            if t in m.setjmps:
                return None
            if m.in_text(t):
                f.calls.add(t)
                f.bnd[i] = ('call', t)
            else:
                f.bnd[i] = ('ucall', ABI_IN, ABI_OUT)
            f.succ[i] = nxt
            wr |= X30
            ref |= X30
            if fall_out:
                return None  # (a call as the last instruction: leave such odd code alone)
        elif k == 'blr':
            u = scan(ins.target)[0]
            f.use[i], f.succ[i], f.bnd[i] = u, nxt, ('ucall', ABI_IN, ABI_OUT)
            wr |= X30
            ref |= u | X30
            if fall_out:
                return None
        elif k == 'br':
            u = scan(ins.target)[0]
            jt = tables.get(a)
            succ, outs = [], []
            if jt:
                for t in jt:
                    si, b = edge(t)
                    if si is not None:
                        succ.append(si)
                    else:
                        outs.append(b)
                # (a table miss, which the emulated tables never have: <name>_r resumes from *c, at an instruction the
                # analysis can't know. Everything this function wrote is stored; what it didn't write is taken as
                # the convention has it: arguments and callee-saved registers)
                outs.append(('mask', ABI_IN))
            else:
                outs.append(('mask', ABI_IN))
            f.use[i], f.succ[i] = u, tuple(succ)
            f.bnd[i] = outs[0] if len(outs) == 1 else ('multi', outs)
            ref |= u
        elif k == 'ret':
            f.bnd[i] = ('ret',)
        else:
            return None
    f.wr, f.ref = wr, ref
    return f


def analyze(m, entries, hle, log):
    """LI and OUT of every function (address -> mask), and the promotable functions (address -> Fn)."""
    fns = {}
    LI, OUT = {}, {}
    # The functions as compiled (an eh_frame entry starts there) follow the calling convention: a call or a tail call
    # to one never hands it temporaries or flags, nor takes them back (what it reads of them on entry is whatever
    # was there: compiled code reads such leftovers, a bfi into a fresh register, and without this one leftover
    # read made temporaries live through most of the module). The pieces discovery split functions into, and code
    # without unwind info, get the exact analysis.
    abi = {a for a in entries if (m.fde(a) or (None,))[0] == a}
    callers = {}  # target -> functions that call it or tail-call it (to redo when its LI grows)
    tail_to = {}  # function -> its tail-call targets (OUT flows along them)
    for a, (end, labels, tables) in entries.items():
        if a in hle:
            LI[a], OUT[a] = ABI_IN, ABI_OUT
            continue
        f = build(m, a, end, tables)
        if f is None:
            LI[a] = OUT[a] = ALL
            # (its callees and tail-call targets may be read anything after: they store everything back)
            for x in range(a, end, 4):
                ins = m.lift(x)
                if ins.kind == 'bl' and m.in_text(ins.target):
                    OUT[ins.target] = ALL
                elif ins.kind in ('b', 'bcond') and m.in_text(ins.target) and not a <= ins.target < end:
                    OUT[ins.target] = ALL
            if m.in_text(end):
                OUT[end] = ALL
            for jt in tables.values():
                if jt != 'all':
                    for t in jt:
                        if not a <= t < end:
                            OUT[t] = ALL
            for t in [t for t in OUT if t in abi and OUT[t] == ALL]:
                OUT[t] = ALL & ~SCRATCH
            continue
        fns[a] = f
        LI.setdefault(a, 0)
        OUT[a] = OUT.get(a, 0) | ABI_OUT
        for t in f.calls | f.tails:
            callers.setdefault(t, set()).add(a)
        tail_to[a] = f.tails
    for t in list(OUT):
        LI.setdefault(t, ALL)

    def bmask(b, lo):
        kind = b[0]
        if kind == 'ret':
            return None  # (the function's OUT)
        if kind == 'tail':
            return li_of(b[1])
        if kind == 'mask':
            return b[1]
        if kind == 'multi':
            r = 0
            for x in b[1]:
                r |= bmask(x, lo)
            return r
        raise AssertionError(b)

    def li_of(t):  # (what a call or a tail call to t hands it)
        return LI.get(t, ALL) & ~SCRATCH if t in abi else LI.get(t, ALL)

    def requeue(t):
        if t in fns and t not in queued:
            work.append(t)
            queued.add(t)

    def liveness(f, out_f):
        """Each instruction's live-in; the calls' live-after go into their targets' OUT."""
        live = [0] * f.n
        changed = True
        while changed:
            changed = False
            for i in range(f.n - 1, -1, -1):
                lo = 0
                for s in f.succ[i]:
                    lo |= live[s]
                b = f.bnd[i]
                if b is not None:
                    k = b[0]
                    if k == 'call':
                        t = b[1]
                        want = lo & ~SCRATCH if t in abi else lo
                        if want & ~OUT.get(t, ALL):
                            OUT[t] = OUT.get(t, 0) | want
                            requeue(t)
                        v = f.use[i] | ((li_of(t) | (lo & ~OUT.get(t, ALL))) & ~X30)
                    elif k == 'ucall':
                        v = f.use[i] | ((b[1] | (lo & ~b[2])) & ~X30)
                    else:
                        bm = bmask(b, lo)
                        lo |= out_f if bm is None else bm
                        v = f.use[i] | (lo & ~f.kill[i])
                else:
                    v = f.use[i] | (lo & ~f.kill[i])
                if v != live[i]:
                    live[i] = v
                    changed = True
        return live

    work = sorted(fns, reverse=True)
    queued = set(work)
    rounds = 0
    while work:
        a = work.pop()
        queued.discard(a)
        rounds += 1
        f = fns[a]
        out_f = OUT[a]
        live = liveness(f, out_f)
        li = live[0] if f.n else 0
        if li != LI[a]:
            LI[a] = li
            for c in callers.get(a, ()):
                requeue(c)
        for t in tail_to[a]:
            want = out_f & ~SCRATCH if t in abi else out_f
            if want & ~OUT.get(t, 0):
                OUT[t] = OUT.get(t, 0) | want
                requeue(t)
    n_li = sum(1 for a in fns if li_of(a) & SCRATCH)
    m.promo_leftover = [a for a in fns if a in abi and LI[a] & SCRATCH]  # (compiled functions reading leftovers)
    n_out = sum(1 for a in fns if OUT[a] & SCRATCH)
    log(f'promote: {len(fns)} of {len(entries)} functions promoted ({len(abi)} compiled functions), {rounds} '
        f'analyses; temporaries or flags in: {n_li}, out: {n_out}')
    return fns, {a: li_of(a) for a in LI}, OUT
