import argparse
import random
import sys
from collections import defaultdict

import numpy as np
from cassandra import ConsistencyLevel
from cassandra.cluster import Cluster
from cassandra.query import BatchStatement


# ==========================================================
# Telemetry generator
# ==========================================================
def generate_telemetry(t):
    return {
        "timestamp": int(t * 1000),  # milliseconds
        "stage": 1 if t < 120 else 2,
        "altitude": float(0.5 * 9.8 * t**2),
        "velocity": float(9.8 * t),
        "fuel": float(max(0, 100 - t * 0.25)),
        "temperature": float(20 + 0.05 * t + random.uniform(-1, 1)),
        "pressure": float(max(0, 101.3 - t * 0.03)),
        "engine_rpm": int(1000 + t * 15 + random.randint(-50, 50)),
        "status": "NOMINAL" if random.random() > 0.01 else "WARNING",
    }


# ==========================================================
# Cassandra setup
# ==========================================================
def setup_keyspace(session):
    session.execute("""
    CREATE KEYSPACE IF NOT EXISTS rocket
    WITH replication = {
        'class': 'SimpleStrategy',
        'replication_factor': 1
    }
    """)

    session.set_keyspace("rocket")


def create_basic_table(session):
    session.execute("""
    CREATE TABLE IF NOT EXISTS telemetry_basic (
        rocket_id TEXT PRIMARY KEY,
        timestamp INT,
        altitude DOUBLE,
        velocity DOUBLE,
        fuel DOUBLE,
        temperature DOUBLE
    )
    """)


def create_advanced_table(session):
    session.execute("""
    CREATE TABLE IF NOT EXISTS telemetry_advanced (
        rocket_id TEXT,
        timestamp BIGINT,
        stage INT,
        altitude DOUBLE,
        velocity DOUBLE,
        fuel DOUBLE,
        temperature DOUBLE,
        pressure DOUBLE,
        engine_rpm INT,
        status TEXT,
        PRIMARY KEY (rocket_id, timestamp)
    ) WITH CLUSTERING ORDER BY (timestamp DESC)
    """)


# ==========================================================
# BASIC MODE
# Original example
# ==========================================================
def run_basic_mode(session):

    print("\n=== BASIC MODE ===")

    create_basic_table(session)

    rocket_id = "falcon9-basic"

    prepared = session.prepare("""
    INSERT INTO telemetry_basic (
        rocket_id,
        timestamp,
        altitude,
        velocity,
        fuel,
        temperature
    )
    VALUES (?, ?, ?, ?, ?, ?)
    """)

    print("Streaming telemetry...")

    for t in np.arange(0, 60, 0.5):
        data = generate_telemetry(t)

        session.execute(
            prepared,
            (
                rocket_id,
                int(t),
                data["altitude"],
                data["velocity"],
                data["fuel"],
                data["temperature"],
            ),
        )

        if int(t) == t:
            print(
                f"T+{int(t):03d}s "
                f"alt={data['altitude']:.1f}m "
                f"vel={data['velocity']:.1f}m/s"
            )

    print("\nInsert complete.")

    rows = session.execute("""
    SELECT *
    FROM telemetry_basic
    """)

    print("\nTelemetry rows:")
    for row in rows:
        print(row)

    rows = session.execute("""
    SELECT *
    FROM telemetry_basic
    """)

    max_altitude = max(r.altitude for r in rows)

    print(f"\nMax altitude: {max_altitude:.2f} m")


# ==========================================================
# ADVANCED MODE
# Real-world telemetry simulation
# ==========================================================
def run_advanced_mode(session):

    print("\n=== ADVANCED MODE ===")

    create_advanced_table(session)

    rocket_id = "falcon9-advanced"

    insert_stmt = session.prepare("""
    INSERT INTO telemetry_advanced (
        rocket_id,
        timestamp,
        stage,
        altitude,
        velocity,
        fuel,
        temperature,
        pressure,
        engine_rpm,
        status
    )
    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    """)

    insert_stmt.consistency_level = ConsistencyLevel.ONE

    batch = BatchStatement()

    print("Streaming telemetry...")

    inserted = 0

    for t in np.arange(0, 180, 0.1):
        data = generate_telemetry(t)

        batch.add(
            insert_stmt,
            (
                rocket_id,
                data["timestamp"],
                data["stage"],
                data["altitude"],
                data["velocity"],
                data["fuel"],
                data["temperature"],
                data["pressure"],
                data["engine_rpm"],
                data["status"],
            ),
        )

        inserted += 1

        if inserted % 50 == 0:
            session.execute(batch)
            batch.clear()

        if int(t) % 10 == 0 and abs(t - int(t)) < 0.01:
            print(
                f"T+{int(t):03d}s | "
                f"alt={data['altitude']:10.1f}m | "
                f"vel={data['velocity']:7.1f}m/s | "
                f"fuel={data['fuel']:6.1f}% | "
                f"temp={data['temperature']:5.1f}C | "
                f"status={data['status']}"
            )

    if len(batch) > 0:
        session.execute(batch)

    print(f"\nInserted {inserted} rows.")

    # ------------------------------------------------------
    # Latest telemetry
    # ------------------------------------------------------
    print("\n=== Latest telemetry ===")

    rows = session.execute("""
    SELECT *
    FROM telemetry_advanced
    WHERE rocket_id = 'falcon9-advanced'
    LIMIT 5
    """)

    for row in rows:
        print(
            f"T={row.timestamp} "
            f"alt={row.altitude:.1f} "
            f"vel={row.velocity:.1f} "
            f"fuel={row.fuel:.1f}"
        )

    # ------------------------------------------------------
    # Range query
    # ------------------------------------------------------
    print("\n=== Range query ===")

    rows = session.execute("""
    SELECT timestamp, altitude, velocity
    FROM telemetry_advanced
    WHERE rocket_id = 'falcon9-advanced'
      AND timestamp >= 60000
      AND timestamp <= 70000
    """)

    for idx, row in enumerate(rows):
        print(f"T={row.timestamp} alt={row.altitude:.1f} vel={row.velocity:.1f}")

        if idx >= 4:
            break

    # ------------------------------------------------------
    # Analytics
    # ------------------------------------------------------
    print("\n=== Analytics ===")

    rows = session.execute("""
    SELECT altitude, velocity, fuel, temperature, status
    FROM telemetry_advanced
    WHERE rocket_id = 'falcon9-advanced'
    """)

    max_altitude = 0
    max_velocity = 0
    min_fuel = 100
    temps = []
    warnings = 0

    for row in rows:
        max_altitude = max(max_altitude, row.altitude)
        max_velocity = max(max_velocity, row.velocity)
        min_fuel = min(min_fuel, row.fuel)

        temps.append(row.temperature)

        if row.status == "WARNING":
            warnings += 1

    print(f"Max altitude:   {max_altitude:.2f} m")
    print(f"Max velocity:   {max_velocity:.2f} m/s")
    print(f"Remaining fuel: {min_fuel:.2f}%")
    print(f"Average temp:   {np.mean(temps):.2f} C")
    print(f"Warnings:       {warnings}")

    # ------------------------------------------------------
    # Pagination
    # ------------------------------------------------------
    print("\n=== Pagination ===")

    statement = session.prepare("""
    SELECT timestamp, altitude
    FROM telemetry_advanced
    WHERE rocket_id = ?
    """)

    statement.fetch_size = 20

    rows = session.execute(statement, [rocket_id])

    page = 1

    while True:
        print(f"\nPage {page}")

        for row in rows.current_rows[:5]:
            print(f"T={row.timestamp} alt={row.altitude:.1f}")

        if not rows.has_more_pages:
            break

        rows.fetch_next_page()

        page += 1

        if page > 3:
            break

    # ------------------------------------------------------
    # Anomaly detection
    # ------------------------------------------------------
    print("\n=== Anomaly detection ===")

    rows = session.execute("""
    SELECT timestamp, temperature, pressure
    FROM telemetry_advanced
    WHERE rocket_id = 'falcon9-advanced'
    """)

    anomalies = []

    for row in rows:
        if row.temperature > 35 or row.pressure < 95:
            anomalies.append(row)

    print(f"Detected {len(anomalies)} anomalies")

    for row in anomalies[:5]:
        print(
            f"T={row.timestamp} temp={row.temperature:.1f} pressure={row.pressure:.1f}"
        )


# ==========================================================
# MAIN
# ==========================================================
def main():

    parser = argparse.ArgumentParser(description="Rocket telemetry Cassandra example")

    parser.add_argument(
        "--mode",
        choices=["basic", "advanced", "both"],
        default="both",
        help="Which example mode to run",
    )

    parser.add_argument(
        "--host",
        default="127.0.0.1",
        help="Cassandra host",
    )

    args = parser.parse_args()

    print("Connecting to Cassandra...")

    try:
        cluster = Cluster([args.host])

        session = cluster.connect()

        print("Connected!")
        print("Cluster:", cluster.metadata.cluster_name)

        setup_keyspace(session)

        if args.mode in ("basic", "both"):
            run_basic_mode(session)

        if args.mode in ("advanced", "both"):
            run_advanced_mode(session)

        cluster.shutdown()

        print("\nDone.")

    except Exception as e:
        print("Error:", e)
        sys.exit(1)


if __name__ == "__main__":
    main()
