-- load the extension
.load ./resqlite sqlite3_resqlite_init

-- configure two replica files
SELECT resqlite_add_replica('replica1.db');
SELECT resqlite_add_replica('replica2.db');

-- turn replication on
SELECT resqlite_enable();

-- writes on the main database will be mirrored
CREATE TABLE users (
  id   INTEGER PRIMARY KEY,
  name TEXT NOT NULL
);

INSERT INTO users(name) VALUES ('Alice');
INSERT INTO users(name) VALUES ('Bob');
UPDATE users SET name = 'Bobby' WHERE id = 2;
DELETE FROM users WHERE id = 1;

-- inspect extension state
SELECT resqlite_status();

-- turn replication off when done
SELECT resqlite_disable();
