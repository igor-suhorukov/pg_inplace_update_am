#!/usr/bin/env python3
#
# A small model of iheap's rollback and discard, checked over every
# interleaving of its atomic steps.
#
# The model: two rows on two data pages; two writers, each updating rows
# (rolling back aborted versions first, waiting for running ones) and then
# committing or aborting; the aborting backend's own rollback, one undo
# record at a time, after the abort is visible, or none at all
# (iheap.foreground_undo_limit); a reader that takes a snapshot and reads
# both rows, walking undo under the data page's share lock with the discard
# pointer it read when it started; discard, which computes its horizon,
# passes the undo pages whose writer is below it, applies the aborted
# records it finds there (each under its data page's exclusive lock), and
# then moves its pointer and frees the pages; undo pages of one record,
# which writers take over, freed, under new logical numbers; and a crash,
# after which running transactions count as aborted and no state outside
# the pages survives.  An atomic step is what the code does under one lock.
#
# The checks: the reader sees what its snapshot must see; rollback never
# misses a record it needs; no row shows an aborted version below the
# discard pointer, which readers take for frozen; and every state reached
# can still be read correctly by a new snapshot.  The search must find no
# violation in the model, and must find one in each of the mutants below,
# protocol rules taken out one at a time, or the model would check nothing.
#
#   rollback.py            the model and its mutants
#   rollback.py MUTANT     one of them, with the trace of what it breaks

import sys
from collections import deque

ROWS = 2                        # row i lives on data page i
PLANS = ((0, 0), (1, 0))        # the rows each writer updates, in order
MAX_SLOTS = 4                   # physical undo pages

RUNNING, COMMITTED, ABORTED = 'r', 'c', 'a'

MUTANTS = {
    # discard moves its pointer past aborted records without applying them
    'discard-skips-apply',
    # the aborting backend restores its records' old versions without
    # checking that the row still shows its own aborted version
    'eager-blind',
    # the reader walks undo without the data page's share lock
    'reader-unlocked',
    # writers write on top of an aborted version instead of rolling it back
    'write-over-aborted',
    # discard's horizon ignores the reader's snapshot
    'horizon-ignores-snapshots',
}


class Violation(Exception):
    pass


# --------------------------------------------------------------------------
# State
#
# rows:    per row (xid, urec, val): its current version, urec the logical
#          undo page of the record that made it (None: the initial version)
# slots:   per physical undo page None, or (lpage, live, rec), rec being
#          (xid, row, prev) and prev the version it replaced
# meta:    (discard pointer, next logical page)
# clog:    per xid (1, 2) the status, None before the writer starts
# writers: per writer (phase, xid, k, own): phase new/run/eager/done, k the
#          next update of its plan, own its records' pages, newest last
# reader:  (phase, snapshot, row, dlocal, cur, pagelock): phase new, snap,
#          walk, done; snapshot (running xids, xmax)
# disc:    (phase, horizon, stop, lpage, targets): phase idle, scan, apply
# ghost:   per row the versions written, (xid, val), in order (for checks)
# misc:    (next xid, crashed)


def initial():
    rows = tuple((0, None, 100 + i) for i in range(ROWS))
    slots = (None,) * MAX_SLOTS
    meta = (0, 0)
    clog = (None, None)
    writers = tuple(('new', 0, 0, ()) for _ in PLANS)
    reader = ('new', None, 0, None, None, None)
    disc = ('idle', None, None, None, ())
    ghost = tuple(((0, 100 + i),) for i in range(ROWS))
    misc = (1, False)
    return (rows, slots, meta, clog, writers, reader, disc, ghost, misc)


def status(clog, xid):
    if xid == 0:
        return COMMITTED
    return clog[xid - 1]


def fetch(slots, lpage, mutant):
    """The record on logical page lpage, or None if it is gone (its page
    freed, or taken over by another logical page)."""
    for s in slots:
        if s is not None and s[1] and s[0] == lpage:
            return s[2]
    return None


def rollback_row(rows, slots, meta, clog, row, mutant):
    """iheap_rollback_tuple(): restore versions while the row's current
    version is an aborted one above the discard pointer."""
    rows = list(rows)
    while True:
        xid, urec, val = rows[row]
        if urec is None or urec < meta[0]:
            break
        if status(clog, xid) != ABORTED:
            break
        rec = fetch(slots, urec, mutant)
        if rec is None:
            raise Violation('rollback of row %d misses record %d' % (row, urec))
        rows[row] = rec[2]
    return tuple(rows)


def visible(clog, snap, xid):
    running, xmax = snap
    return xid == 0 or (status(clog, xid) == COMMITTED and
                        xid not in running and xid < xmax)


def oracle(clog, ghost, snap, row):
    """What a snapshot must see of a row: its last committed version that
    the snapshot sees."""
    for xid, val in reversed(ghost[row]):
        if visible(clog, snap, xid):
            return val
    raise AssertionError('no visible version')


def read_now(state, row, mutant):
    """A reader that took its snapshot now and reads row without anybody
    in between: the result must be the oracle's (a state that new readers
    misread is wrong even if no reader of the search was there)."""
    rows, slots, meta, clog, writers, reader, disc, ghost, misc = state
    running = frozenset(i + 1 for i, s in enumerate(clog) if s == RUNNING)
    snap = (running, misc[0])
    xid, urec, val = rows[row]
    dlocal = meta[0]
    while True:
        if urec is None or urec < dlocal or visible(clog, snap, xid):
            return val, oracle(clog, ghost, snap, row)
        rec = fetch(slots, urec, mutant)
        if rec is None:
            return val, oracle(clog, ghost, snap, row)
        xid, urec, val = rec[2]


def check(state, mutant):
    rows, slots, meta, clog, writers, reader, disc, ghost, misc = state
    for row in range(ROWS):
        xid, urec, val = rows[row]
        if (urec is not None and urec < meta[0] and
                status(clog, xid) == ABORTED):
            raise Violation('row %d shows an aborted version below the '
                            'discard pointer' % row)
        got, want = read_now(state, row, mutant)
        if got != want:
            raise Violation('a new snapshot reads %d from row %d, not %d' %
                            (got, row, want))


# --------------------------------------------------------------------------
# Steps: each returns a list of (label, next state)

def page_locked(reader, row, mutant=None):
    """Does the reader hold the share lock of row's data page?"""
    if mutant == 'reader-unlocked':
        return False
    return reader[0] == 'walk' and reader[5] == row


def writer_steps(state, w, mutant):
    rows, slots, meta, clog, writers, reader, disc, ghost, misc = state
    phase, xid, k, own = writers[w]
    out = []
    plan = PLANS[w]

    def with_writer(st, wstate):
        ws = list(st[4])
        ws[w] = wstate
        return st[:4] + (tuple(ws),) + st[5:]

    if phase == 'new':
        nxid, crashed = misc
        if crashed:
            return out
        cl = list(clog)
        cl[nxid - 1] = RUNNING
        st = (rows, slots, meta, tuple(cl), writers, reader, disc, ghost,
              (nxid + 1, crashed))
        out.append(('w%d begins as xid %d' % (w, nxid),
                    with_writer(st, ('run', nxid, 0, ()))))
    elif phase == 'run' and k < len(plan):
        row = plan[k]
        if page_locked(reader, row, mutant):
            return out
        cxid, curec, cval = rows[row]
        st_cur = status(clog, cxid)
        if st_cur == ABORTED and curec is not None and curec >= meta[0] \
                and mutant != 'write-over-aborted':
            # roll it back first (a step of its own: the lock is held
            # throughout in the code, which this does not depend on)
            nrows = rollback_row(rows, slots, meta, clog, row, mutant)
            out.append(('w%d rolls back row %d' % (w, row),
                        (nrows,) + state[1:]))
            return out
        if st_cur == RUNNING and cxid != xid:
            return out          # waits for that writer
        # take a new undo page: a free one or a new one
        lpage = meta[1]
        free = [i for i, s in enumerate(slots) if s is None or not s[1]]
        if not free:
            return out          # (the code extends the relation)
        prev = rows[row]
        rec = (xid, row, prev)
        val = 10 * (w + 1) + k
        for i in free[:1]:
            ns = list(slots)
            ns[i] = (lpage, True, rec)
            nrows = list(rows)
            nrows[row] = (xid, lpage, val)
            ng = list(ghost)
            ng[row] = ghost[row] + ((xid, val),)
            st = (tuple(nrows), tuple(ns), (meta[0], lpage + 1), clog,
                  writers, reader, disc, tuple(ng), misc)
            out.append(('w%d updates row %d to %d (undo page %d in slot %d)' %
                        (w, row, val, lpage, i),
                        with_writer(st, ('run', xid, k + 1, own + (lpage,)))))
    elif phase == 'run':
        cl = list(clog)
        cl[xid - 1] = COMMITTED
        st = state[:3] + (tuple(cl),) + state[4:]
        out.append(('w%d commits' % w, with_writer(st, ('done', xid, k, own))))
        cl[xid - 1] = ABORTED
        st = state[:3] + (tuple(cl),) + state[4:]
        out.append(('w%d aborts' % w, with_writer(st, ('eager', xid, k, own))))
        out.append(('w%d aborts, its undo left to others' % w,
                    with_writer(st, ('done', xid, k, own))))
    elif phase == 'eager':
        if not own:
            out.append(('w%d is done rolling back' % w,
                        with_writer(state, ('done', xid, k, own))))
            return out
        lpage = own[-1]
        rec = fetch(slots, lpage, mutant)
        if rec is None:
            out.append(('w%d finds undo page %d gone, stops' % (w, lpage),
                        with_writer(state, ('done', xid, k, own))))
            return out
        row = rec[1]
        if page_locked(reader, row, mutant):
            return out
        if mutant == 'eager-blind':
            nrows = list(rows)
            nrows[row] = rec[2]
            nrows = tuple(nrows)
        else:
            nrows = rollback_row(rows, slots, meta, clog, row, mutant)
        st = (nrows,) + state[1:]
        out.append(('w%d applies undo page %d to row %d' % (w, lpage, row),
                    with_writer(st, ('eager', xid, k, own[:-1]))))
    return out


def reader_steps(state, mutant):
    rows, slots, meta, clog, writers, reader, disc, ghost, misc = state
    phase, snap, row, dlocal, cur, lock = reader
    out = []
    if phase == 'new':
        if misc[1]:
            return out
        running = frozenset(i + 1 for i, s in enumerate(clog) if s == RUNNING)
        snap = (running, misc[0])
        out.append(('reader takes a snapshot (running %s, xmax %d)' %
                    (sorted(running), misc[0]),
                    state[:5] + (('snap', snap, 0, None, None, None),) +
                    state[6:]))
    elif phase == 'snap' and row < ROWS:
        out.append(('reader locks page %d, reads row %d %s, discard at %d' %
                    (row, row, rows[row], meta[0]),
                    state[:5] + (('walk', snap, row, meta[0], rows[row], row),) +
                    state[6:]))
    elif phase == 'snap':
        out.append(('reader ends', state[:5] +
                    (('done', None, row, None, None, None),) + state[6:]))
    elif phase == 'walk':
        xid, urec, val = cur
        result = None
        if urec is None or urec < dlocal or visible(clog, snap, xid):
            result = val
        else:
            rec = fetch(slots, urec, mutant)
            if rec is None:
                result = val    # discarded under us: visible to all
            else:
                out.append(('reader steps back from %s to %s' %
                            (cur, rec[2]),
                            state[:5] + (('walk', snap, row, dlocal, rec[2],
                                          lock),) + state[6:]))
                return out
        want = oracle(clog, ghost, snap, row)
        if result != want:
            raise Violation('the reader sees %d in row %d, its snapshot %d' %
                            (result, row, want))
        out.append(('reader sees %d in row %d' % (result, row),
                    state[:5] + (('snap', snap, row + 1, None, None, None),) +
                    state[6:]))
    return out


def disc_steps(state, mutant):
    rows, slots, meta, clog, writers, reader, disc, ghost, misc = state
    phase, horizon, stop, lpage, targets = disc
    out = []

    def with_disc(st, d):
        return st[:6] + (d,) + st[7:]

    if phase == 'idle':
        xids = [i + 1 for i, s in enumerate(clog) if s == RUNNING]
        if reader[0] in ('snap', 'walk') and \
                mutant != 'horizon-ignores-snapshots':
            running, xmax = reader[1]
            xids.append(min(list(running) + [xmax]))
        horizon = min(xids + [misc[0]])
        if meta[0] < meta[1]:
            out.append(('discard starts, horizon %d' % horizon,
                        with_disc(state, ('scan', horizon, meta[1], meta[0],
                                          ()))))
    elif phase == 'scan':
        if lpage >= stop:
            out.append(('discard reaches its end', with_disc(
                state, ('advance', horizon, lpage, lpage, ()))))
            return out
        rec = fetch(slots, lpage, None)
        if rec is not None and rec[0] >= horizon:
            out.append(('discard stops at undo page %d (xid %d)' %
                        (lpage, rec[0]),
                        with_disc(state, ('advance', horizon, lpage, lpage,
                                          ()))))
            return out
        tg = ()
        if rec is not None and status(clog, rec[0]) == ABORTED and \
                mutant != 'discard-skips-apply':
            tg = (rec[1],)
        out.append(('discard passes undo page %d, to apply %s' % (lpage, tg),
                    with_disc(state, ('apply', horizon, stop, lpage, tg))))
    elif phase == 'apply':
        if not targets:
            out.append(('discard is done with undo page %d' % lpage,
                        with_disc(state, ('scan', horizon, stop, lpage + 1,
                                          ()))))
            return out
        row = targets[0]
        if page_locked(reader, row, mutant):
            return out
        nrows = rollback_row(rows, slots, meta, clog, row, mutant)
        out.append(('discard rolls back row %d' % row,
                    with_disc((nrows,) + state[1:],
                              ('apply', horizon, stop, lpage, targets[1:]))))
    elif phase == 'advance':
        end = lpage
        ns = tuple(None if s is None else
                   (s[0], s[1] and s[0] >= end, s[2]) for s in slots)
        st = (rows, ns, (max(meta[0], end), meta[1])) + state[3:]
        out.append(('discard moves its pointer to %d' % end,
                    with_disc(st, ('idle', None, None, None, ()))))
    return out


def crash_steps(state):
    rows, slots, meta, clog, writers, reader, disc, ghost, misc = state
    if misc[1]:
        return []
    cl = tuple(ABORTED if s == RUNNING else s for s in clog)
    ws = tuple(('done', xid, k, own) if phase in ('run', 'eager')
               else (phase, xid, k, own)
               for phase, xid, k, own in writers)
    rd = reader if reader[0] in ('new', 'done') else \
        ('done', None, reader[2], None, None, None)
    st = (rows, slots, meta, cl, ws, rd, ('idle', None, None, None, ()),
          ghost, (misc[0], True))
    return [('crash', st)]


def successors(state, mutant):
    out = []
    for w in range(len(PLANS)):
        out += writer_steps(state, w, mutant)
    out += reader_steps(state, mutant)
    out += disc_steps(state, mutant)
    out += crash_steps(state)
    return out


# --------------------------------------------------------------------------

def search(mutant):
    """Breadth-first over every state; returns (states, violation trace)."""
    start = initial()
    parent = {start: None}
    queue = deque([start])
    while queue:
        state = queue.popleft()
        try:
            check(state, mutant)
            succ = successors(state, mutant)
        except Violation as v:
            trace = []
            s = state
            while parent[s] is not None:
                s, label = parent[s]
                trace.append(label)
            trace.reverse()
            return len(parent), (str(v), trace)
        for label, nxt in succ:
            if nxt not in parent:
                parent[nxt] = (state, label)
                queue.append(nxt)
    return len(parent), None


def main():
    if len(sys.argv) > 1:
        mutant = sys.argv[1]
        if mutant not in MUTANTS:
            sys.exit('unknown mutant %s (%s)' % (mutant, ', '.join(sorted(MUTANTS))))
        n, found = search(mutant)
        print('%s: %d states' % (mutant, n))
        if found:
            print('violation: %s' % found[0])
            for i, label in enumerate(found[1], 1):
                print('  %2d. %s' % (i, label))
        return
    ok = True
    n, found = search(None)
    if found:
        ok = False
        print('model: VIOLATION after %d states: %s' % (n, found[0]))
        for i, label in enumerate(found[1], 1):
            print('  %2d. %s' % (i, label))
    else:
        print('model: %d states, no violation' % n)
    for mutant in sorted(MUTANTS):
        n, found = search(mutant)
        if found:
            print('mutant %s: caught after %d states: %s' % (mutant, n, found[0]))
        else:
            ok = False
            print('mutant %s: NOT caught (%d states)' % (mutant, n))
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
