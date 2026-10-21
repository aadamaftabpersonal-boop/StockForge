"""Rebuild the throwaway test database (migrations + seed + app user).
    scripts/dev.sh python3 tests/integration/resetdb.py [times]"""
import sys

import dbtools

for i in range(int(sys.argv[1]) if len(sys.argv) > 1 else 1):
    dbtools.reset_db()
    print(f"reset #{i + 1} ok: {dbtools.scalar('SELECT COUNT(*) FROM products')} products")
