import sys
import time
import random
import numpy as np
from cassandra.cluster import Cluster

def generate_telemetry(t):
    return {
        "timestamp": int(t),
        "altitude": float(0.5 * 9.8 * t**2),
        "velocity": float(9.8 * t),
        "fuel": float(max(0, 100 - t * 0.5)),
        "temperature": float(20 + 0.1 * t + random.uniform(-1, 1)),
    }


def main():
    print("Connecting to Cassandra...")

    try:
        cluster = Cluster(["127.0.0.1"])
        session = cluster.connect()

        print("Connected!")
        print("Cluster name:", cluster.metadata.cluster_name)

        # --- Create keyspace ---
        session.execute("""
        CREATE KEYSPACE IF NOT EXISTS rocket
        """)

        session.set_keyspace("rocket")

        # --- Create table ---
        session.execute("""
        CREATE TABLE IF NOT EXISTS telemetry (
            rocket_id TEXT PRIMARY KEY,
            timestamp INT,
            altitude DOUBLE,
            velocity DOUBLE,
            fuel DOUBLE,
            temperature DOUBLE
        )
        """)

        print("Table ready.")

        # --- Insert telemetry stream ---
        rocket_id = "falcon9-test"
        print("Streaming telemetry...")

        prepared = session.prepare("""
        INSERT INTO telemetry (rocket_id, timestamp, altitude, velocity, fuel, temperature)
        VALUES (?, ?, ?, ?, ?, ?)
        """)

        for t in np.arange(0, 60, 0.1):
            data = generate_telemetry(t)

            session.execute(prepared, (
                rocket_id,
                data["timestamp"],
                data["altitude"],
                data["velocity"],
                data["fuel"],
                data["temperature"]
            ))

            print(f"t={t} altitude={data['altitude']:.1f}m velocity={data['velocity']:.1f}m/s")
            time.sleep(0.01)

        print("Insert complete.")

        # --- Query back data ---
        print("\nQuerying telemetry...")

        rows = session.execute("""
        SELECT * FROM telemetry
        """)

        for row in rows:
            print(row)

        # --- Simple aggregation (client-side) ---
        rows = session.execute("""
        SELECT * FROM telemetry
        """)

        max_altitude = max(r.altitude for r in rows)
        print("\nMax altitude:", max_altitude)

        cluster.shutdown()

    except Exception as e:
        print("Error:", e)
        sys.exit(1)


if __name__ == "__main__":
    main()