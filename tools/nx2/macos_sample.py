#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

"""Where a running lichii spends its time on macOS, per thread.

Samples the process with /usr/bin/sample and prints, for each thread, how busy it was and the
functions it was in most. A thread near 100 % is what the frame waits for.

usage: python3 -I tools/nx2/macos_sample.py [--seconds 10] [--pid PID] [--top 6] [--waits 3] [--keep FILE]
"""

import argparse
import collections
import re
import subprocess
import sys
import tempfile

# Leaves that mean the thread is waiting, not working.
IDLE = {
    "mach_msg2_trap", "mach_msg_trap", "__psynch_cvwait", "__psynch_mutexwait",
    "semaphore_wait_trap", "semaphore_timedwait_trap", "semaphore_wait_signal_trap", "__ulock_wait", "__ulock_wait2",
    "kevent", "kevent64", "kevent_id", "__select", "__semwait_signal", "nanosleep", "poll",
    "read", "__workq_kernreturn", "__sigwait", "start_wqthread", "__accept", "__recvfrom",
}
# Leaves that are the guest's own code.
GUEST = {"hv_vcpu_run", "hv_trap", "???"}

LINE = re.compile(r"^([ +!:|]*)(\d+) (.*)$")
OURS = re.compile(r"^(Vulkan|VideoCommon|VideoCore|Tegra|Core|Kernel|Service|AudioCore|Common|"
                  r"FileSys|MVK|mvk|vk[A-Z]|-\[MVK|-\[MTL|AGX)")
THREAD = re.compile(r"^Thread_\w+(?::?\s+(.*))?$")


def symbol(text):
    name = text.split("  (in ")[0].strip()
    return re.sub(r"\s+\[0x[0-9a-f,]+\]$", "", name)


def parse(report):
    """Returns [(thread name, total samples, {leaf: samples}, {waiting function: samples})]."""
    threads = []
    lines = report.splitlines()
    try:
        start = next(i for i, line in enumerate(lines) if line.startswith("Call graph:")) + 1
    except StopIteration:
        return threads
    stack = []  # (depth, count, children total, symbol)
    leaves = waits = None

    def close(depth):
        while stack and stack[-1][0] >= depth:
            _, count, children, name = stack.pop()
            if count > children:
                leaves[name] += count - children
                if name in IDLE:
                    # The emulator's function that waits, not the library's primitive.
                    waiter = next((frame[3] for frame in reversed(stack) if OURS.match(frame[3])),
                                  name)
                    waits[waiter] += count - children

    for line in lines[start:]:
        match = LINE.match(line)
        if not match:
            if stack:
                close(0)
            if line.strip() == "" or line.startswith("Total number"):
                if line.startswith("Total number"):
                    break
            continue
        depth, count, text = len(match.group(1)), int(match.group(2)), match.group(3)
        thread = THREAD.match(text.split("   ")[0].strip()) if depth <= 4 else None
        if thread and text.startswith("Thread_"):
            if stack:
                close(0)
            leaves, waits = collections.Counter(), collections.Counter()
            name = (thread.group(1) or "").strip() or text.split()[0]
            name = re.sub(r"^DispatchQueue_\d+:\s*", "", name)
            threads.append((name, count, leaves, waits))
            continue
        if leaves is None:
            continue
        close(depth)
        if stack:
            parent = stack[-1]
            stack[-1] = (parent[0], parent[1], parent[2] + count, parent[3])
        stack.append((depth, count, 0, symbol(text)))
    if stack:
        close(0)
    return threads


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--seconds", type=int, default=10)
    parser.add_argument("--pid", type=int)
    parser.add_argument("--top", type=int, default=6, help="functions listed per thread")
    parser.add_argument("--waits", type=int, default=3, help="waiting functions listed per thread")
    parser.add_argument("--keep", help="also write sample's full report to this file")
    parser.add_argument("--report", help="parse an existing sample report instead of sampling")
    args = parser.parse_args()

    if args.report:
        report = open(args.report, errors="replace").read()
    else:
        pid = args.pid
        if pid is None:
            found = subprocess.run(["pgrep", "-f", "MacOS/lichii|eden-cli"], capture_output=True,
                                   text=True).stdout.split()
            if not found:
                sys.exit("lichii is not running; start a game first")
            pid = int(found[0])
        with tempfile.NamedTemporaryFile(suffix=".txt") as out:
            subprocess.run(["sample", str(pid), str(args.seconds), "-file", out.name],
                           check=True, capture_output=True)
            report = open(out.name, errors="replace").read()
        if args.keep:
            open(args.keep, "w").write(report)

    rows = []
    threads = parse(report)
    longest = max((thread[1] for thread in threads), default=0)
    for name, total, leaves, waits in threads:
        # Threads that lived for a few samples only say nothing.
        if total * 20 < longest:
            continue
        idle = sum(count for leaf, count in leaves.items() if leaf in IDLE)
        # Unsymbolized frames are the guest's code on the core threads only.
        guest = sum(count for leaf, count in leaves.items()
                    if leaf in GUEST) if name.startswith("CPUCore") else 0
        rows.append((100.0 * (total - idle) / total, 100.0 * guest / total, name, total, leaves,
                     waits))
    rows.sort(key=lambda row: -row[0])

    print(f"{'busy':>6} {'guest':>6}  thread")
    for busy, guest, name, total, leaves, waits in rows:
        if busy < 1.0:
            continue
        print(f"{busy:5.1f}% {guest:5.1f}%  {name}")
        work = [(count, leaf) for leaf, count in leaves.items()
                if leaf not in IDLE and leaf not in GUEST]
        for count, leaf in sorted(work, reverse=True)[:args.top]:
            print(f"{'':16}{100.0 * count / total:5.1f}%  {leaf[:100]}")
        for waiter, count in waits.most_common(args.waits):
            print(f"{'':10}waits {100.0 * count / total:5.1f}%  {waiter[:100]}")
    quiet = sum(1 for row in rows if row[0] < 1.0)
    print(f"\n{quiet} threads under 1 % not shown. busy: share of the samples the thread was not "
          "waiting; guest: share spent in the game's own code; waits: where it waited.")


if __name__ == "__main__":
    main()
