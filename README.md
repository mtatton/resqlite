# resqlite

'resqlite' is a SQLite loadable extension that mirrors each **successful top-level write statement** from a source connection to one or more replica SQLite databases.

[Donate](https://paypal.me/michtatton)

## What it does

- Replays successful top-level write statements to each replica.
- Mirrors explicit transaction control ('BEGIN', 'COMMIT', 'ROLLBACK', 'SAVEPOINT', 'RELEASE') so multi-statement transactions stay aligned.
- Can snapshot the source database into a new replica when you add it.

## Important limits

This is a practical loadable-extension approach, not a full distributed replication engine.

- It mirrors **top-level statements**, not internal trigger subprogram traces. If the replica schema matches the source, replica triggers fire naturally.
- If a replica write fails during an **explicit transaction**, 'resqlite' marks the transaction as failed and aborts the source 'COMMIT', causing the source transaction to roll back.
- If a replica write fails during a single **autocommit** statement, the source statement has already succeeded, so the extension cannot retroactively undo it.
- Replica databases should have the same SQLite version/features and stay under exclusive control of 'resqlite'.
- This extension is aimed at ordinary file-backed SQLite databases, not exotic VFS setups or cross-version conflict resolution.

## SQL functions

### 'resqlite_add_replica(path [, snapshot])'
Adds a replica.

- 'path': target database path.
- 'snapshot': default '1'.
  - '1': overwrite/snapshot replica from the current source database before live mirroring starts.
  - '0': open the existing replica as-is.

Returns the number of configured replicas.

### 'resqlite_status()'
Returns a text status summary.

## Build

Linux:

```bash
gcc -fPIC -shared resqlite.c -o resqlite.so -lsqlite3
```

macOS:

```bash
gcc -fPIC -dynamiclib resqlite.c -o resqlite.dylib -lsqlite3
```

Windows (MinGW example):

```bash
gcc -shared resqlite.c -o resqlite.dll -lsqlite3
```

## Example use in sqlite3 CLI

```sql
.load ./resqlite
SELECT resqlite_add_replica('replica.db', 1);

CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);
INSERT INTO t(v) VALUES('hello');
BEGIN;
UPDATE t SET v='world' WHERE id=1;
COMMIT;

SELECT resqlite_status();
```

Python example that loads resqlite

```python
# using package: apsw

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
```

php example:

```php
<?php

declare(strict_types=1);

@unlink('primary.db');
@unlink('replica.db');

$db = new SQLite3('primary.db');
$db->enableExceptions(true);

/*
 * Put resqlite.so in the directory configured by sqlite3.extension_dir,
 * then load it by library name.
 */
if (!$db->loadExtension('resqlite.so')) {
    throw new RuntimeException('Failed to load resqlite.so');
}

/* Configure replication */
$db->query("SELECT resqlite_add_replica('replica.db')");
$db->query("SELECT resqlite_enable()");

/* Write to the primary database */
$db->exec("
    CREATE TABLE users (
        id   INTEGER PRIMARY KEY,
        name TEXT NOT NULL
    )
");

$db->exec("INSERT INTO users(name) VALUES ('Alice')");
$db->exec("INSERT INTO users(name) VALUES ('Bob')");
$db->exec("UPDATE users SET name = 'Bobby' WHERE id = 2");

/* Optional: inspect status from the extension */
$statusResult = $db->query("SELECT resqlite_status() AS status");
$statusRow = $statusResult->fetchArray(SQLITE3_ASSOC);
echo "resqlite status: " . $statusRow['status'] . PHP_EOL;

$db->close();

/* Verify the replica */
$replica = new SQLite3('replica.db');
$replica->enableExceptions(true);

$result = $replica->query("SELECT id, name FROM users ORDER BY id");
while ($row = $result->fetchArray(SQLITE3_ASSOC)) {
    echo $row['id'] . ' | ' . $row['name'] . PHP_EOL;
}

$replica->close();
```

## TBD

- embed SQLite in an application wrapper and replicate before/after statement execution there,
- use the session/changeset API with a stricter apply pipeline,
- or move replication down to a custom VFS / WAL shipping design.
