# resqlite

`resqlite` is a SQLite loadable extension for mirroring top-level write statements from one SQLite connection to one or more replica SQLite databases.

It is a small, practical approach built on SQLite hooks. It is not a distributed database, a conflict-resolution layer, or a full replication system.

[Donate](https://paypal.me/michtatton)

## What it does

- Captures top-level write statements issued on the primary connection.
- Queues captured writes during a transaction and replays them to each configured replica at commit time.
- Mirrors ordinary write workloads to one or more file-backed SQLite databases.
- Exposes simple SQL functions to add replicas, enable or disable mirroring, inspect status, and clear the last error.

## Important limits

This extension deliberately keeps the design small. That also means there are important constraints:

- It mirrors top-level statements, not internal trigger subprogram traces. If the replica schema matches the source schema, replica-side triggers run naturally when the mirrored statements are applied.
- Writes are captured at SQLite statement-trace time, before execution fully completes. If a write inside an explicit transaction later fails, queued SQL may still remain and can cause the final `COMMIT` to fail.
- If replication fails during an explicit transaction, `resqlite` aborts the source `COMMIT`, which causes the source transaction to roll back.
- If replication fails for a single autocommit write, the source write has already succeeded and cannot be undone retroactively.
- Nested transaction control is not supported. In practice, that means `SAVEPOINT`, `RELEASE`, and `ROLLBACK TO` should be avoided.
- Connection-local statements such as `ATTACH` and `DETACH` are not mirrored safely and should not be used while mirroring is enabled.
- Replica databases should use a compatible SQLite version and should remain under the exclusive control of `resqlite`.
- Adding a replica does not clone the primary database. The replica should already have a compatible schema and baseline data if you expect immediate consistency.
- This extension is intended for ordinary file-backed SQLite databases, not exotic VFS setups, cross-version replication, or multi-writer conflict resolution.

## SQL functions

### `resqlite_add_replica(path)`

Adds a replica database.

- `path`: path to the replica SQLite database file.

Returns the number of configured replicas.

Notes:

- The replica database is opened with read/write access and created if it does not already exist.
- This function does not snapshot or copy the current primary database into the replica.

### `resqlite_enable()`

Installs the required SQLite hooks on the current connection and enables mirroring.

Returns `ok` on success.

### `resqlite_disable()`

Disables mirroring, removes installed hooks, clears any queued statements, and closes configured replica handles.

Returns `ok`.

### `resqlite_clear_error()`

Clears the last stored replication error.

Returns `ok`.

### `resqlite_status()`

Returns a JSON status string with the current extension state.

The payload includes:

- `enabled`
- `installed`
- `replicas`
- `queued`
- `unsupported_txn_seen`
- `last_error`

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
        print(path)
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
