#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alejandro Bustos
#
# Reel's keyframes across a reboot (implementation plan, 6.4). The bundle Ingen
# saved during pi_check.py (Reel's state included), kept on the card, is loaded
# into the freshly booted engine as Peepo loads a patch (the running blocks
# removed, then patch:Copy from the file into ingen:/main), and Reel must list
# the same keyframes on t_keyframes. Run on the Pi as the peepo user; reload
# the saved patch afterwards. Usage:
#   reboot_check.py BUNDLE_DIR EXPECTED_LIST BLOCK...   (blocks removed first)
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pi_check import Ingen, M

bundle, expected, blocks = sys.argv[1], sys.argv[2], sys.argv[3:]
e = Ingen()
for b in blocks:
    e.check('remove %s' % b, e._send(('Delete', '\tpatch:subject <%s/%s> .\n' % (M, b))))
e.check('load %s into %s' % (bundle, M), e._send(('Copy', '\tpatch:subject <file://%s> ;\n\tpatch:destination <%s> .\n' % (bundle, M))))
time.sleep(2)
listed = e.keyframe_list(timeout=5)
ok = listed == expected
print(('PASS' if ok else 'FAIL') + ' Reel restored %d keyframes after the reboot: %s' % ((len(listed) - 2) // 24, listed), flush=True)
sys.exit(0 if ok else 1)
