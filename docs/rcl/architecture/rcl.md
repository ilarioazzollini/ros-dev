# `rcl` — Core Package Deep Dive

> Deep dive into the **`rcl`** package, the core of the repository. Read the
> [architecture overview](./architecture.md) first — this document assumes the
> **shared conventions** described there (§3: the handle+impl pattern, the
> lifecycle quartet, return codes, allocators, attribute tables, fault
> injection). Those are not repeated here; this doc focuses on what is specific
> to `rcl`.
>
> Source: `rcl` package version `10.5.1` (`repos/rcl/rcl/`), `rolling` line.

---

## 1. What `rcl` is

`rcl` is the **ROS Client Library — common implementation**: a pure-C library
that turns the low-level middleware abstraction (`rmw`) into the familiar ROS
concepts (nodes, publishers, subscriptions, services, clients, timers) and adds
the cross-cutting machinery every client library needs (init/shutdown, wait
sets, the graph, command-line arguments, naming/remapping, time, logging,
security).

Mental model: **`rcl` is a thin, careful, conventions-enforcing wrapper over
`rmw`.** When an `rcl` function looks like it "doesn't do much," it is usually
validating inputs, applying ROS conventions (naming/remap/QoS), and delegating to
an `rmw_*` call. Almost every entity's private impl struct ultimately holds an
`rmw_*` handle.

---

## 2. Package layout

```
repos/rcl/rcl/
├── CMakeLists.txt          # builds one shared library: librcl
├── package.xml             # ament manifest + dependencies
├── Doxyfile                # API docs generation
├── QUALITY_DECLARATION.md  # this package is Quality Level 1
├── cmake/                  # logging-impl selection, symbol-visibility helper
├── rcl-extras.cmake        # exported cmake consumed by downstream packages
├── include/rcl/            # PUBLIC C headers — the API surface (43 headers)
├── src/rcl/                # implementation (.c) + private *_impl.h headers
└── test/                   # gtest C++ tests, python launch tests, fixtures
```

Build facts (from `CMakeLists.txt`):

- Everything compiles into a **single shared library, `librcl`**.
- C standard **C11**; tests are **C++17** (for gtest).
- Built with `-Wall -Wextra -Wpedantic`; symbol visibility hidden by default
  (`cmake/rcl_set_symbol_visibility_hidden.cmake`), so only `RCL_PUBLIC` symbols
  are exported. `RCL_BUILDING_DLL` flips the visibility macros to *export* mode.
- The public include dir is `include/`; the **private `src/` dir is also on the
  include path** so `.c` files can reach the `*_impl.h` headers.
- The **logging backend is chosen at configure time**
  (`cmake/get_default_rcl_logging_implementation.cmake`): either dynamically
  loaded via `rcl_logging_implementation`, or statically linked against a
  specific backend (e.g. `rcl_logging_spdlog`). This is the one build knob worth
  knowing early.
- The default discovery range can be baked in via `RCL_DEFAULT_DISCOVERY_RANGE`.

> Build-system note: `CMakeLists.txt` marks several exported deps
> (`rcl_interfaces`, `rcl_logging_interface`, `rmw_implementation`) with
> `TODO(clalancette)` to eventually become `PRIVATE` — downstream code currently
> leaks them. Worth knowing if you touch the build/exports.

---

## 3. Dependencies (and what each is for)

| Dependency | Role |
| --- | --- |
| **`rmw`** | The ROS middleware *interface*. `rcl` calls `rmw_*`; almost every entity wraps an `rmw_*` handle. |
| **`rmw_implementation`** | Selects/loads the concrete middleware at runtime. |
| **`rcutils`** | Low-level C utilities: allocators, logging macros, strings, hash maps, atomics, time, error state, fault injection. Pervasive. |
| **`rosidl_runtime_c`** | Runtime type-support structs (`rosidl_*_type_support_t`) passed into the `*_init` calls. |
| **`rcl_interfaces`** | Generated types for ROS infrastructure (parameters, logging, type descriptions). |
| **`service_msgs`**, **`type_description_interfaces`** | Generated interfaces for service introspection and the type-description service. |
| **`rcl_yaml_param_parser`** | Parses `--params-file` YAML into the C parameter structure. |
| **`rcl_logging_interface`** + an implementation | The external (file) logging backend (see §2). |
| **`libyaml_vendor` / `yaml`** | YAML parsing backend. |
| **`tracetools`** | LTTng tracepoints in hot paths for instrumentation. |

Test-only: `osrf_testing_tools_cpp`, `mimick_vendor` (function mocking),
`test_msgs`, `launch`/`launch_testing`, `ament_lint_*`.

---

## 4. Module map

The public API is grouped by ROS concept. This is the entire `include/rcl/`
surface, grouped functionally.

### Core lifecycle & process state
- `init.h` / `init_options.h` — `rcl_init()` / `rcl_shutdown()` and the options
  passed in. Entry point for the whole library.
- `context.h` — `rcl_context_t`, the per-init/shutdown-cycle state everything is
  created against. **Read this header first** (it has the canonical lifecycle
  ASCII diagram).
- `domain_id.h`, `discovery_options.h` — DDS domain & discovery configuration.
- `arguments.h` — parse/store ROS-specific CLI args (`--ros-args …`). Backed by
  `remap.h`, `lexer.h`, `lexer_lookahead.h`.
- `remap.h` — name remapping rules.
- `security.h` — resolve SROS2 security/enclave options from the environment.
- `log_level.h`, `logging.h`, `logging_rosout.h` — logger configuration and the
  `/rosout` topic publisher.

### Communication entities
- `node.h` / `node_options.h` — `rcl_node_t`, the factory for everything below.
- `publisher.h`, `subscription.h` — topic pub/sub.
- `client.h`, `service.h` — service client & server.
- `timer.h` — timers driven by a steady clock, firing user callbacks.
- `event.h`, `event_callback.h` — QoS/status events. The publisher and
  subscription event-type enums live here (deadline missed, liveliness,
  incompatible QoS, matched, …).
- `guard_condition.h` — a manually-triggerable condition used to wake a wait set.

### Waiting / event-loop primitive
- `wait.h` — `rcl_wait_set_t` and `rcl_wait()`: the single blocking primitive
  higher layers build executors on.

### Naming, validation, introspection
- `validate_topic_name.h`, `validate_enclave_name.h` — name-rule checks.
- `expand_topic_name.h` — expand a relative name to a fully-qualified one (applies
  namespace + substitutions like `{node}`, `~`).
- `graph.h` — discover nodes/topics/services and get notified of changes
  (`rcl_get_topic_names_and_types`, `rcl_get_node_names`, `rcl_count_publishers`,
  `rcl_get_publishers_info_by_topic`, `rcl_wait_for_publishers`, …).
- `network_flow_endpoints.h` — inspect the network 5-tuples an endpoint uses.
- `service_introspection.h` — enable publishing service request/response events.

### Type support & type descriptions
- `dynamic_message_type_support.h`, `type_description_conversions.h`,
  `type_hash.h`, `node_type_cache.h` — runtime type information and the
  `~/get_type_description` service (type negotiation / dynamic typing).

### Time
- `time.h` — clock abstraction. Clock types: `RCL_ROS_TIME` (latest value from a
  ROS time source, else falls back to system time), `RCL_SYSTEM_TIME` (system
  clock), `RCL_STEADY_TIME` (monotonic). Includes `rcl_clock_t`,
  `rcl_time_point_t`, and **time-jump callbacks** for simulated time
  (`rcl_clock_change_t`: ROS time activated/deactivated/no-change).

### Cross-cutting utility headers
- `allocator.h`, `types.h`, `macros.h`, `visibility_control.h`,
  `error_handling.h` — the shared conventions (see overview §3).
- `rcl.h` — umbrella convenience header; hosts the Doxygen `\mainpage`.
- `rmw_implementation_identifier_check.h` — sanity check that the loaded rmw
  implementation matches what was compiled against.

---

## 5. Key data structures

| Struct | Header | Role |
| --- | --- | --- |
| `rcl_context_t` | `context.h` | Non-global state of one init→shutdown cycle. Holds `global_arguments`, an opaque `impl` (which owns the `rmw_context_t`, the init-options copy, and the copied `argv`), and an atomically-accessed **instance id**. Nodes, guard conditions, and wait sets are created against it. |
| `rcl_node_t` | `node.h` | `{ rcl_context_t * context; rcl_node_impl_t * impl; }`. The impl holds the `rmw_node_t`, the graph guard condition, the logger name, the fully-qualified name, and a hash map of registered types. Factory for pubs/subs/clients/services/timers. |
| `rcl_publisher_t` / `rcl_subscription_t` | `publisher.h` / `subscription.h` | Thin handles over `rmw_publisher_t` / `rmw_subscription_t`; impl also caches the *actual* negotiated QoS and the message type hash. |
| `rcl_wait_set_t` | `wait.h` | Public arrays of pointers to each waitable kind (subscriptions, guard conditions, timers, clients, services, events) + counts + opaque `impl`. The heart of the event loop. |
| `rcl_clock_t` / `rcl_time_point_t` | `time.h` | Clock abstraction (ROS/system/steady) with jump callbacks for sim time. |
| `rcl_arguments_t` | `arguments.h` | Parsed `--ros-args` (remaps, params files, log levels, enclave, …). Lives on the context as `global_arguments`. |
| `rcl_ret_t` | `types.h` | Return code (overview §3.5). |
| `rcl_allocator_t` | `allocator.h` | Pluggable allocator carried through the API. |

### The impl pattern, concretely

The handle/impl split (overview §3.2) plays out across these private headers
under `src/rcl/`: `context_impl.h`, `node_impl.h`, `publisher_impl.h`,
`subscription_impl.h`, `client_impl.h`, `service_impl.h`, `guard_condition_impl.h`,
`timer_impl.h`, `event_impl.h`, `arguments_impl.h`, `init_options_impl.h`,
`remap_impl.h`. Example:

```c
// src/rcl/publisher_impl.h  (private)
struct rcl_publisher_impl_s {
  rcl_publisher_options_t options;
  rmw_qos_profile_t actual_qos;   // QoS actually negotiated by the middleware
  rcl_context_t * context;
  rmw_publisher_t * rmw_handle;   // the underlying middleware object
  rosidl_type_hash_t type_hash;
};
```

---

## 6. Two end-to-end flows worth tracing

Reading these two paths teaches most of the codebase.

### 6.1 Startup: `rcl_init()` (`src/rcl/init.c`)

What `rcl_init(argc, argv, options, context)` does, condensed:

1. Validate args; reject an already-initialized context
   (`context->impl != NULL` → `RCL_RET_ALREADY_INIT`).
2. Allocate and zero the `rcl_context_impl_t`; stash the allocator.
3. Zero-init the `rmw_context` inside it.
4. Deep-copy the init options into the context (`rcl_init_options_copy`).
5. Deep-copy `argv` into the context (the caller's memory is not retained).
6. Generate a process-unique **instance id** (atomic); store it both in the
   context's `instance_id_storage` and in the rmw init options.
7. Resolve the **domain id** and **discovery options** into the rmw init options.
8. Resolve the **enclave** and **security options** from the environment
   (`rcl_get_security_options_from_environment`).
9. Call **`rmw_init(rmw_init_options, rmw_context)`** — this is where the actual
   middleware comes up.
10. On any failure, unwind everything allocated so far.

`rcl_shutdown()` mirrors this: `rmw_shutdown(rmw_context)`, then reset the
instance id to `0` (which is what makes `rcl_context_is_valid()` return false);
later `rcl_context_fini()` frees the impl. The instance id is stored **outside**
the impl precisely so validity can be checked race-free even while the context is
being torn down (see the long comment in `context.h`).

### 6.2 Steady state: `rcl_wait()` (`src/rcl/wait.c`)

The blocking primitive an executor sits on:

1. `rcl_get_zero_initialized_wait_set()`, then `rcl_wait_set_init(ws, n_subs,
   n_guards, n_timers, n_clients, n_services, n_events, context, allocator)` to
   size it.
2. Each spin: `rcl_wait_set_clear(ws)`, then `rcl_wait_set_add_*` the current
   entities (each add records the entity's index).
3. `rcl_wait(ws, timeout)` blocks until at least one entity is ready or the
   timeout elapses. Internally it gathers the underlying `rmw` waitables (and
   converts timers to a computed timeout), calls `rmw_wait`, then **nulls out the
   array slots of entities that are *not* ready**.
4. The caller iterates the arrays: a **non-NULL slot means "ready"** → take the
   message / call the callback / fire the timer.

Understanding "NULL slot after `rcl_wait` == not ready" is the key to reading any
executor implementation in `rclcpp`.

---

## 7. Tests

Tests live in `test/` and build only when `BUILD_TESTING` is on. They are the
best place to learn intended usage.

- **`test/rcl/test_*.cpp`** — gtest unit/integration tests, roughly one file per
  module (`test_init.cpp`, `test_node.cpp`, `test_publisher.cpp`, `test_wait.cpp`,
  `test_graph.cpp`, `test_arguments.cpp`, `test_time.cpp`, …). C++ for gtest
  ergonomics even though the library is C.
- **`test/rcl/*.py.in`** — `launch_testing` integration tests for multi-process
  scenarios (`test_two_executables.py.in`, `test_rmw_impl_id_check.py.in`).
- **`test/mocking_utils/`** — Mimick-based mocks to force `rmw`/syscall failures.
- **`test/rcl/*_testing_utils.h`, `failing_allocator_functions.hpp`,
  `wait_for_entity_helpers.*`, fixtures** — shared harness. The failing allocator
  + fault injection (overview §3.8) drive the error-path coverage QL1 requires.
- **`test/resources/`** — fixture data (security enclaves, argument files, QoS
  profiles).

Many tests are parameterized over the available rmw implementation via
`rmw_implementation_cmake`.

---

## 8. Suggested reading order within `rcl`

1. **`include/rcl/context.h`** — the lifecycle model and ASCII state diagram; the
   mental model for the whole library.
2. **`include/rcl/types.h`**, **`allocator.h`**, **`error_handling.h`** — the
   cross-cutting conventions (overview §3).
3. **`include/rcl/init.h`** → **`src/rcl/init.c`** — trace startup (§6.1).
4. **`include/rcl/node.h`** + **`src/rcl/node_impl.h`** — the entity factory and
   the handle/impl pattern.
5. **`publisher.h` + `publisher_impl.h` + `publisher.c`** — one full entity
   end-to-end; then notice `subscription`, `client`, `service`, `timer` share the
   same shape.
6. **`wait.h`** → **`src/rcl/wait.c`** — the event-loop primitive (§6.2).
7. Pick a feature area you care about (graph, arguments/remap, time, security)
   and read its header + `.c` + matching `test_*.cpp`.

---

## 9. Quick reference: header → responsibility

| Header (`include/rcl/`) | Responsibility |
| --- | --- |
| `init.h`, `init_options.h`, `context.h` | Process/library lifecycle and per-cycle state |
| `node.h`, `node_options.h` | Node entity; factory for all comm entities |
| `publisher.h`, `subscription.h` | Topic publish / subscribe |
| `client.h`, `service.h`, `service_introspection.h` | Service client / server + introspection events |
| `timer.h` | Timers |
| `event.h`, `event_callback.h` | QoS/status events |
| `guard_condition.h` | Manual wake source for wait sets |
| `wait.h` | Wait set + `rcl_wait()` (the blocking primitive) |
| `graph.h` | Network/graph discovery & change notification |
| `arguments.h`, `remap.h`, `lexer.h`, `lexer_lookahead.h` | ROS CLI args, remapping, parsing |
| `validate_topic_name.h`, `validate_enclave_name.h`, `expand_topic_name.h` | Name validation & expansion |
| `domain_id.h`, `discovery_options.h`, `network_flow_endpoints.h` | Middleware networking config & introspection |
| `security.h` | SROS2 enclave / security options |
| `log_level.h`, `logging.h`, `logging_rosout.h` | Logging config and `/rosout` |
| `time.h` | Clocks and time sources (incl. sim time) |
| `dynamic_message_type_support.h`, `type_description_conversions.h`, `type_hash.h`, `node_type_cache.h` | Runtime type info & type-description service |
| `allocator.h`, `types.h`, `macros.h`, `error_handling.h`, `visibility_control.h`, `rcl.h` | Cross-cutting utilities & umbrella header |
| `rmw_implementation_identifier_check.h` | rmw implementation consistency check |
```
