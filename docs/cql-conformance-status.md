## CQL conformance gaps

Baseline: 12 / 313 passing (scylladb ref: master, 2026-06-01).  

| Tests | Feature | Engine assert / symptom |
|------:|---------|------------------------|
| ~71 | WHERE restrictions on clustering columns (`>` `<` `>=` `<=` `IN`) | `column expression relation operator not implemented for PK` |
| ~47 | DELETE on tables with clustering keys | `DELETE on table with clustering key is not implemented` |
| ~25 | ALLOW FILTERING / non-PK column WHERE restrictions | `non-PK column expression relations are not implemented` |
| ~23 | Collection DML — insert/update list/set/map literals; subscript update (`col[k]=v`) | server crash / `subscript/field access in UPDATE SET is not implemented` |
| ~17 | UPDATE on tables with clustering keys | `UPDATE on table with clustering key is not implemented` |
| ~17 | ORDER BY (see also CLUSTERING ORDER BY item below) | `ORDER BY is not implemented` |
| ~17 | ALTER TABLE — column add/drop crash, `USING TIMESTAMP` on DELETE, UDTs | various crashes |
| ~15 | Secondary indexes — `CREATE INDEX`, `CONTAINS`, `CONTAINS KEY` queries | no index support |
| ~15 | SELECT COUNT(\*) / scalar functions / TOKEN() | `SELECT clause type (count/function/cast/term) is not implemented` |
| ~12 | CREATE TABLE — static columns, composite keys, property validation | various |
| ~11 | BATCH statements | `BATCH is not implemented` |
| ~9  | TTL — `USING TTL`, `default_time_to_live` (see TTL item below) | `INSERT USING TTL is not implemented` / `default_time_to_live is not implemented` |
| ~7  | Counter column updates (`col = col + expr`) | `non-constant/non-bind UPDATE assignment is not implemented` |
| ~7  | NULL / unset column writes | `writing null column values is not implemented` |
| 5   | LIMIT / PER PARTITION LIMIT | `LIMIT is not implemented` |
| ~5  | SELECT DISTINCT | `SELECT DISTINCT/JSON is not implemented` |
| ~5  | Column-level DELETE (`DELETE col FROM …`) | `column-level DELETE is not implemented` |
| ~5  | Composite / tuple partition or clustering key types | `partition/clustering key must be a basic type` |
| 2   | smallint / tinyint serialization (wrong results) | `invalid evaluated value for write` |
