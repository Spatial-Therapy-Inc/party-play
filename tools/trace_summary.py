#!/usr/bin/env python3
"""Sums up a `perf trace` from the profiling build (sdkconfig.profile): per function, how often, its total time and
its own time (without what it called), and the frames' times. Reads a console log (stdin or files); each
PERF_TRACE_BEGIN ... PERF_TRACE_END block is one report.

  trace_summary.py log.txt [--top 30]
"""
import re
import sys

LINE = re.compile(r'LVGL-(\d+) \[(\d+)\] (\d+)\.(\d+): tracing_mark_write: ([BE])\|\d+\|(.+)$')


def report(title, events, top):
    stacks = {}  # thread -> [(func, start, child time)]
    total, own, count = {}, {}, {}
    frames = []
    for tid, t, tag, func in events:
        st = stacks.setdefault(tid, [])
        if tag == 'B':
            st.append([func, t, 0.0])
            continue
        # an end closes the innermost open call of that name (unmatched ends are dropped)
        for i in range(len(st) - 1, -1, -1):
            if st[i][0] == func:
                f, start, child = st.pop(i)
                del st[i:]
                d = t - start
                total[f] = total.get(f, 0) + d
                own[f] = own.get(f, 0) + d - child
                count[f] = count.get(f, 0) + 1
                if st:
                    st[-1][2] += d
                if f in ('lv_display_refr_timer', '_lv_display_refr_timer', 'refr_invalid_areas'):
                    frames.append((f, d))
                break
    print(f'== {title}')
    refr = [d for f, d in frames if f in ('lv_display_refr_timer', '_lv_display_refr_timer')]
    if refr:
        print(f'frames {len(refr)}: ' + ' '.join(f'{d * 1000:.1f}' for d in refr) + ' ms')
    span = (events[-1][1] - events[0][1]) if events else 0
    print(f'traced {span * 1000:.1f} ms, {len(events)} events')
    print(f'{"own ms":>9} {"total ms":>9} {"calls":>7}  function')
    for f in sorted(own, key=lambda k: -own[k])[:top]:
        print(f'{own[f] * 1000:9.2f} {total[f] * 1000:9.2f} {count[f]:7d}  {f}')
    print()


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 30
    if '--top' in sys.argv:
        args.remove(str(top))
    src = (open(a, encoding='utf-8', errors='replace') for a in args) if args else [sys.stdin]
    title, events = None, []
    last = -1.0  # the profiler's buffer is not emptied by a flush: each block repeats the ones before it
    for f in src:
        for raw in f:
            if 'PERF_TRACE_BEGIN' in raw:
                title, events = raw.split('PERF_TRACE_BEGIN', 1)[1].strip(), []
            elif 'PERF_TRACE_END' in raw and title is not None:
                fresh = [e for e in events if e[1] > last]
                if events:
                    last = max(e[1] for e in events)
                report(title, fresh, top)
                title = None
            elif title is not None:
                m = LINE.search(raw.strip())
                if m:
                    tid, _, sec, frac, tag, func = m.groups()
                    events.append((tid, int(sec) + int(frac) / 10 ** len(frac), tag, func.strip()))


if __name__ == '__main__':
    main()
