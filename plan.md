Expression Evaluation Plan — cql.engine.evaluator             
 
Current State
  
evaluate(Term) handles only Constant and literals. FunctionCall, ArithmeticOperation, and TypeHint all assert_not_implemented. There's no row-level evaluation context
(no column reference resolution), no aggregate support, and no predicate evaluation. The conformance suite has 252/313 failures; roughly 30 are directly blocked by the
evaluator gap (uuid(), collection literals, count(*), writetime()).
                                                            
---
Core Design: Evaluation Context

Every call site that evaluates an expression must carry an EvalContext. This is the key structural change — evaluate becomes a context-taking function rather than a
pure term transformer.
                                                            
// evaluator.cppm
struct EvalContext {                                        
    // Bind marker resolution
    TArrayView<const Constant> positional_bindings; // for '?'
    TArrayView<const Pair<AutoString8, Constant>> named_bindings; // for ':name'

    // Row-level access (null for constant folding / INSERT / DDL)       
    const schema::Table* table = nullptr;                     
    const io::ColumnValue* row_values = nullptr; // pre-loaded column array, nullable             

    // Metadata for special selectors (populated per-row by engine)      
    U64 write_timestamp_us = 0;  // for WRITETIME()           
    Optional<U32> ttl_seconds;   // for TTL()                 
};                                                          

Evaluated should remain the return type of evaluate(Term) since the engine already uses it for key serialization. A new evaluate_selector returns io::ColumnValue       
directly (the fully-typed runtime value), used for projection.

---                                                         
Phase 1 — Scalar Expression Evaluation (no rows)

Scope: arithmetic, type hints, core scalar functions, bind marker resolution. These are needed for INSERT/UPDATE values and constant WHERE predicates.

1.1 Arithmetic (+, −, *, /, %)                                

Type promotion rules (matching CQL semantics):                
- Integer value (tinyint/smallint/int/bigint/varint) → S64 arithmetic                        
- Float value (float/double) → F64 arithmetic              
- Mixed int+float → F64                         
- % is integer-only; / produces truncating integer division for integer operands                  
- String + → concatenation (handled only when operands are text constants)                        
- Collection +/− deferred to Phase 2 (needs type information from schema)

BinaryArithmeticOperation and UnaryMinusArithmeticOperation recurse through evaluate(Term, EvalContext). No heap allocation for the common case — both operands are     
Constant results.                                             

1.2 TypeHint                                                  
                                                            
TypeHint is a term-level annotation, not a conversion. It asserts the inner term has the named type; no actual coercion needed for constants. Evaluate the inner term,  check type compatibility, return as-is. The real coercion logic lives in io::cast_write_evaluated_as_column_value (already exists).

1.3 Bind Marker Resolution                                  

case BindMarker:
    if marker is positional: return positional_bindings[marker.index] (or Null if out of range)   
    if marker is named:      look up in named_bindings map    

The engine already collects bind_variables at prepare time (collect_bind_variables). The positional binding index is established at prepare time; the evaluator just    
dereferences.                                                 

1.4 Function Registry                                       

A flat registry of FunctionEntry registered at startup:       

struct FunctionEntry {                                        
    AutoString8 name;                                       
    // nullptr param_types = accepts any types (variadic/polymorphic)
    Optional<DynamicArray<Type>> param_types;                 
    Evaluated (*fn)(TArrayView<const Evaluated>, const EvalContext&);
};                                                            
                                                            
// Module-level registry populated by register_builtin_functions()       
// Lookup: name match + arity check. Overload resolution: first match wins.                     

Priority functions to register (unblocks the most conformance failures): 

┌─────────────────────────────────┬───────────┬──────────────────────────────────────────────┐
│            Function             │  Returns  │                    Notes                     │
├─────────────────────────────────┼───────────┼──────────────────────────────────────────────┤
│ uuid()                          │ uuid      │ plexdb.os random bytes or std::random_device │
├─────────────────────────────────┼───────────┼──────────────────────────────────────────────┤
│ now() / currentTimeUUID()       │ timeuuid  │ coordinator-time timeuuid                    │
├─────────────────────────────────┼───────────┼──────────────────────────────────────────────┤
│ currentTimestamp()              │ timestamp │ ms since epoch                               │
├─────────────────────────────────┼───────────┼──────────────────────────────────────────────┤
│ currentDate()                   │ date      │ today as CQL date                            │
├─────────────────────────────────┼───────────┼──────────────────────────────────────────────┤
│ currentTime()                   │ time      │ time of day, ns                              │
├─────────────────────────────────┼───────────┼──────────────────────────────────────────────┤
│ toDate(timeuuid|timestamp)      │ date      │ extract date part                            │
├─────────────────────────────────┼───────────┼──────────────────────────────────────────────┤
│ toTimestamp(timeuuid|date)      │ timestamp │                                              │
├─────────────────────────────────┼───────────┼──────────────────────────────────────────────┤
│ toUnixTimestamp(*)              │ bigint    │                                              │
├─────────────────────────────────┼───────────┼──────────────────────────────────────────────┤
│ minTimeuuid(t) / maxTimeuuid(t) │ timeuuid  │ boundary timeuuids                           │
├─────────────────────────────────┼───────────┼──────────────────────────────────────────────┤
│ dateOf(timeuuid)                │ timestamp │ deprecated alias                             │
├─────────────────────────────────┼───────────┼──────────────────────────────────────────────┤
│ unixTimestampOf(timeuuid)       │ bigint    │ deprecated alias                             │
├─────────────────────────────────┼───────────┼──────────────────────────────────────────────┤
│ typeAsBlob(v) / blobAsType(v)   │ blob / T  │ all 21 native types × 2                      │
└─────────────────────────────────┴───────────┴──────────────────────────────────────────────┘
                                                            
---                                                           
Phase 2 — Row-Level Evaluation and Predicate Filtering      
                                                      
Scope: TermWithIdentifiers (column references), WHERE predicate evaluation, SELECT projection, WRITETIME/TTL, CONTAINS, multi-column tuple relations.

2.1 TermWithIdentifiers Evaluation                            

Evaluated evaluate(TermWithIdentifiers term, const EvalContext& ctx);    
                                                            
The AutoString8 variant (column reference) looks up ctx.table->cols by name, then reads ctx.row_values[col_index] and converts io::ColumnValue back to Evaluated. TOIArithmeticOperation dispatches the same arithmetic path as Term's BinaryArithmeticOperation but resolves operands via TermWithIdentifiers.

2.2 Predicate Evaluation                                    

The engine currently only evaluates one WHERE relation and only for partition key equality/range. This needs to become a full predicate evaluator invoked per-row for filtering.

// Returns true if the row passes all predicates            
bool evaluate_where(const WhereClause& where, const EvalContext& ctx);   

The engine's SELECT loop would then:                          
1. Use partition/clustering key restrictions to set iterator bounds (existing behavior, keep it)
2. Call evaluate_where per row to filter any remaining predicates (non-key columns, CONTAINS, etc.)      
3. This is the ALLOW FILTERING path — it's correct and complete even if slow                    

Comparison semantics per type:                                
- Scalars: natural ordering for integers, IEEE-754 for floats, lexicographic for text             
- Collections (CONTAINS, CONTAINS KEY): linear scan of deserialized collection                    
- Multi-column tuple: (a, b) > (x, y) → a > x || (a == x && b > y) — lexicographic tuple comparison      
- TOKEN() relation: hash partition key columns → compare against token value                      

Multiple WHERE relations: fix the existing assert_true_not_implemented(relations.length == 1). AND-semantics — all relations must hold.

2.3 Selector Evaluation (SELECT Projection)                   

io::ColumnValue evaluate_selector(const Selector& sel, const EvalContext& ctx);                   
                                                            
┌──────────────────┬─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│ Selector variant │                                                           Action                                                            │
├──────────────────┼─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ ColumnName       │ read row_values[col_index]                                                                                                  │
├──────────────────┼─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ Term             │ evaluate as constant/function                                                                                               │
├──────────────────┼─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ Function         │ evaluate args, dispatch function registry; special-case WRITETIME(col) → ctx.write_timestamp_us, TTL(col) → ctx.ttl_seconds │
├──────────────────┼─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ Count            │ handled at aggregate layer (Phase 3)                                                                                        │
├──────────────────┼─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ Cast             │ evaluate inner selector, apply CAST conversion table                                                                        │
└──────────────────┴─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘

The engine's select_col_indices currently stores only column indices. Extend ExecutionResult or the engine's SELECT handler to carry a DynamicArray<Selector> for non-trivial selectors, falling back to direct column index for the ColumnName case (preserving the fast path).

2.4 CAST                                                    

io::ColumnValue cast_value(io::ColumnValue src, BasicType from, BasicType to);

Implement the full CAST conversion table from the spec. Edge cases: narrowing integer casts (bigint→tinyint truncate); float→int truncate toward zero; timestamp→date extract UTC date; timeuuid→timestamp/date extract embedded timestamp.

---                                                         
Phase 3 — Aggregates and GROUP BY
                                  
Scope: COUNT(*), COUNT(col), SUM, AVG, MIN, MAX. GROUP BY prefix of partition/clustering key.

3.1 Aggregate Accumulator

struct AggregateAccumulator {                               
    TaggedUnion<CountAcc, SumAcc, AvgAcc, MinAcc, MaxAcc> state;         
    void accumulate(const io::ColumnValue& v);
    io::ColumnValue finalize() const;                         
};                                                          

The SELECT handler detects aggregate selectors at prepare time. If any selector is aggregate, the engine materializes all rows into an accumulator array (one per       
aggregate selector), then returns a single synthetic row. Non-aggregate selectors in a mixed query return values from the first row (Cassandra behavior).

3.2 GROUP BY                                                  

Cassandra's GROUP BY only permits a prefix of the partition/clustering key columns. Because rows arrive from the BTree in key order, grouping is a streaming operation —
  no sort needed. Emit an output row when the GROUP BY key changes.     

---                                                           
Phase 4 — JIT with LLVM (Prepared Queries)

Rationale                                                   

For OLAP-style queries scanning millions of rows with complex predicates, tree-walking evaluation adds measurable overhead. ScyllaDB's profiling shows that for large   
result sets, predicate evaluation can consume 10–15% of query time. JIT compiles the predicate + projection into a tight native loop.

However: LLVM is a ~40MB dependency and adds ~2–10ms compilation overhead per expression. JIT is only beneficial for prepared queries executed ≥ ~5,000 times (the      
breakeven point at which the interpreter overhead exceeds LLVM compilation overhead). This is a build-time optional feature, disabled by default.

Architecture                                                

Trigger: in execute(Engine&, U64 prepared_id, ...), after the Nth execution (tracked in PreparedEntry::exec_count), attempt JIT compilation. The interpreter continues  
to be used until compilation completes (and can be the permanent fallback if LLVM is unavailable).

IR Generation — express the expression AST as LLVM IR:      

Input struct passed to compiled function:                     
  { ColumnValue* row_values, U64 col_count,
    Constant* bindings, U64 binding_count,                    
    U64 write_ts, U32 ttl }                                 

Generated functions:                                        
  bool predicate(Input*)        // WHERE clause               
  void project(Input*, ColumnValue* out, U64 out_count)  // SELECT projection                     

Each Term / Selector node maps to LLVM IR:                    
- Constant{S64} → llvm::ConstantInt                           
- Constant{F64} → llvm::ConstantFP                            
- Column reference → getelementptr + load from row_values   
- Arithmetic → llvm::BinaryOperator (add/sub/mul/sdiv/srem for integers, fadd/etc. for floats)    
- Function call → llvm::CallInst to pre-compiled C++ function stubs (uuid, datetime, etc.)    
- Comparison → llvm::ICmpInst / llvm::FCmpInst                
- Boolean AND (multiple WHERE relations) → llvm::BinaryOperator::And on i1 values                 

Passes: mem2reg, instcombine, simplifycfg, scalarizer — standard O1 pipeline sufficient for expression trees. LLVM's ORCv2 LLJIT handles symbol resolution and caching. 

Cache: PreparedEntry gains an Optional<CompiledQuery> field:  

struct CompiledQuery {                                        
    // Owning handle from LLJIT that keeps the module alive 
    llvm::orc::ResourceTrackerSP tracker;                     
    bool (*predicate)(const CompiledQueryInput*);             
    void (*project)(const CompiledQueryInput*, ColumnValue* out, U64 n); 
};                                                            

Build system: option(PLEXDB_JIT "Enable LLVM JIT for prepared queries" OFF). When ON, links llvm-core, llvm-orcjit, llvm-x86-codegen (or target-specific). The          
interpreter is always available as fallback.                  

ScyllaDB Comparison                                           

ScyllaDB doesn't JIT native CQL expressions — it JITs only UDFs (via LuaJIT). For native predicates it relies on:                                                       
1. Restriction pre-processing at prepare time — classify each relation by whether it maps to a key bound (uses iterator), a secondary index lookup, or a filtering    
predicate. This avoids the interpreter entirely for key-bound queries.   
2. Type-specialized byte comparison — compare serialized key bytes directly without deserialization for simple equality/range on typed scalars.                       

Point 1 is the highest-ROI optimization for plexdb independent of JIT: at prepare time, classify each WHERE relation as KeyBound | ClusteringBound | FilterPredicate.   
Key-bound relations set iterator start/stop; only FilterPredicate relations go through the interpreter/JIT. This alone eliminates most per-row expression evaluation for
  the common case.                                             

---                                                         
Implementation Order (by conformance impact)

┌───────────────────────────────────────────────┬────────────────────────────────────────┬──────────────────┐
│                     Step                      │                Unblocks                │ Conformance hits │                                                           
├───────────────────────────────────────────────┼────────────────────────────────────────┼──────────────────┤                                                         
│ 1. Bind marker resolution in evaluate         │ Collection bind params, prepared stmts │ ~14              │                                                           
├───────────────────────────────────────────────┼────────────────────────────────────────┼──────────────────┤                                                         
│ 2. Function registry + uuid(), now()          │ Non-constant term eval in INSERT       │ ~5               │                                                           
├───────────────────────────────────────────────┼────────────────────────────────────────┼──────────────────┤                                                           
│ 3. Arithmetic + TypeHint                      │ general expression evaluation          │ ~3               │                                                           
├───────────────────────────────────────────────┼────────────────────────────────────────┼──────────────────┤                                                           
│ 4. Multiple WHERE relations (AND)             │ range scans pk > x AND pk < y          │ ~6               │                                                         
├───────────────────────────────────────────────┼────────────────────────────────────────┼──────────────────┤                                                           
│ 5. evaluate_selector + COUNT(*)               │ SELECT aggregate/function              │ ~2               │                                                         
├───────────────────────────────────────────────┼────────────────────────────────────────┼──────────────────┤                                                           
│ 6. evaluate_where predicate (non-key columns) │ ALLOW FILTERING queries                │ N/A (new)        │                                                         
├───────────────────────────────────────────────┼────────────────────────────────────────┼──────────────────┤                                                           
│ 7. WRITETIME/TTL selectors                    │ SELECT writetime()                     │ N/A              │                                                         
├───────────────────────────────────────────────┼────────────────────────────────────────┼──────────────────┤                                                           
│ 8. CAST                                       │ type conversion queries                │ N/A              │                                                         
├───────────────────────────────────────────────┼────────────────────────────────────────┼──────────────────┤                                                           
│ 9. Aggregate accumulators                     │ SUM/AVG/MIN/MAX                        │ N/A              │                                                         
├───────────────────────────────────────────────┼────────────────────────────────────────┼──────────────────┤                                                           
│ 10. LLVM JIT (optional)                       │ performance only                       │ N/A              │
└───────────────────────────────────────────────┴────────────────────────────────────────┴──────────────────┘                                                           
                                                            
---                                                           
Key Design Constraints to Honor                             
                                
- No breaking change to Evaluated: the existing io::cast_write_evaluated_as_column_value bridge is already used throughout the engine; keep Evaluated as the
evaluate(Term) return type                                    
- EvalContext is passed by const-ref: evaluating an expression must not mutate engine state (side-effect-free except for functions like uuid() which read entropy, not
engine state)                                                 
- Single-node first: TOKEN() can return hash(partition_key_bytes) using the same murmur3 hash planned for sharding; no partitioner abstraction needed yet             
- Aggregates are materializing: no streaming aggregate iterator for now — accumulate all rows, return one result row. Good enough for single-node with reasonable result
  sets
