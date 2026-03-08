#!/usr/bin/env node
// Quick smoke-test for a CQL-native-protocol server on localhost:9042 (no auth).
// Requires: npm install cassandra-driver  (DataStax Node.js driver)

"use strict";

const cassandra = require("cassandra-driver");

const KS = "test_ks";

function ok(label)       { console.log(`  [PASS] ${label}`); }
function section(title)  { console.log(`\n${title}\n${"-".repeat(title.length)}`); }
function fail(msg)       { console.error(`  [FAIL] ${msg}`); process.exit(1); }

async function main() {
    const client = new cassandra.Client({
        contactPoints: ["127.0.0.1"],
        localDataCenter: "datacenter1",
        protocolOptions: { port: 9042 },
        // no auth
    });

    await client.connect();
    console.log(`Connected (protocol v${client.controlConnection.protocolVersion})`);

    // ── keyspace ──────────────────────────────────────────────────────────────
    section("Keyspace");

    await client.execute(
        `CREATE KEYSPACE IF NOT EXISTS ${KS}` +
        ` WITH replication = {'class': 'SimpleStrategy', 'replication_factor': 1}`
    );
    ok("CREATE KEYSPACE");

    await client.execute(`USE ${KS}`);
    ok("USE KEYSPACE");

    // ── table ─────────────────────────────────────────────────────────────────
    section("Table DDL");

    await client.execute(`
        CREATE TABLE IF NOT EXISTS users (
            id      uuid    PRIMARY KEY,
            name    text,
            age     int,
            active  boolean
        )
    `);
    ok("CREATE TABLE");

    // ── writes ────────────────────────────────────────────────────────────────
    section("Writes");

    const ids = [cassandra.types.TimeUuid.now(), cassandra.types.TimeUuid.now(), cassandra.types.TimeUuid.now()];
    const rows = [
        [ids[0], "Alice", 30, true],
        [ids[1], "Bob",   25, false],
        [ids[2], "Carol", 35, true],
    ];

    const insert = await client.prepare(
        `INSERT INTO ${KS}.users (id, name, age, active) VALUES (?, ?, ?, ?)`
    );
    for (const row of rows) {
        await client.execute(insert, row, { prepare: false });
    }
    ok(`INSERT ${rows.length} rows (prepared)`);

    // ── reads ─────────────────────────────────────────────────────────────────
    section("Reads");

    const all = await client.execute(`SELECT id, name, age, active FROM ${KS}.users`);
    if (all.rows.length !== 3) fail(`expected 3 rows, got ${all.rows.length}`);
    ok("SELECT * — row count correct");

    const byId = Object.fromEntries(all.rows.map(r => [r.id.toString(), r]));
    for (const [id, name, age, active] of rows) {
        const r = byId[id.toString()];
        if (!r)              fail(`missing row for ${name}`);
        if (r.name !== name) fail(`name mismatch: expected ${name}, got ${r.name}`);
        if (r.age  !== age)  fail(`age mismatch for ${name}: expected ${age}, got ${r.age}`);
        if (r.active !== active) fail(`active mismatch for ${name}`);
    }
    ok("Column values match");

    const activeResult = await client.execute(
        `SELECT name FROM ${KS}.users WHERE active = true ALLOW FILTERING`
    );
    const activeNames = new Set(activeResult.rows.map(r => r.name));
    if (!activeNames.has("Alice") || !activeNames.has("Carol") || activeNames.size !== 2) {
        fail(`unexpected active set: ${[...activeNames].join(", ")}`);
    }
    ok("Filtered SELECT (active = true)");

    // ── update ────────────────────────────────────────────────────────────────
    section("Update");

    await client.execute(`UPDATE ${KS}.users SET age = 26 WHERE id = ${ids[1]}`);
    const updated = await client.execute(`SELECT age FROM ${KS}.users WHERE id = ${ids[1]}`);
    if (updated.first().age !== 26) fail(`expected age 26, got ${updated.first().age}`);
    ok("UPDATE + re-read");

    // ── delete ────────────────────────────────────────────────────────────────
    section("Delete");

    await client.execute(`DELETE FROM ${KS}.users WHERE id = ${ids[2]}`);
    const deleted = await client.execute(`SELECT id FROM ${KS}.users WHERE id = ${ids[2]}`);
    if (deleted.rows.length !== 0) fail("expected row to be deleted");
    ok("DELETE single row");

    // ── cleanup ───────────────────────────────────────────────────────────────
    section("Cleanup");

    await client.execute(`DROP TABLE IF EXISTS ${KS}.users`);
    ok("DROP TABLE");

    await client.execute(`DROP KEYSPACE IF EXISTS ${KS}`);
    ok("DROP KEYSPACE");

    await client.shutdown();
    console.log("\nAll tests passed.");
}

main().catch(err => {
    console.error(err.message);
    process.exit(1);
});
