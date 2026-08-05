#!/usr/bin/env python3
"""Thin CLI over imgtools/itqmp.py - see that module's USAGE/docstring.

  ipod-touch-qmp.py <port> shot <out.ppm>
  ipod-touch-qmp.py <port> tap <x> <y>          # 0..319 x, 0..479 y
  ipod-touch-qmp.py <port> swipe <x1> <y1> <x2> <y2>
  ipod-touch-qmp.py <port> button <home|power|voldown|volup>
  ipod-touch-qmp.py <port> key <qcode>          # raw key, reaches guest as text
  ipod-touch-qmp.py <port> finger <slot> <x> <y> <begin|update|end>
  ipod-touch-qmp.py <port> pinch <cx> <cy> <r0> <r1>
  ipod-touch-qmp.py <port> cmd <qmp-command> [k=v ...]   # e.g. cmd system_reset
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "imgtools"))
import itqmp

if __name__ == "__main__":
    itqmp.main(sys.argv[1:])
