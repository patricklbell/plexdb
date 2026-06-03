# CQL Java Example

Rocket-telemetry demo using the [DataStax Java Driver 4.x](https://docs.datastax.com/en/developer/java-driver/4.17/).

## Requirements

- Java 17+
- Maven 3.8+
- A running CQL-compatible server on `127.0.0.1:9042` (no auth)

## Build

```bash
mvn package -q
```

Produces `target/cql-java-example-1.0-SNAPSHOT.jar` (fat jar with all dependencies).

## Run

```bash
# Both modes (default)
java -jar target/cql-java-example-1.0-SNAPSHOT.jar

# Basic mode only — simple primary key, INSERT + SELECT
java -jar target/cql-java-example-1.0-SNAPSHOT.jar --mode basic

# Advanced mode — compound key, batch writes, range queries, pagination, anomaly detection
java -jar target/cql-java-example-1.0-SNAPSHOT.jar --mode advanced

# Custom host / port
java -jar target/cql-java-example-1.0-SNAPSHOT.jar --host 10.0.0.1 --port 9042
```

## What it does

**Basic mode**
- Creates keyspace `rocket` and table `telemetry_basic`
- Inserts 60 rows via a prepared statement
- Reads all rows and computes max altitude

**Advanced mode**
- Creates `telemetry_advanced` with a `(rocket_id, timestamp)` compound primary key clustered by timestamp descending
- Writes 1 800 rows in batches of 50
- Demonstrates: latest-row query, range query, full-scan analytics, manual pagination, and in-memory anomaly detection
