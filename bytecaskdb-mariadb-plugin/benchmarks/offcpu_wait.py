#!/usr/bin/env python3
"""Where the server's threads wait, weighted by how long they wait.

Reads `perf script --show-switch-events -F tid,time,event,ip,sym,dso` of a
capture's offcpu.data (lib_capture.sh records every 50th context switch with its
stack, and every switch in and out as a switch event) from stdin. Each sampled
switch-out is paired with the same thread's next switch-in, so a sample counts
for the time the thread stayed off the CPU, not once. Counting switches alone
ranks a site by how often threads block there, which says little about where
the time goes: a thousand 5 µs waits weigh less than one 50 ms wait.

A switch-out the kernel marks as a preemption is a thread that could have run
but had no CPU; it is reported apart from the sleeps.
"""
import collections
import re
import sys

HEADER = re.compile(r'^\s*(\d+)\s+(\d+\.\d+):\s+(.*?)\s*$')
FRAME = re.compile(r'^\s+[0-9a-f]+\s+(.*)\s+\((.*)\)\s*$')
PLUGIN = 'ha_bytecaskdb'
SITE_FRAMES = 4  # the leaf and its callers that name a site
TOP = 25


def short(sym, dso):
    if sym in ('[unknown]', ''):
        return f'[{dso.rsplit("/", 1)[-1]}]'
    sym = re.sub(r'\(.*', '', sym)  # drop the argument list
    sym = re.sub(r'@bytecask[\w.:]*', '', sym)  # module attachments
    return sym if len(sym) <= 70 else sym[:67] + '...'


def main():
    pending = {}   # tid -> (time, user frames) of a sampled switch-out
    preempted = set()
    by_site = collections.Counter()
    by_plugin = collections.Counter()
    by_tid = collections.Counter()
    plugin_time = 0.0
    total = 0.0
    first = last = None
    cur = None     # (tid, time, frames) of the sample being read

    def end_sample():
        nonlocal cur
        if cur:
            tid, t, frames = cur
            pending[tid] = (t, frames)
        cur = None

    for line in sys.stdin:
        m = HEADER.match(line)
        if m:
            end_sample()
            tid, t, event = int(m.group(1)), float(m.group(2)), m.group(3)
            first = t if first is None else first
            last = t
            if event.startswith(('context-switches', 'cs:', 'sched:sched_switch')):
                cur = (tid, t, [])
            elif event.startswith('PERF_RECORD_SWITCH OUT'):
                if 'preempt' in event and tid in pending:
                    preempted.add(tid)
            elif event.startswith('PERF_RECORD_SWITCH IN') and tid in pending:
                t0, frames = pending.pop(tid)
                d = t - t0
                if d < 0:
                    continue
                total += d
                by_tid[tid] += d
                if tid in preempted:
                    preempted.discard(tid)
                    by_site['(preempted: runnable, waiting for a CPU)'] += d
                    continue
                user = [(s, o) for s, o in frames if not o.startswith('[kernel')]
                names = []
                for s, o in user:
                    n = short(s, o)
                    if not names or names[-1] != n:
                        names.append(n)
                by_site[' < '.join(names[:SITE_FRAMES]) or '(kernel only)'] += d
                p = next((short(s, o) for s, o in user if PLUGIN in o), None)
                if p:
                    plugin_time += d
                    by_plugin[p] += d
            continue
        if cur:
            f = FRAME.match(line)
            if f:
                cur[2].append((f.group(1), f.group(2)))
    end_sample()

    if total == 0:
        print('No paired off-CPU samples: was offcpu.data recorded with --switch-events?')
        return
    span = (last - first) if first is not None else 0
    print(f'Off-CPU time of the sampled switches (every 50th), over a {span:.1f} s window.')
    print('Shares are of sampled off-CPU time; a site is the first user-space frame')
    print('and its callers, leaf first.')
    print()
    print(f'Stacks through {PLUGIN}.so: {100 * plugin_time / total:5.1f}% of the time')
    print()
    print('--- by site')
    for site, d in by_site.most_common(TOP):
        print(f'{100 * d / total:6.2f}%  {site}')
    print()
    print(f'--- by first {PLUGIN}.so frame (share of all off-CPU time)')
    for site, d in by_plugin.most_common(TOP):
        print(f'{100 * d / total:6.2f}%  {site}')
    print()
    print('--- by thread (sampled seconds off CPU)')
    for tid, d in by_tid.most_common(10):
        print(f'{d:9.3f} s  tid {tid}')


if __name__ == '__main__':
    main()
