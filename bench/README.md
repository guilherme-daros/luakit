# luakit — Microbenchmarks

This directory contains microbenchmarks built on [google/benchmark](https://github.com/google/benchmark) to measure C++/Lua boundary crossing overhead, container conversion, inheritance lookup costs, sandboxing hook overhead, and frame execution strategies.

---

## How to Build and Run Benchmarks

### 1. Configure CMake with Benchmarks Enabled

Benchmarks are disabled by default (`LUNA_BUILD_BENCH=OFF`) so standard builds remain zero-dependency. Enable `LUNA_BUILD_BENCH` in **Release** mode:

```bash
cmake -S . -B build -DLUNA_BUILD_BENCH=ON -DCMAKE_BUILD_TYPE=Release
```

> **Note:** Benchmarks are built without AddressSanitizer or UBSan to ensure accurate timing measurements without sanitizer instrumentation noise.

### 2. Build the `bench` Target

```bash
cmake --build build --target bench
```

### 3. Run the Benchmark Suite

Run all benchmarks with repetition sampling and aggregated summary statistics:

```bash
./build/bench/bench --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

### 4. Filter by Benchmark Group

Use `--benchmark_filter=<group>` to isolate specific benchmark categories:

```bash
# Measure C++/Lua boundary crossing primitives
./build/bench/bench --benchmark_filter=Crossing

# Measure inheritance hierarchy lookup costs
./build/bench/bench --benchmark_filter=Inheritance

# Measure polymorphic identity resolution overhead
./build/bench/bench --benchmark_filter=Identity

# Measure vector and map marshalling
./build/bench/bench --benchmark_filter=Containers

# Compare frame execution strategies (Pure C++, Lua batching, per-item loops)
./build/bench/bench --benchmark_filter=Sequence --benchmark_counters_tabular=true

# Measure sandboxing hook overhead
./build/bench/bench --benchmark_filter=Sandbox
```

---

## Benchmark Results & Analysis

*Measured on Linux x86_64, GCC 16.2 Release build with google/benchmark v1.9.0.*

### 1. `Crossing` — Boundary Crossing Primitives

Measures the isolated overhead of single operations across the C++/Lua boundary.

| Benchmark | CPU Time (Mean) | Iterations | Description |
| :--- | :---: | :---: | :--- |
| `Crossing_TableSet` | **22.5 ns** | 31.9M | Writing a table field from C++ (`t.set("width", 1280)`) |
| `Crossing_TableGet` | **28.9 ns** | 24.9M | Reading a table field from C++ (`t.get<int>("width")`) |
| `Crossing_FunctionCallRoundTrip` | **61.2 ns** | 10.6M | Invoking a void Lua closure from C++ (`fn.call<void>()`) |
| `Crossing_CoroutineResume` | **68.0 ns** | 9.9M | Resuming a yielding Lua coroutine from C++ |
| `Crossing_OverloadOneArm` | **103.0 ns** | 6.9M | Invoking a 1-arm overload set |
| `Crossing_FreeFunctionTwoNumbers` | **113.0 ns** | 6.2M | Lua calling a bound C++ free function taking numbers |
| `Crossing_FreeFunctionString` | **117.0 ns** | 5.9M | Lua calling a bound C++ free function returning string |
| `Crossing_OverloadFourArmsLastMatches` | **122.0 ns** | 5.8M | Invoking a 4-arm overload set matching the last arm |
| `Crossing_PerInstanceStateRead` | **132.0 ns** | 5.5M | Reading dynamic state stashed on object's Lua uservalue table |
| `Crossing_PropertyWrite` | **141.0 ns** | 4.9M | Writing a bound C++ data member (`a_obj.a = 1`) |
| `Crossing_PropertyRead` | **163.0 ns** | 4.1M | Reading a bound C++ data member (`a_obj.a`) |
| `Crossing_MethodOnBorrowedObject` | **170.0 ns** | 4.0M | Invoking a bound C++ method (`a_obj:touch()`) |

> **Comments:** Table field reads/writes are extremely fast (~22–29ns). Function call round-trips cost ~61ns. Invoking bound C++ methods or properties costs ~140–170ns, accounting for metatable lookup, stack parameter checking, and error boundary handling.

---

### 2. `Inheritance` — Hierarchy Lookup Overhead

Measures method and field access across class inheritance depth.

| Benchmark | CPU Time (Mean) | Description |
| :--- | :---: | :--- |
| `Inheritance_OwnFieldFourDeep` | **165 ns** | Field defined on derived class at depth 4 (`d_obj.d`) |
| `Inheritance_OwnMethodNoBases` | **170 ns** | Method on class with no base classes (`a_obj:touch()`) |
| `Inheritance_OwnMethodFourDeep` | **172 ns** | Method defined on derived class at depth 4 (`d_obj:deep()`) |
| `Inheritance_InheritedFieldFourLevelsUp` | **195 ns** | Field inherited from base class 4 levels up (`d_obj.a`) |
| `Inheritance_InheritedMethodOneLevelUp` | **235 ns** | Method inherited from base class 1 level up (`b_obj:touch()`) |
| `Inheritance_InheritedMethodFourLevelsUp` | **239 ns** | Method inherited from base class 4 levels up (`d_obj:touch()`) |

> **Comments:** Class inheritance **depth is virtually free** for a class's own members (170ns at depth 0 vs 172ns at depth 4), confirming that metatable member flattening works effectively. Reaching an *inherited* member pays a small fixed cost (~65ns extra) to convert the receiver pointer through the upcast table.

---

### 3. `Identity` — Polymorphic Type Resolution

Measures pushing C++ object pointers into Lua under exact type vs polymorphic base-pointer resolution.

| Benchmark | CPU Time (Mean) | Description |
| :--- | :---: | :--- |
| `Identity_PushExactType` | **190 ns** | Pushing exact registered type pointer into Lua |
| `Identity_PushNonPolymorphic` | **190 ns** | Pushing non-polymorphic struct pointer into Lua |
| `Identity_PushBaseResolvedToDerived` | **224 ns** | Pushing `Base *` resolved to `Derived` via RTTI `dynamic_cast` |

> **Comments:** Resolving polymorphic base pointers to their most-derived registered C++ type adds only **~34 ns** on first push. This preserves exact Lua object identity (`a == b`) and weak reference table caching across boundary crossings.

---

### 4. `Containers` — Vector and Map Marshalling

Measures container conversion between C++ and Lua across element counts ($N$).

| Benchmark | $N=1$ | $N=10$ | $N=100$ | $N=1,000$ | Unit Cost (Slope) |
| :--- | :---: | :---: | :---: | :---: | :---: |
| `Containers_VectorDoubleArg` | **229 ns** | **311 ns** | **1,063 ns** | **8,286 ns** | **~8.0 ns / element** |
| `Containers_MapStringIntArg` | **379 ns** | **1,389 ns** | **22,033 ns** | **313,224 ns** | **~310 ns / key-value** |

> **Comments:** `std::vector<double>` marshalling is fast (~8 ns per element). `std::map<string, int>` marshalling is more expensive (~310 ns per key-value pair) due to string allocation, key hashing, and table bucket insertion in Lua.

---

### 5. `Sequence` — Frame Execution Strategies

Compares five ways to structure recurring loop workloads over $N$ items.

| Strategy | $N=10$ | $N=100$ | $N=1,000$ | $N=10,000$ | Throughput ($N=1k$) |
| :--- | :---: | :---: | :---: | :---: | :---: |
| `Sequence_PureCpp` | **5.5 ns** | **29.5 ns** | **301 ns** | **3,380 ns** | **3.32 Giga-items/s** |
| **`Sequence_LuaCallsCppBatch`** | **121 ns** | **158 ns** | **470 ns** | **3,761 ns** | **2.13 Giga-items/s** |
| `Sequence_LuaOnly` | **511 ns** | **1,337 ns** | **6,015 ns** | **60,404 ns** | **166 Mega-items/s** |
| `Sequence_LuaDrivesLoop` | **1,729 ns** | **16,441 ns** | **163,601 ns** | **2,485,605 ns** | **6.11 Mega-items/s** |
| `Sequence_CppDrivesLuaPerItem` | **2,395 ns** | **23,789 ns** | **237,148 ns** | **2,394,468 ns** | **4.22 Mega-items/s** |

> **Key Architectural Takeaways:**
> 1. **Batching is King (`Sequence_LuaCallsCppBatch`):** Having Lua invoke a single C++ function that loops natively in C++ is **13x faster than Pure Lua** (`Sequence_LuaOnly`) and **350x faster than per-item Lua calls** (`Sequence_LuaDrivesLoop`).
> 2. **Avoid Per-Item Boundary Crossing (`Sequence_CppDrivesLuaPerItem`):** Invoking a script hook per item from C++ pays a full round-trip cost (~240ns per item) and is the slowest pattern.

---

### 6. `Sandbox` — Instruction Limit Hook Overhead

Measures the VM count hook overhead when host instruction sandboxing is enabled.

| Benchmark | CPU Time ($N=1,000$) | Throughput | Description |
| :--- | :---: | :---: | :--- |
| `Sandbox_LuaDrivesLoopNoLimit` | **166,597 ns** | 6.00 M items/s | Lua loop with no instruction budget |
| `Sandbox_LuaDrivesLoopWithInstructionBudget` | **196,562 ns** | 5.09 M items/s | Lua loop with `Limits{.instructions = ...}` hook armed |

> **Comments:** Host instruction sandboxing (`Limits.instructions`) adds a modest ~18% steady-state overhead. It protects the host process from runaway plugin loops (`while true do end`) while maintaining high execution performance.
