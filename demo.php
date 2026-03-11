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
