# ROS 2 `rcl` Repository — Architecture Overview

> A technical orientation for developers who want to start exploring and
> contributing to the [`rcl`](https://github.com/ros2/rcl) repository.
>
> This document is the **hub**: it explains where the repo sits in the ROS 2
> stack, what the four packages are and how they relate, and — most importantly —
> the **conventions shared by all of them**. Each package then gets its own
> deep-dive document:
>
> - [`rcl.md`](./rcl.md) — the core client-library implementation
> - [`rcl_action.md`](./rcl_action.md) — ROS 2 Actions
> - [`rcl_lifecycle.md`](./rcl_lifecycle.md) — managed/lifecycle nodes
> - [`rcl_yaml_param_parser.md`](./rcl_yaml_param_parser.md) — YAML parameter parsing
>
> Source analyzed: the `rolling` line (`rcl` package version `10.5.1`), as
> checked out under `repos/rcl/`.

---

## 1. Where this repo sits in the ROS 2 stack

The `rcl` repo provides the **common, language-agnostic C layer** of the ROS 2
client-library stack. It sits between the middleware abstraction (`rmw`) and the
language-specific client libraries (`rclcpp`, `rclpy`, …).

```
        ┌──────────────────────────────────────────────┐
        │   Application / Node code                    │
        └──────────────────────────────────────────────┘
                          │
        ┌─────────────────┴───────────┬──────────────────┐
        │  rclcpp (C++)               │  rclpy (Python)  │   ← language client libs
        └─────────────────┬───────────┴──────────────────┘
                          │  (C ABI)
        ┌─────────────────▼──────────────────────────────┐
        │            THIS REPO (pure C)                  │
        │  rcl · rcl_action · rcl_lifecycle ·            │
        │  rcl_yaml_param_parser                         │
        └─────────────────┬──────────────────────────────┘
                          │
        ┌─────────────────▼──────────────────────────────┐
        │      rmw  +  rmw_implementation (DDS, etc.)    │   ← middleware abstraction
        └─────────────────┬──────────────────────────────┘
                          │
        ┌─────────────────▼──────────────────────────────┐
        │   DDS / Zenoh / other middleware vendor        │
        └────────────────────────────────────────────────┘
```

**Why a common C layer exists.** Every higher-level client library needs the
same logic: validating topic names, expanding/remapping names, parsing ROS
command-line arguments, managing init/shutdown, wiring QoS, talking to the graph,
running the state machines for actions and lifecycle nodes, etc. Rather than
re-implement all of that in C++, Python, Rust, and so on, it lives once here as
**plain C with a stable ABI**, and each language library wraps it. This is why
the whole repo deliberately exposes **C, not C++**.

**What this repo does *not* do:** it does not implement the wire protocol or
transport (that is the middleware vendor behind `rmw`), and it does not provide
executors, spinning, or high-level ergonomics (those live in `rclcpp`/`rclpy`).

---

## 2. The four packages

The git repository is a **multi-package repo** containing four ament/CMake
packages:

| Package | Depends on | Responsibility | Deep dive |
| --- | --- | --- | --- |
| **`rcl`** | `rmw`, `rcutils`, `rosidl_runtime_c`, `rcl_yaml_param_parser`, … | The core: nodes, pub/sub, services, clients, timers, wait sets, graph, command-line args, logging, time, security. Everything else builds on it. | [rcl.md](./rcl.md) |
| **`rcl_action`** | **`rcl`**, `action_msgs`, `rcutils`, `rmw`, `rosidl_runtime_c` | ROS 2 *Actions*: action client/server, goal handles, and the goal **state machine** — built on top of `rcl` topics + services. | [rcl_action.md](./rcl_action.md) |
| **`rcl_lifecycle`** | **`rcl`**, `lifecycle_msgs`, `rcutils`, `rmw`, `tracetools` | *Managed/lifecycle nodes*: the state machine (unconfigured → inactive → active → finalized) plus the ROS service/topic communication interface for driving it. | [rcl_lifecycle.md](./rcl_lifecycle.md) |
| **`rcl_yaml_param_parser`** | `libyaml_vendor`/`yaml`, `rcutils`, `rmw` | Standalone helper that parses a YAML parameter file into a C data structure. Consumed by `rcl`'s argument parsing (`--params-file`). Does **not** depend on `rcl`. | [rcl_yaml_param_parser.md](./rcl_yaml_param_parser.md) |

### Dependency direction

```
   rcl_action ──┐
                ├──▶ rcl ──▶ rcl_yaml_param_parser ──▶ (libyaml)
   rcl_lifecycle┘           │
                            └──▶ rmw / rcutils / rosidl_runtime_c
```

`rcl_action` and `rcl_lifecycle` are **siblings that both build on `rcl`** and do
not depend on each other. `rcl_yaml_param_parser` is a leaf utility that `rcl`
pulls in; notably it does **not** depend on `rcl`, so it can be reused
standalone.

---

## 3. Conventions shared by every package

This is the highest-leverage section. All four packages are written by the same
team in the same style, so learning these conventions **once** lets you read any
file in the repo. Each package doc assumes you have read this section.

### 3.1 Pure C with a stable ABI

Everything is C11 (tests are C++17 for gtest). Public symbols are tagged with a
per-package `<PKG>_PUBLIC` visibility macro (e.g. `RCL_PUBLIC`,
`RCL_ACTION_PUBLIC`); symbol visibility is hidden by default, so only tagged
symbols are exported. Each package has a `visibility_control.h` for this.

### 3.2 The "handle + opaque impl" pattern (PIMPL in C)

Each public entity is a small struct holding a pointer to a **forward-declared**
implementation struct. The full struct is defined in a private `*_impl.h` header
under `src/`, invisible to users:

```c
// include/.../publisher.h  — PUBLIC, stable ABI
typedef struct rcl_publisher_impl_s rcl_publisher_impl_t;  // forward decl only
typedef struct rcl_publisher_s {
  rcl_publisher_impl_t * impl;   // opaque pointer
} rcl_publisher_t;

// src/.../publisher_impl.h  — PRIVATE, full definition (rmw handle, QoS, ...)
struct rcl_publisher_impl_s { /* ... */ };
```

This keeps the ABI stable: implementations can grow new fields without
recompiling downstream code. The pattern repeats for nearly every entity in
every package. Almost every impl ultimately holds an underlying `rmw_*` handle —
these libraries are largely *managed, validated, convention-enforcing wrappers*
around `rmw`.

### 3.3 The lifecycle quartet (naming is mechanical)

For an entity `X`, expect these functions — and once you know one entity, you can
**guess** the rest:

```c
rcl_X_t        rcl_get_zero_initialized_X(void);     // 1. zero-init, no allocation
rcl_ret_t      rcl_X_init(rcl_X_t *, ...options...); // 2. allocate + wire up rmw
rcl_ret_t      rcl_X_fini(rcl_X_t *, ...);           // 3. tear down + free
rcl_X_options_t rcl_X_get_default_options(void);     // 4. default options (most entities)
```

The mandated usage sequence is **always**
`zero_initialized → init → use → fini`. Calling `init` on an already-initialized
handle is an error; re-initialization is only allowed after `fini`. The canonical
example with a full ASCII state diagram is in `rcl`'s `include/rcl/context.h` —
read it early.

### 3.4 Options structs

Configuration is passed via a value struct (`rcl_X_options_t`). You typically
start from `rcl_X_get_default_options()` and override fields before `init`.

### 3.5 Return codes & error handling

- Every fallible function returns `rcl_ret_t` (an alias of `rmw_ret_t`).
- Codes are **namespaced by numeric range** so the failing subsystem is obvious
  (in `rcl/types.h`): generic `RCL_RET_OK/ERROR/BAD_ALLOC/INVALID_ARGUMENT`,
  then `1xx` init, `2xx` node, `3xx` publisher, `4xx` subscription, `5xx` client,
  `6xx` service, `8xx` timer, `9xx` wait set, `10xx` args, `20xx` events,
  `30xx` lifecycle, `40xx` action.
- Alongside the code, a **thread-local error string** is set
  (`rcl/error_handling.h`, re-exporting `rcutils`): `RCL_SET_ERROR_MSG`,
  `rcl_get_error_state`, `rcl_reset_error`. **When debugging, read the error
  state, not just the return code** (e.g. print `rcl_get_error_string().str`).
- `RCL_WARN_UNUSED` marks return values you must not ignore.

### 3.6 Explicit allocators everywhere

Memory is never hidden. An `rcl_allocator_t` (alias of `rcutils_allocator_t`) is
threaded through init calls and stored in impls, so embedders fully control
allocation. `rcl_get_default_allocator()` is the malloc/free default.

### 3.7 Documented behavioral contract on each function

Most public functions carry a Doxygen "Attribute / Adherence" table declaring
whether they **Allocate Memory**, are **Thread-Safe**, **Use Atomics**, and are
**Lock-Free**. These are part of the **Quality Level 1** contract. If you change
a function's behavior, update its table.

### 3.8 Argument checks & fault injection

Functions begin with guard macros like `RCL_CHECK_ARGUMENT_FOR_NULL(...)`. Under
test builds, `RCUTILS_ENABLE_FAULT_INJECTION` is defined so allocation/`rmw`
failure paths can be exercised deterministically — which is why the code is
meticulous about unwinding cleanup on **every** error branch.

### 3.9 The header is the contract; the test is the example

For any feature: the **header** (`include/.../X.h`) is the contract (Doxygen,
attribute tables, error codes); the **`.c`** is the behavior; the
**`test/test_X.cpp`** is an executable usage example. Read all three together —
this is the single best habit for this repo.

---

## 4. Build system

All packages are `ament_cmake` and build with `colcon`. Per package:

- One shared library per package (`librcl`, `librcl_action`, …).
- `find_package(...)` for each dependency in `CMakeLists.txt`, mirrored by
  `<depend>` entries in `package.xml`.
- Tests are gated behind `BUILD_TESTING` and use `ament_add_gtest` /
  `launch_testing`; linting via `ament_lint_auto`.

In this workspace the build is driven by the helper scripts described in the repo
root `README.md` (`scripts/clean_build.sh` for a full build,
`scripts/build.sh` for incremental). They symlink `repos/{rcl,rclcpp}` into
`ros2_ws/src/` and cap parallelism to fit the container's RAM.

---

## 5. Suggested reading order for the whole repo

1. **This document** — the stack, the packages, and the shared conventions (§3).
2. **[`rcl.md`](./rcl.md)** — start here; everything else builds on `rcl`. Within
   it, read `context.h` → the lifecycle model → `rcl_init()` → one entity
   (publisher) end-to-end → `wait.h`/`rcl_wait()`.
3. **[`rcl_yaml_param_parser.md`](./rcl_yaml_param_parser.md)** — small, leaf,
   self-contained; a good second read.
4. **[`rcl_action.md`](./rcl_action.md)** and
   **[`rcl_lifecycle.md`](./rcl_lifecycle.md)** — the two state-machine packages
   layered on `rcl`; read in either order.

---

## 6. Document status

| Document | Status |
| --- | --- |
| `architecture.md` (this file) | ✅ overview |
| `rcl.md` | ✅ written |
| `rcl_action.md` | ✅ written |
| `rcl_lifecycle.md` | ✅ written |
| `rcl_yaml_param_parser.md` | ✅ written |
