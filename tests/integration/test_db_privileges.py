"""Least privilege: even a fully compromised server process (SQL injection, RCE)
connects as a user that can read and write rows but cannot alter the schema."""
import unittest

import dbtools


class Privileges(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        dbtools.reset_db()

    def test_app_user_can_do_dml(self):
        rc, out, err = dbtools.as_app("SELECT COUNT(*) FROM products")
        self.assertEqual((rc, out.strip()), (0, "48"), err)

    def test_app_user_cannot_change_schema_or_other_databases(self):
        for sql in ("CREATE TABLE evil (a int)", "DROP TABLE users", "ALTER TABLE users ADD COLUMN x int",
                    "TRUNCATE TABLE audit_events", "CREATE USER 'evil'@'%' IDENTIFIED BY 'x'",
                    "GRANT ALL ON *.* TO 'sf_app'@'%'", "SELECT * FROM mysql.user", "DROP DATABASE stockforge_test"):
            rc, _, err = dbtools.as_app(sql)
            self.assertNotEqual(rc, 0, f"app user was allowed to run: {sql}")
            self.assertIn("denied", err.lower(), sql)
        self.assertEqual(dbtools.scalar("SELECT COUNT(*) FROM users"), "3")


if __name__ == "__main__":
    unittest.main()
