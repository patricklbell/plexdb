#!/usr/bin/env python3
import re
import sys
from collections import defaultdict


def parse_server_log(log_path):
    failures = []

    with open(log_path, "r") as f:
        lines = f.readlines()

    current_db = None

    for i, line in enumerate(lines):
        line = line.strip()

        db_match = re.match(r'created new database "([^"]+)"', line)
        if db_match:
            current_db = db_match.group(1)
            continue

        if line.startswith("Assert failed "):
            match = re.match(r'Assert failed "([^"]+)" at (.+?) in (.+)$', line)
            if match:
                cause = match.group(1)
                location = match.group(2)
                failures.append(
                    {
                        "db": current_db,
                        "cause": cause,
                        "location": location,
                        "line_num": i + 1,
                    }
                )

    return failures


def group_failures(failures):
    grouped = defaultdict(list)
    for f in failures:
        grouped[f["cause"]].append(f)
    return grouped


def extract_stmt_type(location):
    stmt_match = re.search(r"\[stmt:auto = (cql::\w+)\]", location)
    if stmt_match:
        return stmt_match.group(1).replace("cql::", "")
    return None


def main():
    log_path = sys.argv[1] if len(sys.argv) > 1 else "server.log"

    failures = parse_server_log(log_path)
    grouped = group_failures(failures)

    print(f"=== Parsed {len(failures)} failures from {log_path} ===\n")

    for cause, items in sorted(grouped.items(), key=lambda x: -len(x[1])):
        print(f"FAILURE: {cause}")
        print(f"  Count: {len(items)} occurrences")
        print(f"  Location: {items[0]['location']}")

        if cause == "not implemented":
            stmt_types = defaultdict(int)
            for f in items:
                stmt = extract_stmt_type(f["location"])
                if stmt:
                    stmt_types[stmt] += 1
            if stmt_types:
                print(f"  Statement types:")
                for stmt, cnt in sorted(stmt_types.items(), key=lambda x: -x[1]):
                    print(f"    - {stmt}: {cnt}")

        print(f"  Example DB: {items[0]['db']}")
        print()

    print(f"\n=== Summary ===")
    print(f"Total unique failure types: {len(grouped)}")
    print(f"Total failures: {len(failures)}")


if __name__ == "__main__":
    main()
