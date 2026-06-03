package com.example;

import com.datastax.oss.driver.api.core.CqlSession;
import com.datastax.oss.driver.api.core.cql.*;

import java.net.InetSocketAddress;
import java.nio.ByteBuffer;
import java.util.*;

public class CqlExample {
    record Telemetry(
        long    timestamp,
        int     stage,
        double  altitude,
        double  velocity,
        double  fuel,
        double  temperature,
        double  pressure,
        int     engineRpm,
        String  status
    ) {}

    static final Random RNG = new Random(42);

    static Telemetry generate(double t) {
        return new Telemetry(
            (long) (t * 1000),
            t < 120 ? 1 : 2,
            0.5 * 9.8 * t * t,
            9.8 * t,
            Math.max(0, 100 - t * 0.25),
            20 + 0.05 * t + (RNG.nextDouble() * 2 - 1),
            Math.max(0, 101.3 - t * 0.03),
            1000 + (int) (t * 15) + (RNG.nextInt(101) - 50),
            RNG.nextDouble() > 0.01 ? "NOMINAL" : "WARNING"
        );
    }

    static void setupKeyspace(CqlSession session) {
        session.execute("""
            CREATE KEYSPACE IF NOT EXISTS rocket
            WITH replication = {
                'class': 'SimpleStrategy',
                'replication_factor': 1
            }
            """);
    }

    static void runBasicMode(CqlSession session) {
        System.out.println("\n=== BASIC MODE ===");

        session.execute("""
            CREATE TABLE IF NOT EXISTS rocket.telemetry_basic (
                rocket_id   TEXT PRIMARY KEY,
                timestamp   INT,
                altitude    DOUBLE,
                velocity    DOUBLE,
                fuel        DOUBLE,
                temperature DOUBLE
            )
            """);

        PreparedStatement insert = session.prepare("""
            INSERT INTO rocket.telemetry_basic
                (rocket_id, timestamp, altitude, velocity, fuel, temperature)
            VALUES (?, ?, ?, ?, ?, ?)
            """);

        System.out.println("Streaming telemetry...");

        for (int ti = 0; ti <= 118; ti += 2) {
            double t = ti * 0.5;
            Telemetry d = generate(t);
            session.execute(insert.bind(
                "falcon9-basic",
                (int) t,
                d.altitude(),
                d.velocity(),
                d.fuel(),
                d.temperature()
            ));
            if (ti % 20 == 0) {
                System.out.printf("T+%03ds  alt=%.1fm  vel=%.1fm/s%n",
                    (int) t, d.altitude(), d.velocity());
            }
        }

        System.out.println("\nInsert complete.");

        ResultSet rs = session.execute("SELECT * FROM rocket.telemetry_basic");
        double maxAlt = 0;
        int count = 0;
        for (Row row : rs) {
            maxAlt = Math.max(maxAlt, row.getDouble("altitude"));
            count++;
        }
        System.out.printf("Rows returned: %d%n", count);
        System.out.printf("Max altitude:  %.2f m%n", maxAlt);
    }

    // ===========================================================
    static void runAdvancedMode(CqlSession session) {
        System.out.println("\n=== ADVANCED MODE ===");

        session.execute("""
            CREATE TABLE IF NOT EXISTS rocket.telemetry_advanced (
                rocket_id   TEXT,
                timestamp   BIGINT,
                stage       INT,
                altitude    DOUBLE,
                velocity    DOUBLE,
                fuel        DOUBLE,
                temperature DOUBLE,
                pressure    DOUBLE,
                engine_rpm  INT,
                status      TEXT,
                PRIMARY KEY (rocket_id, timestamp)
            ) WITH CLUSTERING ORDER BY (timestamp DESC)
            """);

        PreparedStatement insert = session.prepare("""
            INSERT INTO rocket.telemetry_advanced
                (rocket_id, timestamp, stage, altitude, velocity,
                 fuel, temperature, pressure, engine_rpm, status)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """);

        System.out.println("Streaming telemetry...");

        final String rocketId = "falcon9-advanced";
        BatchStatementBuilder batch = BatchStatement.builder(DefaultBatchType.LOGGED);
        int batchSize = 0;
        int inserted = 0;

        for (int ti = 0; ti < 1800; ti++) {
            double t = ti * 0.1;
            Telemetry d = generate(t);

            batch.addStatement(insert.bind(
                rocketId,
                d.timestamp(),
                d.stage(),
                d.altitude(),
                d.velocity(),
                d.fuel(),
                d.temperature(),
                d.pressure(),
                d.engineRpm(),
                d.status()
            ));
            batchSize++;
            inserted++;

            if (batchSize == 50) {
                session.execute(batch.build());
                batch = BatchStatement.builder(DefaultBatchType.LOGGED);
                batchSize = 0;
            }

            if (ti % 100 == 0) {
                System.out.printf(
                    "T+%03ds | alt=%10.1fm | vel=%7.1fm/s | fuel=%6.1f%% | status=%s%n",
                    (int) t, d.altitude(), d.velocity(), d.fuel(), d.status());
            }
        }

        if (batchSize > 0) {
            session.execute(batch.build());
        }

        System.out.printf("%nInserted %d rows.%n", inserted);

        // ── Latest telemetry ─────────────────────────────────────
        System.out.println("\n=== Latest telemetry ===");

        PreparedStatement latest = session.prepare("""
            SELECT * FROM rocket.telemetry_advanced
            WHERE rocket_id = ?
            LIMIT 5
            """);

        for (Row row : session.execute(latest.bind(rocketId))) {
            System.out.printf("T=%-8d  alt=%.1f  vel=%.1f  fuel=%.1f%n",
                row.getLong("timestamp"),
                row.getDouble("altitude"),
                row.getDouble("velocity"),
                row.getDouble("fuel"));
        }

        // ── Range query ───────────────────────────────────────────
        System.out.println("\n=== Range query (T=60s–70s) ===");

        PreparedStatement range = session.prepare("""
            SELECT timestamp, altitude, velocity
            FROM rocket.telemetry_advanced
            WHERE rocket_id = ?
              AND timestamp >= ? AND timestamp <= ?
            """);

        int shown = 0;
        for (Row row : session.execute(range.bind(rocketId, 60_000L, 70_000L))) {
            System.out.printf("T=%-8d  alt=%.1f  vel=%.1f%n",
                row.getLong("timestamp"),
                row.getDouble("altitude"),
                row.getDouble("velocity"));
            if (++shown >= 5) break;
        }

        // ── Analytics ─────────────────────────────────────────────
        System.out.println("\n=== Analytics ===");

        ResultSet all = session.execute("""
            SELECT altitude, velocity, fuel, temperature, status
            FROM rocket.telemetry_advanced
            WHERE rocket_id = 'falcon9-advanced'
            """);

        double maxAlt = 0, maxVel = 0, minFuel = 100;
        DoubleSummaryStatistics tempStats = new DoubleSummaryStatistics();
        int warnings = 0;

        for (Row row : all) {
            maxAlt  = Math.max(maxAlt,  row.getDouble("altitude"));
            maxVel  = Math.max(maxVel,  row.getDouble("velocity"));
            minFuel = Math.min(minFuel, row.getDouble("fuel"));
            tempStats.accept(row.getDouble("temperature"));
            if ("WARNING".equals(row.getString("status"))) warnings++;
        }

        System.out.printf("Max altitude:   %.2f m%n",   maxAlt);
        System.out.printf("Max velocity:   %.2f m/s%n", maxVel);
        System.out.printf("Remaining fuel: %.2f %%%n",  minFuel);
        System.out.printf("Average temp:   %.2f C%n",   tempStats.getAverage());
        System.out.printf("Warnings:       %d%n",       warnings);

        // ── Pagination ────────────────────────────────────────────
        System.out.println("\n=== Pagination ===");

        PreparedStatement paginate = session.prepare("""
            SELECT timestamp, altitude
            FROM rocket.telemetry_advanced
            WHERE rocket_id = ?
            """);

        ByteBuffer pagingState = null;
        int page = 0;

        while (page < 3) {
            page++;
            BoundStatement stmt = paginate.bind(rocketId).setPageSize(20);
            if (pagingState != null) stmt = stmt.setPagingState(pagingState);

            ResultSet rs = session.execute(stmt);
            System.out.printf("%nPage %d%n", page);

            // Consume exactly the fetched page without triggering auto-fetch
            int available = rs.getAvailableWithoutFetching();
            for (int i = 0; i < available; i++) {
                Row row = rs.one();
                if (i < 5) {
                    System.out.printf("  T=%-8d  alt=%.1f%n",
                        row.getLong("timestamp"), row.getDouble("altitude"));
                }
            }

            pagingState = rs.getExecutionInfo().getPagingState();
            if (pagingState == null) break;
        }

        // ── Anomaly detection ─────────────────────────────────────
        System.out.println("\n=== Anomaly detection ===");

        ResultSet sensor = session.execute("""
            SELECT timestamp, temperature, pressure
            FROM rocket.telemetry_advanced
            WHERE rocket_id = 'falcon9-advanced'
            """);

        List<Row> anomalies = new ArrayList<>();
        for (Row row : sensor) {
            if (row.getDouble("temperature") > 35 || row.getDouble("pressure") < 95) {
                anomalies.add(row);
            }
        }

        System.out.printf("Detected %d anomalies%n", anomalies.size());
        anomalies.stream().limit(5).forEach(row ->
            System.out.printf("  T=%-8d  temp=%.1f  pressure=%.1f%n",
                row.getLong("timestamp"),
                row.getDouble("temperature"),
                row.getDouble("pressure")));
    }

    public static void main(String[] args) {
        String host = "127.0.0.1";
        int port = 9042;
        String mode = "both";

        for (int i = 0; i < args.length; i++) {
            switch (args[i]) {
                case "--host" -> host = args[++i];
                case "--port" -> port = Integer.parseInt(args[++i]);
                case "--mode" -> mode = args[++i];
                default -> {
                    System.err.println("Usage: CqlExample [--host H] [--port P] [--mode basic|advanced|both]");
                    System.exit(1);
                }
            }
        }

        System.out.println("Connecting to Cassandra at " + host + ":" + port + " ...");

        try (CqlSession session = CqlSession.builder()
                .addContactPoint(new InetSocketAddress(host, port))
                .withLocalDatacenter("datacenter1")
                .build()) {

            System.out.println("Connected.");
            setupKeyspace(session);

            if (mode.equals("basic") || mode.equals("both"))    runBasicMode(session);
            if (mode.equals("advanced") || mode.equals("both")) runAdvancedMode(session);

            System.out.println("\nDone.");

        } catch (Exception e) {
            System.err.println("Error: " + e.getMessage());
            System.exit(1);
        }
    }
}
