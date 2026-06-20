# ROS 2 `rclcpp` Repository — Architecture Overview

> A technical orientation for developers who want to start exploring and
> contributing to the [`rclcpp`](https://github.com/ros2/rclcpp) repository.
>
> This document is the **hub**: it explains where the repo sits in the ROS 2
> stack, what the four packages are and how they relate, and — most importantly —
> the **C++ conventions shared by all of them**. Each package then gets its own
> deep-dive document:
>
> - [`rclcpp.md`](./rclcpp.md) — the core C++ client library
> - [`rclcpp_action.md`](./rclcpp_action.md) — C++ Actions
> - [`rclcpp_lifecycle.md`](./rclcpp_lifecycle.md) — C++ managed/lifecycle nodes
> - [`rclcpp_components.md`](./rclcpp_components.md) — dynamically loadable node components
>
> Source analyzed: the `rolling` line, as checked out under `repos/rclcpp/`.
>
> Companion: the [`rcl` architecture docs](../../rcl/architecture/architecture.md)
> describe the C layer this repo wraps; this doc assumes familiarity with it.

---

## 1. Where this repo sits in the ROS 2 stack

`rclcpp` is the **C++ client library** of ROS 2 — the API application developers
actually use to write C++ nodes. It is an **ergonomic, type-safe, RAII C++
wrapper over `rcl`** (the common C layer). It does not talk to the middleware
(`rmw`) directly for the most part; it builds on `rcl`'s managed C entities and
adds everything that makes ROS 2 pleasant in C++: templates over message types,
smart-pointer ownership, exceptions, parameter/callback ergonomics, and — crucially
— the **executor/spin model** that `rcl` deliberately leaves out.

```
        ┌──────────────────────────────────────────────┐
        │   Your C++ node / application                 │
        └──────────────────────────────────────────────┘
                          │  #include "rclcpp/rclcpp.hpp"
        ┌─────────────────▼──────────────────────────────┐
        │            THIS REPO (C++17)                    │
        │  rclcpp · rclcpp_action · rclcpp_lifecycle ·    │
        │  rclcpp_components                              │
        │  + executors / spinning / callbacks            │
        └─────────────────┬──────────────────────────────┘
                          │  (C ABI)
        ┌─────────────────▼──────────────────────────────┐
        │   rcl  (+ rcl_action / rcl_lifecycle / …)       │   ← common C layer
        └─────────────────┬──────────────────────────────┘
                          │
        ┌─────────────────▼──────────────────────────────┐
        │      rmw  +  rmw_implementation (DDS, etc.)     │
        └─────────────────────────────────────────────────┘
```

**Division of labor with `rcl`.** `rcl` owns the lifecycle and validation of the
underlying middleware entities (it is a "managed wrapper over `rmw`"). `rclcpp`
owns **developer experience and runtime orchestration**: type-safe pub/sub,
ownership via `std::shared_ptr`, exceptions instead of return codes, parameters,
and the event loop (executors, callback groups, waitables). The single biggest
thing `rclcpp` adds that has no `rcl` equivalent is **spinning**: waiting for work
and dispatching the right C++ callback.

---

## 2. The four packages

The git repository is a **multi-package repo** with four ament/CMake packages
that mirror the `rcl` repo's layout:

| Package | Depends on | Responsibility | Deep dive |
| --- | --- | --- | --- |
| **`rclcpp`** | `rcl`, `rcl_yaml_param_parser`, `rcpputils`, `rcutils`, `rmw`, `libstatistics_collector`, … | The core C++ API: nodes, pub/sub, services, clients, timers, parameters, **executors & spinning**, wait sets, QoS, time, logging, serialization, intra-process. | [rclcpp.md](./rclcpp.md) |
| **`rclcpp_action`** | **`rclcpp`**, `rcl_action`, `action_msgs`, `rcpputils` | C++ Actions: typed action client/server, goal handles with futures/callbacks — built on `rcl_action` + `rclcpp`. | [rclcpp_action.md](./rclcpp_action.md) |
| **`rclcpp_lifecycle`** | **`rclcpp`**, `rcl_lifecycle`, `lifecycle_msgs` | C++ managed nodes: `LifecycleNode` and lifecycle-aware publishers wrapping `rcl_lifecycle`'s state machine. | [rclcpp_lifecycle.md](./rclcpp_lifecycle.md) |
| **`rclcpp_components`** | **`rclcpp`**, `class_loader`, `composition_interfaces` | Tooling to compile nodes as **dynamically loadable components** and compose multiple nodes in one process at runtime. | [rclcpp_components.md](./rclcpp_components.md) |

### Dependency direction

```
   rclcpp_action ────┐
   rclcpp_lifecycle ─┼──▶ rclcpp ──▶ rcl (+ rcl_action / rcl_lifecycle) ──▶ rmw
   rclcpp_components ┘
```

All three satellite packages **build on `rclcpp`** and don't depend on each
other. `rclcpp_action` and `rclcpp_lifecycle` additionally wrap their `rcl_*`
counterparts; `rclcpp_components` is pure C++ tooling (no `rcl_*` counterpart).

---

## 3. C++ conventions shared by every package

This is the highest-leverage section. All four packages share a consistent C++
style; learning it once lets you read any file. Each package doc assumes you have
read this section. (These are the C++ analogues of the C conventions in the
[`rcl` overview §3](../../rcl/architecture/architecture.md).)

### 3.1 Smart-pointer ownership everywhere

Entities are heap-allocated and owned via `std::shared_ptr`; you almost never
construct them with `new` or on the stack. Each class declares standard pointer
aliases through macros in `macros.hpp`:

```cpp
class Publisher {
public:
  RCLCPP_SMART_PTR_DEFINITIONS(Publisher)  // ::SharedPtr, ::ConstSharedPtr,
                                           // ::WeakPtr, ::UniquePtr + make_shared
};
```

So you'll constantly see `rclcpp::Publisher<T>::SharedPtr`,
`Node::SharedPtr`, etc. `RCLCPP_DISABLE_COPY` and the
`..._NOT_COPYABLE` / `..._ALIASES_ONLY` variants express ownership intent.
**Lifetime is RAII**: destructors call the matching `rcl_*_fini`, so there are no
manual `fini` calls in user code (contrast the C layer's explicit quartet).

### 3.2 Factory free functions: `create_*`

Rather than calling constructors directly, entities are created through
`create_publisher<T>()`, `create_subscription<T>()`, `create_service<T>()`,
`create_client<T>()`, `create_timer()`, etc. (and their `create_generic_*`
variants). `Node`'s member functions (`node->create_publisher<T>(...)`) are thin
wrappers over these. The factories live in `create_*.hpp` headers and wire the new
entity into the node's interfaces (§3.4).

### 3.3 Templates over message types + type erasure

Pub/sub/service/client are **class templates** parameterized on the ROS message or
service type (`Publisher<std_msgs::msg::String>`). A non-templated base class
(`PublisherBase`, `SubscriptionBase`, …) holds the type-erased machinery so
executors and wait sets can handle entities uniformly. `type_adapter.hpp` and the
typesupport headers let you publish/subscribe with custom C++ types mapped to ROS
messages.

### 3.4 The Node is composed of *interfaces* (the most important rclcpp pattern)

`rclcpp::Node` is not a monolith — it is an aggregate of focused **node interface**
aspects under `node_interfaces/`, each an abstract base with a concrete impl:

`NodeBaseInterface`, `NodeGraphInterface`, `NodeTopicsInterface`,
`NodeServicesInterface`, `NodeTimersInterface`, `NodeParametersInterface`,
`NodeClockInterface`, `NodeLoggingInterface`, `NodeWaitablesInterface`,
`NodeTimeSourceInterface`, `NodeTypeDescriptionsInterface`.

`Node` exposes them via `get_node_base_interface()`, `get_node_topics_interface()`,
and so on. **Why it matters:** generic code (executors, components,
`rclcpp_action`, `rclcpp_lifecycle`) accepts the *interfaces* it needs rather than
a concrete `Node`, which is exactly how `LifecycleNode` can be a different class
yet work everywhere a node is expected. When tracing how something gets created or
wired, you almost always pass through a `Node...Interface`.

### 3.5 Exceptions, not return codes

Where `rcl` returns `rcl_ret_t`, `rclcpp` **throws**. Errors surface as C++
exceptions defined in `exceptions.hpp` / `exceptions/` (e.g.
`rclcpp::exceptions::RCLError` and friends); `rclcpp::exceptions::throw_from_rcl_error()`
converts a failed `rcl_ret_t` (plus its error string) into the right exception
type. So idiomatic `rclcpp` code uses normal C++ error handling, and you rarely
see raw return-code checks.

### 3.6 Options structs (mirrors the C layer)

Configuration is passed via options objects — `NodeOptions`, `PublisherOptions`,
`SubscriptionOptions`, `ExecutorOptions`, `InitOptions` — typically built with
sensible defaults and customized fluently before being handed to a `create_*`
call. These wrap the corresponding `rcl_*_options_t`.

### 3.7 The execution model: executors, callback groups, waitables

This is `rclcpp`'s defining addition over `rcl`. The pieces:

- **Waitable** — anything that can be waited on and then "executed" (subscriptions,
  timers, services, clients, guard conditions, and composites like actions).
- **CallbackGroup** — groups entities to control concurrency (mutually exclusive
  vs. reentrant).
- **Executor** — owns the spin loop: it builds a wait set from a node's entities,
  blocks on `rcl_wait`, then dispatches ready entities to their callbacks.
  Implementations live in `executors/`: `SingleThreadedExecutor`,
  `MultiThreadedExecutor`, plus the static entity-collection machinery. The
  familiar `rclcpp::spin(node)` is sugar over an executor.

Understanding "executor → wait set → dispatch callback" is the key to reading the
runtime side of the library, and ties directly back to `rcl`'s
[`rcl_wait()` model](../../rcl/architecture/rcl.md#62-steady-state-rcl_wait-srcrclwaitc).

### 3.8 The `detail` and `experimental` namespaces

Implementation helpers that must live in headers (templates) are placed in
`namespace detail` / `include/rclcpp/detail/` — treat these as private. The
`experimental/` headers (notably intra-process communication) are subject to
change and not part of the stable API.

---

## 4. Build system

All packages are `ament_cmake`, C++17, built with `colcon`. Per package:

- One primary shared library per package (`librclcpp`, `librclcpp_action`, …);
  `rclcpp_components` also installs node-registration CMake macros and executors.
- `find_package(...)` for each dependency, mirrored by `<depend>` in
  `package.xml`.
- Heavy template use means much of the library is header-only; `src/` holds the
  non-templated definitions and the type-erased base classes.
- Tests use `ament_cmake_gtest` / `ament_cmake_gmock` and `launch_testing`;
  linting via `ament_lint_auto`.

In this workspace the build is driven by the helper scripts in the repo root
`README.md` (`scripts/clean_build.sh` full, `scripts/build.sh` incremental), which
build `rcl` and `rclcpp` together.

---

## 5. Suggested reading order for the whole repo

1. **This document** — the stack, the packages, and the C++ conventions (§3).
2. **[`rclcpp.md`](./rclcpp.md)** — start here; everything builds on it. Within it,
   read the node-interfaces composition, one entity end-to-end (publisher), then
   the executor/spin model.
3. **[`rclcpp_components.md`](./rclcpp_components.md)** — small and conceptually
   self-contained (dynamic loading + composition); a good second read.
4. **[`rclcpp_action.md`](./rclcpp_action.md)** and
   **[`rclcpp_lifecycle.md`](./rclcpp_lifecycle.md)** — the two packages that wrap
   their `rcl_*` counterparts; read in either order, ideally alongside the
   matching `rcl` package doc.

---

## 6. Document status

| Document | Status |
| --- | --- |
| `architecture.md` (this file) | ✅ overview |
| `rclcpp.md` | ✅ written |
| `rclcpp_action.md` | ✅ written |
| `rclcpp_lifecycle.md` | ✅ written |
| `rclcpp_components.md` | ✅ written |
```
