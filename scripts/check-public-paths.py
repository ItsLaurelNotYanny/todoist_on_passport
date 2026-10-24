#!/usr/bin/env python3
"""Reject developer home-directory paths in tracked text without rewriting files."""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PRIVATE_PATH = re.compile(
    r"/(?:home|Users)/[A-Za-z0-9_.-]+/[^\s\"'<>?\\]*"
    r'|[A-Za-z]:[\\/]+Users[\\/]+[^\\/\s]+[\\/]'
)


# These exact paths are examples in the vendored SQLite documentation.
SQLITE_FILES = {'third_party/sqlite/sqlite3.c', 'third_party/sqlite/sqlite3.h'}
SQLITE_EXAMPLES = {'/' + path for path in (
    'home/fred/data.db', 'home/user1/config.db', 'home/user1/config.db-shm',
    'Users/me/foo.db', 'home/bill/a.db-journal', 'home/bill/b.db-journal',
)}


def private_matches(name, line):
    return [match.group() for match in PRIVATE_PATH.finditer(line)
            if not (name in SQLITE_FILES and match.group() in SQLITE_EXAMPLES)]


def self_test():
    assert PRIVATE_PATH.search('/' + 'home' + '/developer/project')
    assert PRIVATE_PATH.search('file:' + '///' + 'Users' + '/developer/project')
    assert PRIVATE_PATH.search('C:' + '\\Users\\developer\\project')
    assert not PRIVATE_PATH.search('/opt/bb10-toolchain.cmake')
    assert not PRIVATE_PATH.search('https://example.org/docs')
    assert not PRIVATE_PATH.search('../docs/architecture.md')
    example = '/' + 'home/fred/data.db'
    assert not private_matches('third_party/sqlite/sqlite3.c', example)
    assert private_matches('src/app.cpp', example)
    assert private_matches('third_party/sqlite/sqlite3.c', '/' + 'home/developer/private')


def main():
    self_test()
    if sys.argv[1:] == ['--self-test']:
        print('OK: public path guard self-check')
        return 0
    if sys.argv[1:]:
        raise SystemExit('Usage: check-public-paths.py [--self-test]')
    names = subprocess.check_output(['git', '-C', str(ROOT), 'ls-files', '-z'])
    failures = []
    for raw_name in names.split(b'\0'):
        if not raw_name:
            continue
        name = raw_name.decode('utf-8')
        content = (ROOT / name).read_bytes()
        if b'\0' in content:
            continue
        for number, line in enumerate(content.decode('utf-8', errors='replace').splitlines(), 1):
            if private_matches(name, line):
                failures.append('%s:%d' % (name, number))
    for location in failures:
        print('FAIL: private home-directory path in ' + location, file=sys.stderr)
    if failures:
        print('Use a relative path, an environment variable or a documented placeholder.', file=sys.stderr)
        return 1
    print('OK: tracked text contains no developer home-directory paths')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
