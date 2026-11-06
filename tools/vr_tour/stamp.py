#!/usr/bin/env python3
# Reads lines on stdin, writes "<epoch seconds>\t<line>" to the file named in argv[1], flushed per line (the game's log with
# wall-clock times, so spot windows, screenshot times and the log can be lined up). Part of tools/vr_tour.sh.
import sys, time
out = open(sys.argv[1], 'wb', buffering=0)
for line in sys.stdin.buffer:
    out.write(b'%.3f\t' % time.time() + line.rstrip(b'\r\n') + b'\n')
