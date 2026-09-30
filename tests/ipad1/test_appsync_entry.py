#!/usr/bin/env python3
"""appsync_cachepatch's entry check: framed entries (3.x/4.x) and iOS 5.x's movs;b.w thunk pass, non-entries
refuse. Host only. Fails if the 5.x thunk rule is reverted (9A5288d..9B206 would refuse to patch)."""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../imgtools"))
from appsync_cachepatch import looks_like_thumb_entry as ok

assert ok(bytes.fromhex("80b500af"))        # push {r7,lr}   8L1, 9A5220p
assert ok(bytes.fromhex("2de9f04f"))        # push.w with lr
assert ok(bytes.fromhex("0022fff7"))        # 5.x thunk: movs r2,#0 ; b.w
assert not ok(bytes.fromhex("2de9f00f"))    # push.w without lr
assert not ok(bytes.fromhex("00207047"))    # the patch itself: movs ; bx lr
assert not ok(bytes.fromhex("00220022"))    # movs ; movs
assert not ok(bytes.fromhex("00000000"))    # data
print("test_appsync_entry: ok")
