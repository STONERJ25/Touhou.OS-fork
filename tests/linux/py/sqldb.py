"""The sqlite3 module: an in-memory database and a file in the working directory (journal files, locks, transactions, rollback, a second connection that sees what the first one
committed), queries, joins, aggregates, functions defined in Python, text and blob values, a backup and a dump."""
import os
import sqlite3
import threading


def show(label, value=""):
    print(f"{label}: {value}")


show("library", (sqlite3.sqlite_version_info >= (3, 30), sqlite3.threadsafety, sqlite3.paramstyle))

print("--- in memory")
db = sqlite3.connect(":memory:")
db.execute("CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT NOT NULL, age INTEGER, city TEXT)")
db.executemany("INSERT INTO people (name, age, city) VALUES (?, ?, ?)",
               [("Reimu", 17, "Hakurei"), ("Marisa", 17, "Forest"), ("Sakuya", 21, "Mansion"), ("Remilia", 500, "Mansion"), ("Youmu", 18, "Netherworld"), ("Yuyuko", 1000, "Netherworld")])
show("count", db.execute("SELECT COUNT(*) FROM people").fetchone())
show("select", db.execute("SELECT name, age FROM people WHERE age < 100 ORDER BY age DESC, name").fetchall())
show("group by", db.execute("SELECT city, COUNT(*), MAX(age), AVG(age) FROM people GROUP BY city ORDER BY city").fetchall())
show("like", db.execute("SELECT name FROM people WHERE name LIKE 'R%' ORDER BY name").fetchall())
show("subquery", db.execute("SELECT name FROM people WHERE age > (SELECT AVG(age) FROM people) ORDER BY name").fetchall())
db.execute("CREATE TABLE pets (owner INTEGER REFERENCES people(id), pet TEXT)")
db.executemany("INSERT INTO pets VALUES (?, ?)", [(1, "none"), (3, "knives"), (4, "bat"), (4, "bats")])
show("join", db.execute("SELECT p.name, COUNT(t.pet) FROM people p LEFT JOIN pets t ON t.owner = p.id GROUP BY p.id ORDER BY p.id").fetchall())
db.row_factory = sqlite3.Row
row = db.execute("SELECT * FROM people WHERE id = 2").fetchone()
show("Row", (row["name"], row[2], row.keys(), tuple(row)))
db.row_factory = None
cur = db.execute("UPDATE people SET age = age + 1 WHERE city = ?", ("Mansion",))
show("update", (cur.rowcount, db.total_changes))
cur = db.execute("DELETE FROM people WHERE age > 900")
show("delete", (cur.rowcount, db.execute("SELECT COUNT(*) FROM people").fetchone()[0]))
cur = db.execute("INSERT INTO people (name, age, city) VALUES ('Alice', 30, 'Underground')")
show("lastrowid", cur.lastrowid)
try:
    db.execute("INSERT INTO people (id, name) VALUES (1, 'dup')")
except sqlite3.IntegrityError as e:
    show("IntegrityError", e)
try:
    db.execute("INSERT INTO people (name) VALUES (NULL)")
except sqlite3.IntegrityError as e:
    show("NOT NULL", e)
try:
    db.execute("SELECT * FROM nothing")
except sqlite3.OperationalError as e:
    show("OperationalError", e)
try:
    db.execute("SELEC 1")
except sqlite3.OperationalError as e:
    show("syntax error", e)
show("types", db.execute("SELECT typeof(1), typeof(1.5), typeof('x'), typeof(x'00'), typeof(NULL)").fetchone())
db.execute("CREATE TABLE blobs (b BLOB, t TEXT, f REAL)")
db.execute("INSERT INTO blobs VALUES (?, ?, ?)", (bytes(range(256)), "héllo wörld ☃", 2.5e-3))
b, t, f = db.execute("SELECT b, t, f FROM blobs").fetchone()
show("round trip", (len(b), b[255], t, f, db.execute("SELECT length(t), length(CAST(t AS BLOB)) FROM blobs").fetchone()))
show("big integer", db.execute("SELECT ?", (2 ** 62,)).fetchone())
try:
    db.execute("SELECT ?", (2 ** 70,))
except OverflowError as e:
    show("too big", type(e).__name__)
db.create_function("shout", 1, lambda s: s.upper() + "!")
show("function", db.execute("SELECT shout(name) FROM people WHERE id = 1").fetchone())


class Product:
    def __init__(self):
        self.total = 1

    def step(self, value):
        self.total *= value

    def finalize(self):
        return self.total


db.create_aggregate("product", 1, Product)
show("aggregate", db.execute("SELECT product(id) FROM people WHERE id < 5").fetchone())
db.create_collation("reverse", lambda a, b: (a < b) - (a > b))
show("collation", db.execute("SELECT name FROM people ORDER BY name COLLATE reverse LIMIT 3").fetchall())
show("pragma", (db.execute("PRAGMA user_version").fetchone(), db.execute("PRAGMA encoding").fetchone()))
show("window function", db.execute("SELECT name, RANK() OVER (ORDER BY age) FROM people ORDER BY age, name LIMIT 3").fetchall())
show("cte", db.execute("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 10) SELECT SUM(i) FROM n").fetchone())
show("json", db.execute("SELECT json_extract('{\"a\": [1, 2, {\"b\": 3}]}', '$.a[2].b')").fetchone())
db.execute("CREATE VIRTUAL TABLE docs USING fts5(body)")
db.executemany("INSERT INTO docs VALUES (?)", [("the quick brown fox",), ("jumps over the lazy dog",), ("the dog barks",)])
show("full-text search", db.execute("SELECT body FROM docs WHERE docs MATCH 'dog' ORDER BY rowid").fetchall())
show("dump", [l[:60] for l in db.iterdump()][:3])
db.close()

print("--- a file")
for name in ("test.db", "test.db-journal"):
    if os.path.exists(name):
        os.remove(name)
db = sqlite3.connect("test.db")
db.execute("CREATE TABLE log (n INTEGER PRIMARY KEY, text TEXT)")
db.executemany("INSERT INTO log (text) VALUES (?)", [(f"row {i}",) for i in range(1000)])
db.commit()
show("file size is a multiple of the page size", os.path.getsize("test.db") % 4096 == 0)
show("rows", db.execute("SELECT COUNT(*), MIN(text), MAX(n) FROM log").fetchone())
db.execute("INSERT INTO log (text) VALUES ('uncommitted')")
show("in a transaction", db.in_transaction)
other = sqlite3.connect("test.db", timeout=0.2)
show("other connection does not see it", other.execute("SELECT COUNT(*) FROM log").fetchone())
try:
    other.execute("INSERT INTO log (text) VALUES ('blocked')")
except sqlite3.OperationalError as e:
    show("the writer holds the lock", e)
db.rollback()
show("rollback", db.execute("SELECT COUNT(*) FROM log").fetchone())
db.execute("INSERT INTO log (text) VALUES ('committed')")
db.commit()
show("other connection sees the commit", other.execute("SELECT COUNT(*), MAX(text) FROM log").fetchone())
other.execute("INSERT INTO log (text) VALUES ('from the other')")
other.commit()
show("and the first one sees theirs", db.execute("SELECT COUNT(*) FROM log").fetchone())
show("journal mode", db.execute("PRAGMA journal_mode").fetchone())
show("wal", db.execute("PRAGMA journal_mode=WAL").fetchone())
db.execute("INSERT INTO log (text) VALUES ('in the write-ahead log')")
db.commit()
show("reader in WAL mode", other.execute("SELECT COUNT(*) FROM log").fetchone())
db.execute("PRAGMA wal_checkpoint(TRUNCATE)")
show("synchronous", db.execute("PRAGMA synchronous").fetchone())
other.close()
db.execute("PRAGMA journal_mode=DELETE")
results = []


def worker(n):
    c = sqlite3.connect("test.db", timeout=60)
    for i in range(20):
        c.execute("INSERT INTO log (text) VALUES (?)", (f"thread {n} row {i}",))
        c.commit()
    c.close()


ts = [threading.Thread(target=worker, args=(n,)) for n in range(4)]
for t in ts:
    t.start()
for t in ts:
    t.join()
show("four writers", db.execute("SELECT COUNT(*) FROM log WHERE text LIKE 'thread %'").fetchone())
db.execute("VACUUM")
show("vacuum", db.execute("PRAGMA integrity_check").fetchone())
backup = sqlite3.connect("backup.db")
db.backup(backup)
show("backup", backup.execute("SELECT COUNT(*) FROM log").fetchone())
backup.close()
db.close()
show("files", sorted(f for f in os.listdir(".") if f.endswith((".db", "-journal", "-wal", "-shm"))))
db = sqlite3.connect("test.db")
show("reopened", db.execute("SELECT COUNT(*) FROM log").fetchone())
db.close()
try:
    sqlite3.connect("no/such/directory/x.db")
except sqlite3.OperationalError as e:
    show("missing directory", e)
os.mkdir("ro")
db = sqlite3.connect("file:test.db?mode=ro", uri=True)
try:
    db.execute("INSERT INTO log (text) VALUES ('no')")
except sqlite3.OperationalError as e:
    show("read-only", e)
db.close()
print("--- done")
