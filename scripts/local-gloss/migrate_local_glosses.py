#!/usr/bin/env python3
"""把旧版安装包里自带的本地释义包迁到新版。

0.10.1 的 english.db 带着 9 个语种、225 万条 zh_glosses；新版安装包不再分发
这份数据，于是「本地优先」会静默退化成每个词都打云端。这个脚本提供两条路：

    # 1) 抽成独立的小库，随安装包分发，首次启动时再合并
    python migrate_local_glosses.py export \
        --source "D:/旧版/data/english.db" --out local_glosses.db

    # 2) 直接并进正在用的 english.db（先备份）
    python migrate_local_glosses.py merge \
        --source "D:/旧版/data/english.db" --target "D:/Program Files/Metasequoia/english.db"

两边表结构一致：zh_glosses(chinese TEXT, lang TEXT, gloss TEXT) + 唯一索引，
所以是纯粹的 INSERT OR IGNORE，不需要改任何代码。
"""

import argparse
import os
import shutil
import sqlite3
import sys
import time

BATCH = 20000


def open_ro(path):
    if not os.path.isfile(path):
        sys.exit(f"找不到文件：{path}")
    return sqlite3.connect(f"file:{path}?mode=ro", uri=True)


def ensure_schema(con):
    con.execute(
        "CREATE TABLE IF NOT EXISTS zh_glosses ("
        " chinese TEXT NOT NULL,"
        " lang TEXT NOT NULL,"
        " gloss TEXT NOT NULL,"
        " PRIMARY KEY (chinese, lang))"
    )
    con.execute("CREATE UNIQUE INDEX IF NOT EXISTS zh_glosses_key ON zh_glosses(chinese, lang)")


def describe(con, label):
    try:
        tables = {r[0] for r in con.execute("SELECT name FROM sqlite_master WHERE type='table'")}
    except sqlite3.DatabaseError as exc:
        sys.exit(f"{label} 不是可读的 SQLite 文件：{exc}")
    if "zh_glosses" not in tables:
        sys.exit(f"{label} 里没有 zh_glosses，可能不是本地释义包")
    total = con.execute("SELECT count(*) FROM zh_glosses").fetchone()[0]
    langs = con.execute("SELECT lang, count(*) FROM zh_glosses GROUP BY lang ORDER BY lang").fetchall()
    print(f"{label}: {total} 条")
    for lang, count in langs:
        print(f"  {lang}: {count}")
    return total


def copy_rows(source, target):
    target.execute("BEGIN")
    copied = 0
    started = time.time()
    cursor = source.execute("SELECT chinese, lang, gloss FROM zh_glosses")
    while True:
        rows = cursor.fetchmany(BATCH)
        if not rows:
            break
        target.executemany(
            "INSERT OR IGNORE INTO zh_glosses(chinese, lang, gloss) VALUES(?,?,?)", rows
        )
        copied += len(rows)
        elapsed = max(time.time() - started, 0.001)
        print(f"\r  已处理 {copied} 条（{copied / elapsed:.0f} 条/秒）", end="", flush=True)
    target.execute("COMMIT")
    print()
    return copied


def cmd_export(args):
    source = open_ro(args.source)
    describe(source, "源库")
    if os.path.exists(args.out):
        if not args.force:
            sys.exit(f"{args.out} 已存在，加 --force 覆盖")
        os.remove(args.out)

    target = sqlite3.connect(args.out)
    ensure_schema(target)
    target.execute("PRAGMA journal_mode=OFF")
    target.execute("PRAGMA synchronous=OFF")
    copied = copy_rows(source, target)
    target.execute("VACUUM")
    target.close()
    source.close()
    size = os.path.getsize(args.out) / (1024 * 1024)
    print(f"完成：{args.out}（{copied} 条，{size:.1f} MB）")


def cmd_merge(args):
    source = open_ro(args.source)
    describe(source, "源库")
    if not os.path.exists(args.target):
        sys.exit(f"找不到目标库：{args.target}")

    if not args.no_backup:
        stamp = time.strftime("%Y%m%d-%H%M%S")
        backup = f"{args.target}.bak-{stamp}"
        print(f"备份 {backup}")
        shutil.copy2(args.target, backup)

    target = sqlite3.connect(args.target)
    ensure_schema(target)
    target.execute("PRAGMA journal_mode=WAL")
    target.execute("PRAGMA synchronous=NORMAL")
    before = target.execute("SELECT count(*) FROM zh_glosses").fetchone()[0]
    copied = copy_rows(source, target)
    after = target.execute("SELECT count(*) FROM zh_glosses").fetchone()[0]
    target.execute("PRAGMA wal_checkpoint(TRUNCATE)")
    target.close()
    source.close()
    print(f"完成：{before} → {after} 条（写入 {copied}，新增 {after - before}）")


def main():
    parser = argparse.ArgumentParser(description="迁移旧版本地释义包")
    sub = parser.add_subparsers(dest="command", required=True)

    export = sub.add_parser("export", help="抽成独立的 local_glosses.db")
    export.add_argument("--source", required=True, help="旧版 english.db 路径")
    export.add_argument("--out", default="local_glosses.db", help="输出文件名")
    export.add_argument("--force", action="store_true", help="覆盖已存在的输出")
    export.set_defaults(func=cmd_export)

    merge = sub.add_parser("merge", help="直接合并进目标 english.db")
    merge.add_argument("--source", required=True, help="旧版 english.db 路径")
    merge.add_argument("--target", required=True, help="要写入的 english.db 路径")
    merge.add_argument("--no-backup", action="store_true", help="跳过备份（不建议）")
    merge.set_defaults(func=cmd_merge)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
