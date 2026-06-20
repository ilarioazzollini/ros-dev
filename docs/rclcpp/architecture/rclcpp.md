# `rclcpp` — Core Package Deep Dive

> Deep dive into the **`rclcpp`** package, the core C++ client library. Read the
> [architecture overview](./architecture.md) first — this document assumes the
> **shared C++ conventions** described there (§3: smart-pointer ownership/RAII,
> `create_*` factories, templates + type erasure, the node-interfaces
> composition, exceptions instead of return codes, options structs, and the
> executor/spin model). Those are not repeated here; this doc focuses on what is
> specific to the core package.
>
> Source: `repos/rclcpp/rclcpp/`, `rolling` line.

---

## 1. What `rclcpp` is

`rclcpp` is the **C++ API developers use to write ROS 2 nodes**. It wraps the C
layer ([`rcl`](../../rcl/architecture/rcl.md)) and turns its manually-managed C
entities into type-safe, RAII C++ objects, then adds the runtime that `rcl`
deliberately omits: the **executor/spin model**.

Mental model: **almost every `rclcpp` class is a C++ owner of an `rcl_*` handle.**
For example, `rclcpp::PublisherBase` holds a `std::shared_ptr<rcl_publisher_t>`
and a `std::shared_ptr<rcl_node_t>`; its destructor finalizes them. The C++ layer
adds templates (typed messages), smart-pointer lifetimes, exceptions, and the
event loop on top of that handle.

It is a large package — ~167 public headers, ~92 `.cpp` files. The size is mostly
breadth (many entity types and options) plus template code that must live in
headers; the *concepts* are far fewer than the file count suggests, and this doc
maps them.

---

## 2. Package layout

```
repos/rclcpp/rclcpp/
├── CMakeLists.txt                 # builds librclcpp (C++17)
├── package.xml
├── include/rclcpp/
│   ├── rclcpp.hpp                 # umbrella header (#include this in apps)
│   ├── *.hpp                      # ~85 top-level public headers (entities, options, utils)
│   ├── node_interfaces/           # the Node, decomposed into aspects (§6) — KEY
│   ├── executors/                 # SingleThreaded / MultiThreaded / EventsCBG executors
│   ├── strategies/                # memory strategies for executors
│   ├── wait_set_policies/         # policy mixins composing WaitSet behavior
│   ├── topic_statistics/          # subscription statistics collection
│   ├── allocator/                 # allocator adapters bridging std<->rcl
│   ├── contexts/                  # default context implementation
│   ├── dynamic_typesupport/       # runtime/dynamic message types
│   ├── experimental/              # intra-process + experimental executors (unstable)
│   └── detail/                    # private template helpers — treat as internal
└── src/rclcpp/
    ├── *.cpp                      # non-template definitions + type-erased bases
    └── (same subdirs as include)
```

Build facts: one shared library `librclcpp`, **C++17**. Because so much is
templated, a large fraction of the library is header-only; `src/` holds the
non-templated parts — the type-erased base classes (`publisher_base.cpp`,
`subscription_base.cpp`), the executors, context/init, parameters, time, and the
intra-process manager.

The `src/rclcpp/*.cpp` list doubles as a **subsystem inventory**: `context.cpp`,
`node.cpp`, `executor.cpp`/`executors.cpp`, `callback_group.cpp`, `client.cpp`,
`service.cpp`, `timer.cpp`, `clock.cpp`/`time.cpp`/`time_source.cpp`,
`parameter*.cpp`, `qos.cpp`, `serialization.cpp`, `graph_listener.cpp`,
`guard_condition.cpp`, `waitable.cpp`, `intra_process_manager.cpp`,
`signal_handler.cpp`, etc.

---

## 3. Dependencies (and what each is for)

| Dependency | Role |
| --- | --- |
| **`rcl`** | The C client library `rclcpp` wraps. Every entity owns `rcl_*` handles. |
| **`rcl_yaml_param_parser`** | Parameter YAML parsing (used through `rcl`). |
| **`rcpputils`** | C++ utilities (`shared_library`, asserts, scope_exit, thread-safety helpers). The C++ analogue of `rcutils`. |
| **`rcutils`** | Low-level C utilities (logging, allocators) — used directly in places. |
| **`rmw`** | Middleware types surfaced in the API (QoS profiles, message info). |
| **`rosidl_runtime_cpp` / `rosidl_typesupport_cpp`** | C++ message/service type support — what makes `Publisher<T>` work. |
| **`rcl_interfaces`, `rosgraph_msgs`, `statistics_msgs`, `builtin_interfaces`** | Generated message types for parameters, `/rosout`/clock, statistics, time. |
| **`libstatistics_collector`** | Topic statistics computation. |
| **`rosidl_dynamic_typesupport`** | Runtime/dynamic typing support. |
| **`tracetools`** | LTTng tracepoints. |

---

## 4. Module map

Grouped by concept (the `.hpp` files under `include/rclcpp/`):

### Process lifecycle & context
- `utilities.hpp` — `rclcpp::init()`, `shutdown()`, `ok()`, `spin()` free
  functions; the usual program entry/exit points.
- `context.hpp`, `contexts/`, `init_options.hpp` — `Context` (C++ owner of
  `rcl_context_t`); a process can have several. `signal_handler.cpp` wires Ctrl-C
  to shutdown.

### Nodes
- `node.hpp` / `node_impl.hpp` / `node_options.hpp` — `rclcpp::Node`, the main
  entry point and the factory for all entities (`create_publisher<T>()`, …).
- `node_interfaces/` — the aspects `Node` is composed of (see §6). **The single
  most important directory in the package.**

### Communication entities (all templated on message/service type)
- `publisher.hpp` + `publisher_base.hpp` + `publisher_factory.hpp` +
  `publisher_options.hpp` — typed publisher over a type-erased base.
- `subscription.hpp` + `subscription_base.hpp` + `subscription_factory.hpp` +
  `subscription_options.hpp` + callback machinery
  (`any_subscription_callback.hpp`).
- `client.hpp` / `service.hpp` (+ `any_service_callback.hpp`) — service client &
  server.
- `generic_publisher.hpp` / `generic_subscription.hpp` / `generic_client.hpp` /
  `generic_service.hpp` — type-erased variants for tools that don't know the type
  at compile time.
- `create_*.hpp` — the factory free functions behind the `Node::create_*`
  methods.

### Execution model (rclcpp's defining addition — see overview §3.7)
- `executor.hpp`, `executors.hpp`, `executors/` — the spin loop and its
  implementations.
- `callback_group.hpp` — `MutuallyExclusive` vs `Reentrant` concurrency control.
- `waitable.hpp`, `any_executable.hpp` — the unit of "something waitable that can
  be executed."
- `memory_strategy.hpp`, `strategies/` — how executors allocate the collections
  they wait on.

### Waiting (lower-level, executor-free)
- `wait_set.hpp`, `wait_set_template.hpp`, `wait_set_policies/` — a directly-usable
  wait set (policy-based design) for code that wants to wait without an executor.
- `wait_for_message.hpp`, `guard_condition.hpp`.

### Parameters
- `parameter.hpp`, `parameter_value.hpp`, `parameter_client.hpp`,
  `parameter_service.hpp`, `parameter_event_handler.hpp`,
  `parameter_events_filter.hpp`, `parameter_map.hpp`,
  `qos_overriding_options.hpp` — the full parameter system (declare/get/set,
  remote access, change notifications, QoS overrides via parameters).

### Time
- `time.hpp`, `duration.hpp`, `clock.hpp`, `rate.hpp`, `time_source.hpp` — C++
  wrappers over `rcl`'s clocks; `TimeSource` drives `/clock`-based simulated time.

### QoS, messages, serialization, types
- `qos.hpp`, `qos_overriding_options.hpp` — fluent QoS profile builder.
- `serialization.hpp`, `serialized_message.hpp`, `loaned_message.hpp`,
  `message_info.hpp`, `message_memory_strategy.hpp`.
- `type_adapter.hpp`, `type_support_decl.hpp`, `typesupport_helpers.hpp`,
  `get_message_type_support_handle.hpp`, `is_ros_compatible_type.hpp` — the
  type-support / type-adaptation machinery behind the templates.

### Graph, events, logging, exceptions, misc
- `graph_listener.hpp` — background thread watching graph changes.
- `event_handler.hpp`, `event.hpp` — QoS/status event callbacks.
- `logger.hpp`, `logging.hpp` — `RCLCPP_INFO(...)` and friends.
- `exceptions.hpp`, `exceptions/` — the exception hierarchy + `throw_from_rcl_error`.
- `macros.hpp` (smart-ptr defs), `function_traits.hpp` (callback signature
  deduction), `visibility_control.hpp`.

---

## 5. Key abstractions

| Type | Header | Role |
| --- | --- | --- |
| `Context` | `context.hpp` | C++ owner of `rcl_context_t`; per-process init/shutdown state. |
| `Node` | `node.hpp` | The developer entry point and entity factory. Internally an aggregate of node interfaces (§6). |
| `Node...Interface` (×11) | `node_interfaces/` | The decomposed aspects of a node; generic code depends on these, not on `Node`. |
| `Publisher<MsgT>` / `Subscription<MsgT>` | `publisher.hpp` / `subscription.hpp` | Typed entities; derive from `PublisherBase` / `SubscriptionBase`, which hold the `std::shared_ptr<rcl_*_t>` handle. |
| `Client<SrvT>` / `Service<SrvT>` | `client.hpp` / `service.hpp` | Typed service client/server. |
| `Executor` (+ `SingleThreaded` / `MultiThreaded` / `EventsCBG`) | `executor.hpp`, `executors/` | The spin loop; owns wait/dispatch (§7.2). |
| `CallbackGroup` | `callback_group.hpp` | Concurrency policy for a group of entities. |
| `Waitable` / `AnyExecutable` | `waitable.hpp` / `any_executable.hpp` | The type-erased "waitable thing" + a resolved "ready unit of work." |
| `WaitSet` | `wait_set.hpp` | Executor-free waiting; composed from `wait_set_policies/`. |
| `Parameter` / `ParameterValue` | `parameter.hpp` / `parameter_value.hpp` | A parameter and its variant value. |
| `QoS` | `qos.hpp` | Fluent wrapper over `rmw_qos_profile_t`. |
| `Clock` / `Time` / `Duration` | `clock.hpp` / `time.hpp` / `duration.hpp` | Time abstractions over `rcl`. |

---

## 6. The Node-interfaces composition (the signature pattern)

`rclcpp::Node` is **not a monolithic class** — it is an aggregate of focused
*interface* objects, each living in `node_interfaces/` as an abstract base
(`Node<Aspect>Interface`) with a concrete implementation (`Node<Aspect>`):

| Interface | Owns / does |
| --- | --- |
| `NodeBaseInterface` | the underlying `rcl_node_t`, the node's `Context`, name/namespace |
| `NodeGraphInterface` | graph queries + the `GraphListener` |
| `NodeTopicsInterface` | creates and tracks publishers/subscriptions |
| `NodeServicesInterface` | creates and tracks services/clients |
| `NodeTimersInterface` | tracks timers |
| `NodeParametersInterface` | declare/get/set parameters, parameter callbacks |
| `NodeClockInterface` | the node clock |
| `NodeTimeSourceInterface` | attaches the clock to `/clock` (sim time) |
| `NodeLoggingInterface` | the node logger |
| `NodeWaitablesInterface` | tracks `Waitable`s |
| `NodeTypeDescriptionsInterface` | the `~/get_type_description` service |

`Node` exposes each via `get_node_<aspect>_interface()`. **Why this is the most
important pattern to internalize:**

1. **Decoupling.** A `create_publisher` factory needs only a
   `NodeTopicsInterface`, not a whole `Node`. Generic/library code accepts the
   narrow interface it actually uses.
2. **Substitutability.** `rclcpp_lifecycle::LifecycleNode` is a *different* class
   that exposes the *same* interfaces, so it works everywhere a node is expected
   (executors, components, `rclcpp_action`). This is the mechanism that makes
   lifecycle nodes and composable components possible.

When tracing "how does X get created / wired up," you almost always pass through
one of these interfaces. Read `node_interfaces/node_base_interface.hpp` and
`node_topics_interface.hpp` first.

---

## 7. Two end-to-end flows worth tracing

### 7.1 Creating and using a publisher

```cpp
auto node = std::make_shared<rclcpp::Node>("talker");
auto pub  = node->create_publisher<std_msgs::msg::String>("chatter", 10);
pub->publish(msg);
```

1. `Node::create_publisher<T>(...)` → the `create_publisher` factory
   (`create_publisher.hpp`), which uses the **`NodeTopicsInterface`** to build the
   entity and register it on the node.
2. Construction creates a `Publisher<T>` (deriving from `PublisherBase`), which
   calls `rcl_publisher_init` and stores the resulting
   `std::shared_ptr<rcl_publisher_t>`. RAII: `~PublisherBase` calls the matching
   `rcl` fini.
3. `publish(msg)` chooses a path: **inter-process**
   (`do_inter_process_publish` → `rcl_publish`) and/or **intra-process** (via the
   `experimental::IntraProcessManager`, which hands the message to in-process
   subscriptions without serialization). Loaned messages and type adapters
   (`type_adapter.hpp`) plug in here.

### 7.2 The executor spin loop

`rclcpp::spin(node)` is sugar for: create a `SingleThreadedExecutor`,
`add_node(node)`, `spin()`. The loop (`executor.cpp`) is:

1. **Collect** — gather the node's entities (subscriptions, timers, services,
   clients, waitables), respecting **callback groups** (`collect_entities`). A
   `MemoryStrategy` (`strategies/`) backs the storage.
2. **Wait** — build an `rcl_wait_set_t` and block on `rcl_wait`
   (`wait_for_work`). This is the same `rcl` primitive described in
   [`rcl.md` §6.2](../../rcl/architecture/rcl.md): after it returns, "not ready"
   slots are nulled.
3. **Pick** — `get_next_ready_executable` resolves one ready entity into an
   `AnyExecutable`.
4. **Execute** — `execute_any_executable` dispatches to the right typed handler:
   `execute_subscription` (take + call the user callback), `execute_timer`,
   `execute_service`, `execute_client`.

`MultiThreadedExecutor` runs steps 3–4 on a thread pool, with `CallbackGroup`
type (mutually-exclusive vs reentrant) deciding what may run concurrently. The
experimental `EventsExecutor` replaces the wait-set polling with an event-driven
queue.

Internalizing "**collect → wait (rcl_wait) → pick → execute callback**" is the
key to the entire runtime side of `rclcpp`.

---

## 8. Tests

`test/` is extensive (gtest + gmock, with `launch_testing` for multi-process
scenarios), broadly mirroring the source: per-entity tests
(`test_publisher.cpp`, `test_subscription.cpp`, `test_service.cpp`,
`test_client.cpp`, `test_node.cpp`, `test_timer.cpp`), the execution model
(`executors/`, `test_executor*.cpp`, callback-group tests), parameters, QoS, wait
sets, intra-process, serialization, and the node interfaces. As in `rcl`, tests
are the best executable usage reference, and many use fault injection / mocking.

---

## 9. Suggested reading order within `rclcpp`

1. **`include/rclcpp/node.hpp`** — the API surface and the `create_*` /
   `get_node_*_interface` methods; the developer's mental model.
2. **`node_interfaces/node_base_interface.hpp` + `node_topics_interface.hpp`** —
   the composition pattern (§6); the key to how everything is wired.
3. **`publisher_base.hpp` + `publisher.hpp`** — one entity end-to-end, including
   the `rcl` handle ownership; then `subscription*` mirrors it.
4. **`executor.hpp` → `src/rclcpp/executor.cpp`** — the spin loop (§7.2); then
   `callback_group.hpp` and `executors/single_threaded_executor.hpp`.
5. **`context.hpp` + `utilities.hpp`** — init/shutdown/spin entry points.
6. Pick a subsystem you care about (parameters, time/`time_source`, intra-process
   under `experimental/`, QoS) and read its header + `.cpp` + test.

---

## 10. Quick reference: header → responsibility

| Header (`include/rclcpp/`) | Responsibility |
| --- | --- |
| `rclcpp.hpp` | Umbrella include for applications |
| `utilities.hpp`, `context.hpp`, `init_options.hpp`, `contexts/` | init / shutdown / spin / process context |
| `node.hpp`, `node_options.hpp`, `node_interfaces/` | Node + its decomposed interfaces |
| `publisher*.hpp`, `subscription*.hpp` | Typed pub/sub over type-erased bases |
| `client.hpp`, `service.hpp`, `any_service_callback.hpp` | Service client/server |
| `generic_*.hpp` | Type-erased pub/sub/client/service |
| `create_*.hpp` | Entity factory free functions |
| `executor.hpp`, `executors.hpp`, `executors/`, `strategies/`, `memory_strategy.hpp` | Spin loop + implementations |
| `callback_group.hpp`, `waitable.hpp`, `any_executable.hpp` | Concurrency + waitable units |
| `wait_set*.hpp`, `wait_set_policies/`, `wait_for_message.hpp`, `guard_condition.hpp` | Executor-free waiting |
| `parameter*.hpp`, `qos_overriding_options.hpp` | Parameter system |
| `time.hpp`, `duration.hpp`, `clock.hpp`, `rate.hpp`, `time_source.hpp` | Time / clocks / sim time |
| `qos.hpp` | QoS profile builder |
| `serialization.hpp`, `serialized_message.hpp`, `loaned_message.hpp`, `message_info.hpp` | Messages & serialization |
| `type_adapter.hpp`, `type_support_decl.hpp`, `typesupport_helpers.hpp`, `dynamic_typesupport/` | Type support & adaptation |
| `graph_listener.hpp`, `event_handler.hpp`, `event.hpp` | Graph & QoS-event notification |
| `logger.hpp`, `logging.hpp` | Logging macros |
| `exceptions.hpp`, `exceptions/` | Exception hierarchy + rcl-error conversion |
| `macros.hpp`, `function_traits.hpp`, `visibility_control.hpp`, `detail/` | Cross-cutting C++ machinery |
| `experimental/` | Intra-process + experimental executors (unstable) |
```
