# `rcl_lifecycle` — Package Deep Dive

> Deep dive into the **`rcl_lifecycle`** package: the pure-C implementation of ROS
> 2 *managed (lifecycle) nodes*. Read the [architecture overview](./architecture.md)
> first for the shared conventions, and [`rcl.md`](./rcl.md) — like
> [`rcl_action`](./rcl_action.md), this package is **built on top of `rcl`'s**
> publishers and services.
>
> Source: package version `10.5.1` (`repos/rcl/rcl_lifecycle/`), `rolling` line.

---

## 1. What a lifecycle node is

A normal ROS 2 node starts running its logic the moment it is created. A
**managed (lifecycle) node** instead has an explicit, externally-controllable
**state machine** so that startup, configuration, activation, and shutdown happen
in well-defined, observable steps. This lets a system bring nodes up in a
deterministic order, (re)configure them, activate/deactivate them at runtime, and
shut them down cleanly.

`rcl_lifecycle` provides the **C core of that state machine** plus the **ROS
communication interface** to drive and observe it. As with actions, the
higher-level ergonomics (the `LifecycleNode` class, the user callbacks
`on_configure`, `on_activate`, …) live in `rclcpp_lifecycle`; this package
supplies the state-machine engine those build on. It builds on `rcl` and does not
talk to `rmw` directly for communication.

---

## 2. The two halves of the package

`rcl_lifecycle` has two clearly separated concerns, and almost every file belongs
to one of them:

1. **The state machine (pure data + logic)** — states, transitions, the map that
   connects them, and the "trigger a transition" engine. No ROS communication
   involved; this part could run in isolation.
2. **The communication interface (`com_interface`)** — the ROS publisher +
   services that expose the state machine to the rest of the system, so external
   tools can query the current state and command transitions.

```
   external tools (ros2 lifecycle CLI, lifecycle manager)
        │  services: change_state / get_state / get_available_states /
        │            get_available_transitions / get_transition_graph
        ▼
   ┌──────────────── rcl_lifecycle_state_machine_t ──────────────────┐
   │  current_state        ← where we are now                        │
   │  transition_map       ← all states + valid transitions (§4)     │
   │  com_interface        ← 1 publisher + 5 services (rcl) (§5)     │
   │  options                                                        │
   └─────────────────────────────────────────────────────────────────┘
        │  publisher: /<node>/transition_event   (every transition)
        ▼
   observers / loggers
```

---

## 3. A convention note: transparent structs, no PIMPL

Like [`rcl_yaml_param_parser`](./rcl_yaml_param_parser.md), and **unlike** `rcl`
and `rcl_action`, this package does **not** use the opaque handle+impl (PIMPL)
pattern (overview §3.2). All the structures are **fully public** in
`include/rcl_lifecycle/data_types.h` — `rcl_lifecycle_state_t`,
`rcl_lifecycle_transition_t`, `rcl_lifecycle_transition_map_t`,
`rcl_lifecycle_com_interface_t`, and `rcl_lifecycle_state_machine_t` are all
transparent. Callers (notably `rclcpp_lifecycle`) read these fields directly.

It does still follow the rest of the `rcl` conventions: the
`rcl_lifecycle_get_zero_initialized_X` / `_init` / `_fini` lifecycle, `rcl_ret_t`
return codes (the `30xx` range is reserved for lifecycle — overview §3.5),
explicit allocators, and the Doxygen attribute tables. It also emits `tracetools`
tracepoints on transitions.

---

## 4. The state machine

### Data model (`data_types.h`)

```
rcl_lifecycle_state_t                 a node state
  ├── label            "unconfigured" / "inactive" / "active" / ...
  ├── id               numeric id (from lifecycle_msgs/msg/State)
  ├── valid_transitions[]   transitions leaving this state
  └── valid_transition_size

rcl_lifecycle_transition_t            an edge between two states
  ├── label            "configure" / "activate" / ...
  ├── id               numeric id (from lifecycle_msgs/msg/Transition)
  ├── start            → source rcl_lifecycle_state_t
  └── goal             → target rcl_lifecycle_state_t

rcl_lifecycle_transition_map_t        the whole graph (registry)
  ├── states[]   / states_size
  └── transitions[] / transitions_size
```

The transition map is a **registry you populate**, not a fixed table. You call
`rcl_lifecycle_register_state()` and `rcl_lifecycle_register_transition()`
(`transition_map.h`) to build a graph, then look things up with
`rcl_lifecycle_get_state()` / `rcl_lifecycle_get_transitions()`.

> **Contrast with `rcl_action`.** `rcl_action`'s goal FSM is a *compile-time*
> 2-D function-pointer table (fixed at build time). `rcl_lifecycle`'s FSM is a
> *runtime* data structure built during init — which is why a **custom** state
> machine is possible (pass `default_state = false`), though in practice almost
> everyone uses the standard one.

### The default (standard ROS 2) lifecycle (`default_state_machine.c`)

`rcl_lifecycle_init_default_state_machine()` registers the canonical ROS 2
lifecycle. There are two kinds of state:

- **Primary states** (resting points): `Unconfigured`, `Inactive`, `Active`,
  `Finalized` (plus `Unknown`).
- **Transition states** (transient, where the user callbacks run):
  `Configuring`, `CleaningUp`, `Activating`, `Deactivating`, `ShuttingDown`,
  `ErrorProcessing`.

```
        ┌──────────────┐  configure   ┌────────────┐ on_success ┌──────────┐
        │ Unconfigured │ ───────────▶ │ Configuring│ ─────────▶ │ Inactive │
        │              │ ◀─────────── │            │ on_failure └────┬─────┘
        └──────┬───────┘   cleanup ▲  └─────┬──────┘                 │
               │           (CleaningUp)     │ on_error               │
               │                            ▼                        │
               │                     ┌───────────────┐  activate     │
   shutdown    │                     │ErrorProcessing│   ┌───────────┘
   (any of     │                     └──────┬────────┘   ▼
   the 3) ─────┼──▶ ShuttingDown            │     ┌────────────┐ on_success ┌────────┐
               │        │                   │     │ Activating │ ─────────▶ │ Active │
               ▼        ▼ on_success        │     └────────────┘            └───┬────┘
          ┌───────────┐  ┌──────────┐       │                      deactivate   │
          │ Finalized │◀─┤          │       │   ┌─────────────┐  ◀──────────────┘
          └───────────┘  └──────────┘       │   │ Deactivating│
                                            ▼   └─────────────┘
```

The key idea: a user-triggered transition (`configure`, `cleanup`, `activate`,
`deactivate`, `shutdown`) moves the node into a **transition state**; the user
callback runs; then a **result transition** (`on_…_success` / `on_…_failure` /
`on_…_error`) moves it to the next primary state. `ErrorProcessing` is the common
landing pad for any callback that errors. (The exact ids come from
`lifecycle_msgs/msg/State` and `…/Transition`.)

### Triggering a transition (`rcl_lifecycle.c`)

```c
rcl_ret_t rcl_lifecycle_trigger_transition_by_id(   // or _by_label
  rcl_lifecycle_state_machine_t * sm, uint8_t id, bool publish_notification);
```

Internally this looks up the transition leaving `current_state` with that
id/label, updates `current_state` to the transition's `goal`, and (if requested)
publishes a transition-event message via the com interface. Looking up an invalid
transition for the current state is an error — this is what enforces the state
machine's rules.

---

## 5. The communication interface (`com_interface.{h,c}`)

`rcl_lifecycle_com_interface_t` is what makes the state machine *managed from
outside the process*. It bundles a set of `rcl` entities:

- **1 publisher** — publishes a `TransitionEvent` on the node's transition-event
  topic every time a transition fires (so observers can watch state changes).
- **5 services** that external tooling calls:
  - `change_state` — command a transition (the write path),
  - `get_state` — read the current state,
  - `get_available_states`,
  - `get_available_transitions`,
  - `get_transition_graph` — the full state graph.

The struct also holds the node handle, a clock for time-stamping transition
events, and a cached transition-event message. Init is split so the publisher and
services can be brought up independently
(`rcl_lifecycle_com_interface_publisher_init` /
`…services_init`), and the whole thing can be disabled via the state-machine
options (`rcl_lifecycle_state_machine_options_t.enable_com_interface`) for a
state machine that runs purely in-process with no ROS surface.

---

## 6. Public API map

### State machine (`rcl_lifecycle.h`)
- Building blocks: `rcl_lifecycle_get_zero_initialized_state` / `_state_init` /
  `_state_fini`; same for `transition`.
- State machine: `rcl_lifecycle_get_zero_initialized_state_machine`,
  `rcl_lifecycle_state_machine_init` (wires up the com interface — takes the node,
  clock, the publisher type support, and the five service type supports),
  `rcl_lifecycle_state_machine_fini`, `rcl_lifecycle_state_machine_is_initialized`,
  `rcl_lifecycle_get_default_state_machine_options`.
- Lookups: `rcl_lifecycle_get_transition_by_id` / `_by_label`,
  `rcl_lifecycle_get_transition_label_by_id`.
- Drive it: `rcl_lifecycle_trigger_transition_by_id` / `_by_label`.
- Debug: `rcl_print_state_machine`.

### Transition map (`transition_map.h`)
`rcl_lifecycle_get_zero_initialized_transition_map`,
`rcl_lifecycle_transition_map_is_initialized`, `…_fini`,
`rcl_lifecycle_register_state`, `rcl_lifecycle_register_transition`,
`rcl_lifecycle_get_state`, `rcl_lifecycle_get_transitions`. (Use these to build a
custom state machine; the default builder uses them internally.)

### Default state machine (`default_state_machine.h`)
`rcl_lifecycle_init_default_state_machine` — registers the standard ROS 2
lifecycle described in §4.

### Communication interface (`com_interface.h`, internal)
`rcl_lifecycle_com_interface_init` / `_fini`, the split publisher/services
init/fini, and `rcl_lifecycle_com_interface_publish_notification`. (Declared under
`src/`; used by the state machine, not part of the public top-level API.)

---

## 7. Package layout

```
rcl_lifecycle/
├── CMakeLists.txt           # builds librcl_lifecycle
│                            # deps: rcl, lifecycle_msgs, rcutils, rmw,
│                            #       rosidl_runtime_c, tracetools
├── package.xml
├── include/rcl_lifecycle/
│   ├── rcl_lifecycle.h          # state-machine API + umbrella header
│   ├── data_types.h             # all (transparent) structs
│   ├── default_state_machine.h  # the standard ROS 2 lifecycle builder
│   ├── transition_map.h         # the state/transition registry
│   └── visibility_control.h
└── src/
    ├── rcl_lifecycle.c          # state-machine init/fini + trigger engine
    ├── default_state_machine.c  # registers the standard states/transitions
    ├── transition_map.c         # the registry implementation
    ├── com_interface.c + com_interface.h   # the ROS pub + 5 services
```

---

## 8. Tests

`test/test_default_state_machine.cpp` (walks the standard lifecycle and checks
every valid/invalid transition), `test_rcl_lifecycle.cpp` (state-machine init and
triggering), `test_transition_map.cpp` (the registry), and
`test_multiple_instances.cpp` (several state machines coexisting). gtest, linked
against `rcl` and `lifecycle_msgs`.

---

## 9. Suggested reading order

1. **`include/rcl_lifecycle/data_types.h`** — the transparent structs; the whole
   data model fits on a page (§4).
2. **`include/rcl_lifecycle/transition_map.h`** — how states/transitions are
   registered and looked up (the FSM is a runtime graph).
3. **`src/default_state_machine.c`** — see the standard ROS 2 lifecycle built up
   transition by transition (§4); this makes the abstract diagram concrete.
4. **`src/rcl_lifecycle.c`** — `rcl_lifecycle_trigger_transition_*`: how a
   transition is validated, applied, and published.
5. **`src/com_interface.{h,c}`** — the 1 publisher + 5 services that expose the
   machine to ROS (§5).
6. `test/test_default_state_machine.cpp` for an executable tour of the lifecycle.
```
