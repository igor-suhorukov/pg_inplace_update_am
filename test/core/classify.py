#!/usr/bin/env python3
"""
Classify the differences of a core-suite run (test/run.sh core) against the
heap expected output, to help keep test/core/expected_failures honest:

  order   removed and added lines are the same multiset (row order only)
  plan    every changed line belongs to an EXPLAIN plan
  other   anything else (look at it)

usage: classify.py out/core/regress/regression.diffs
"""
import re
import sys
from collections import Counter

PLAN = re.compile(r'(Scan|->|QUERY PLAN|Sort|Join|Heap Fetches|Recheck Cond|'
                  r'Filter|Index Cond|Aggregate|Gather|Hash|Workers|Append|'
                  r'Materialize|Memoize|Limit|Result|Nested Loop|Merge|Group|'
                  r'Unique|SetOp|Subquery|InitPlan|SubPlan|Output|Rows Removed|'
                  r'Bitmap|BitmapOr|BitmapAnd|One-Time|Partial|Finalize|^[-+]\s*-+$|'
                  r'\(\d+ rows?\)|Presorted Key|Sort Key|Group Key|Cache Key|'
                  r'Order By|Join Filter|Hash Cond|Merge Cond|TID Cond)')


def main(path):
    text = open(path).read()
    for part in re.split(r'(?m)^diff -U3 ', text)[1:]:
        m = re.search(r'expected/(\S+)\.out', part)
        name = m.group(1) if m else '?'
        body = part.split('\n')[3:]
        minus = [l[1:] for l in body if l.startswith('-')]
        plus = [l[1:] for l in body if l.startswith('+')]
        if Counter(minus) == Counter(plus):
            kind = 'order'
        elif all(PLAN.search(l) for l in minus + plus):
            kind = 'plan'
        else:
            kind = 'other'
        print(f'{name:24s} {kind:6s} -{len(minus):<5d} +{len(plus):<5d}')


if __name__ == '__main__':
    main(sys.argv[1])
