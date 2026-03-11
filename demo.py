# pip install apsw

import os
import apsw

for path in ("primary.db", "replica.db"):
    try:
        os.remove(path)
    except FileNotFoundError:
        pass

conn = apsw.Connection("primary.db")
conn.enable_load_extension(True)
conn.load_extension("./resqlite.so")

conn.execute("SELECT resqlite_add_replica('replica.db')")
conn.execute("SELECT resqlite_enable()")

conn.execute("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL)")
conn.execute("INSERT INTO users(name) VALUES ('Alice')")
conn.execute("INSERT INTO users(name) VALUES ('Bob')")
conn.execute("UPDATE users SET name = 'Bobby' WHERE id = 2")

replica = apsw.Connection("replica.db")
rows = list(replica.execute("SELECT id, name FROM users ORDER BY id"))
print(rows)

