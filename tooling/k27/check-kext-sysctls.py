#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026 Leo Zhang. All rights reserved.
"""Reject permanent SYSCTL node macros in package sources before a KC link.

XNU's static node macros are for early kernel registration, not a kext's
sysctl_register_oid(). Reject the declaration itself, even if registration is
in another translation unit or through a helper/alias. SYSCTL_DECL and handler
I/O macros remain valid. Kexts must build versioned, non-permanent runtime OIDs.
"""
import argparse
from pathlib import Path
import re
import sys


NODE = re.compile(r'\bSYSCTL_(?:OID(?:_WITH_KERN|_MANUAL)?|NODE(?:_MANUAL)?|EXTENSIBLE_NODE|PROC|STRING|INT|UINT|LONG|ULONG|'
                  r'QUAD|UQUAD|INT8|UINT8|INT16|UINT16|INT32|UINT32|INT64|UINT64|'
                  r'OPAQUE|STRUCT(?:_INIT)?|COMPAT_U?INT|SCALABLE_COUNTER|SKMEM_(?:TCP_)?INT)\s*\(')
# Blank comments and literals, preserving newlines and offsets for diagnostics.
NONCODE = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\[\s\S]|[^"\\])*"|\'(?:\\[\s\S]|[^\'\\])*\'')
SUFFIXES = {'.c', '.h', '.cc', '.cpp', '.cxx', '.hpp', '.m', '.mm'}


def violations(text):
    code = NONCODE.sub(lambda m: re.sub(r'[^\n]', ' ', m[0]), text)
    return [(code.count('\n', 0, m.start()) + 1, m[0].split('(')[0].strip()) for m in NODE.finditer(code)]


def self_test():
    # The bt-path1 panic, plus alternate declarations, multiline and headers.
    for macro in ('PROC', 'STRING', 'INT', 'NODE', 'OID', 'OID_WITH_KERN', 'OID_MANUAL', 'NODE_MANUAL',
                  'EXTENSIBLE_NODE', 'COMPAT_INT', 'COMPAT_UINT', 'QUAD', 'STRUCT', 'SCALABLE_COUNTER', 'SKMEM_INT'):
        source = f'SYSCTL_{macro}\n (_kern, OID_AUTO, bad, 0);\nvoid start(void) {{ sysctl_register_oid(&sysctl__kern_bad); }}'
        if violations(source) != [(1, 'SYSCTL_' + macro)]:
            raise AssertionError('missed static SYSCTL_' + macro)
    good = '''// SYSCTL_INT(_kern, OID_AUTO, example);
/* SYSCTL_PROC(_kern, OID_AUTO, example); */
const char *message = "SYSCTL_STRING(not a declaration)";
SYSCTL_DECL(_machdep);
static struct sysctl_oid oid;
void start(void) { bzero(&oid, sizeof oid); sysctl_register_oid(&oid); }
int handler(SYSCTL_HANDLER_ARGS) { return SYSCTL_OUT(req, &value, sizeof value); }
'''
    if violations(good):
        raise AssertionError('rejected runtime OID, handler, comment or string')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('repo', nargs='?', type=Path)
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    self_test()
    if args.self_test:
        print('kext sysctl rule self-test: permanent node macros refused; runtime OIDs accepted')
        return
    if args.repo is None or not (args.repo / 'package').is_dir():
        parser.error('REPO must contain package/')
    bad = []
    for path in sorted((args.repo / 'package').rglob('*')):
        if path.is_file() and path.suffix in SUFFIXES:
            bad.extend(f'{path.relative_to(args.repo)}:{line}: {macro} declares a static node; use a runtime OID'
                       for line, macro in violations(path.read_text()))
    if bad:
        sys.exit('kext sysctl rule FAILED:\n' + '\n'.join(bad))
    print('kext sysctl rule: no permanent node macros in package sources')


if __name__ == '__main__':
    main()
