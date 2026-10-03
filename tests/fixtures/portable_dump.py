"""Makes `sqlite3 <db> .dump` readable by SQLite before 3.50.

SQLite 3.50 and later write a string holding a newline as
unistr('...\\u000a...'), which older versions (Ubuntu 24.04 has 3.45) can't
read. This rewrites each as replace('...\\u000a...', '\\u000a', char(10)),
which every version reads the same. Any other escape is refused.

    sqlite3 q.db .dump | python3 tests/fixtures/portable_dump.py > out.sql
"""
import re
import sys

UNISTR = re.compile(r"unistr\('((?:[^']|'')*)'\)")


def portable(match):
    text = match.group(1)
    if re.search(r"\\(?!u000a)", text):
        sys.exit("an escape other than \\u000a: " + text)
    return "replace('" + text + "','\\u000a',char(10))"


sys.stdout.write(UNISTR.sub(portable, sys.stdin.read()))
