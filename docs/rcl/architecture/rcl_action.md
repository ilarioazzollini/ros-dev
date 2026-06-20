# `rcl_action` — Package Deep Dive

> Deep dive into the **`rcl_action`** package: the pure-C implementation of ROS 2
> *Actions*. Read the [architecture overview](./architecture.md) first for the
> shared conventions, and [`rcl.md`](./rcl.md) — this package is **built directly
> on `rcl`'s** services, topics, and timers, and follows all of `rcl`'s
> conventions (handle+impl PIMPL, the lifecycle quartet, `rcl_ret_t` codes,
> explicit allocators, attribute tables).
>
> Source: package version `10.5.1` (`repos/rcl/rcl_action/`), `rolling` line.

---

## 1. What an Action is (and why it needs its own package)

A ROS 2 **action** is a long-running, goal-oriented interaction: a client sends a
*goal*, the server streams *feedback* while working, the goal can be *canceled*,
and eventually a *result* is returned. Unlike a topic or a service, an action is
**not a single middleware primitive**. It is a *composite* built from several
ordinary ROS entities working together:

| Sub-entity | rcl type | Direction | Purpose |
| --- | --- | --- | --- |
| **goal** service | `rcl_service_t` / `rcl_client_t` | client → server | send a goal, get accepted/rejected |
| **cancel** service | `rcl_service_t` / `rcl_client_t` | client → server | request cancellation of goal(s) |
| **result** service | `rcl_service_t` / `rcl_client_t` | client → server | request the final result |
| **feedback** topic | `rcl_publisher_t` / `rcl_subscription_t` | server → client | stream progress |
| **status** topic | `rcl_publisher_t` / `rcl_subscription_t` | server → client | broadcast goal status changes |

So `rcl_action` exists to **bundle these 3 services + 2 topics into a single
action client / action server abstraction**, manage the per-goal lifecycle (the
*goal state machine*), and handle the cross-cutting concerns (goal expiration,
wait-set integration, name derivation). It builds entirely on top of `rcl` — it
does not talk to `rmw` directly for communication.

```
            ┌─────────────────── rcl_action_client_t ────────────────────┐
            │  goal_client · cancel_client · result_client  (rcl_client) │
            │  feedback_sub · status_sub                    (rcl_sub)    │
            └────────────────────────────┬───────────────────────────────┘
                       3 services + 2 topics over rcl/rmw
            ┌────────────────────────────┴─────────────────────────────────┐
            │  goal_service · cancel_service · result_service (rcl_service)│
            │  feedback_pub · status_pub                      (rcl_pub)    │
            │  expire_timer                                   (rcl_timer)  │
            │  goal_handles[]  + per-goal state machine                    │
            └─────────────────── rcl_action_server_t ──────────────────────┘
```

This decomposition is the single most important thing to understand about the
package. Everything else follows from "an action = 3 services + 2 topics + a goal
state machine."

---

## 2. Package layout

```
rcl_action/
├── CMakeLists.txt           # builds librcl_action; deps: rcl, action_msgs, rcutils, rmw, rosidl_runtime_c
├── package.xml
├── include/rcl_action/
│   ├── rcl_action.h         # umbrella header (\mainpage)
│   ├── action_client.h      # action client entity
│   ├── action_server.h      # action server entity
│   ├── goal_handle.h        # one goal's lifecycle handle
│   ├── goal_state_machine.h # the goal state machine (transition function)
│   ├── types.h              # goal states/events, msg typedefs, return codes
│   ├── names.h              # derive sub-entity names from an action name
│   ├── graph.h              # discover actions on the graph
│   ├── wait.h               # add a client/server to a wait set (composite!)
│   ├── default_qos.h        # default QoS profiles for the sub-entities
│   └── visibility_control.h
└── src/rcl_action/
    ├── action_client.c  + action_client_impl.h
    ├── action_server.c  + action_server_impl.h
    ├── goal_handle.c
    ├── goal_state_machine.c
    ├── graph.c · names.c · types.c
```

Standard `rcl` conventions apply throughout: public handle structs with opaque
`*_impl` pointers (`action_client_impl.h`, `action_server_impl.h`), the
`rcl_action_get_zero_initialized_X` / `rcl_action_X_init` / `rcl_action_X_fini` /
`rcl_action_X_get_default_options` quartet, and `rcl_ret_t` return codes (action
codes are in the `40xx` range — see overview §3.5).

---

## 3. The composite handles, concretely

The two impl structs (private, under `src/`) make the decomposition literal.

### Action server (`action_server_impl.h`)

```c
typedef struct rcl_action_server_impl_s {
  rcl_service_t   goal_service;
  rcl_service_t   cancel_service;
  rcl_service_t   result_service;
  rcl_publisher_t feedback_publisher;
  rcl_publisher_t status_publisher;
  rcl_timer_t     expire_timer;             // fires to expire old, done goals
  rcl_event_callback_with_data_t goal_expire_callback;
  char * remapped_action_name;
  bool   owns_expire_timer;
  rcl_action_server_options_t options;
  rcl_action_goal_handle_t ** goal_handles; // all in-flight goals
  size_t num_goal_handles;
  rcl_clock_t * clock;                       // for goal timestamps / expiration
  size_t wait_set_*_index;                   // bookkeeping for rcl_wait (see §6)
  rosidl_type_hash_t type_hash;
} rcl_action_server_impl_t;
```

### Action client (`action_client_impl.h`)

```c
typedef struct rcl_action_client_impl_s {
  rcl_client_t       goal_client;
  rcl_client_t       cancel_client;
  rcl_client_t       result_client;
  rcl_subscription_t feedback_subscription;
  rcl_subscription_t status_subscription;
  rcl_action_client_options_t options;
  char * remapped_action_name;
  size_t wait_set_*_index;                   // bookkeeping for rcl_wait
  rosidl_type_hash_t type_hash;
  bool disable_feedback_sub_cft;             // content-filter toggle for feedback
} rcl_action_client_impl_t;
```

Note the server owns a **`clock` + `expire_timer`**: terminal goals are retained
for a while (so late result requests still work) and then garbage-collected when
the timer fires.

---

## 4. The goal state machine — the heart of the package

Each accepted goal has a lifecycle tracked by a small, explicit state machine. It
is defined in `goal_state_machine.{h,c}` and the per-goal instance lives in a
`rcl_action_goal_handle_t`.

### States and events (`types.h`)

**States** (`rcl_action_goal_state_t`, values come from
`action_msgs/msg/GoalStatus`): `UNKNOWN`, `ACCEPTED`, `EXECUTING`, `CANCELING`,
`SUCCEEDED`, `CANCELED`, `ABORTED`.

**Events** (`rcl_action_goal_event_t`): `EXECUTE`, `CANCEL_GOAL`, `SUCCEED`,
`ABORT`, `CANCELED`.

### The transition diagram

```
                 EXECUTE
   ACCEPTED ───────────────▶ EXECUTING ───SUCCEED──▶ SUCCEEDED ✓
      │                        │   │
      │CANCEL_GOAL             │   └────ABORT──────▶ ABORTED   ✓
      │                        │CANCEL_GOAL
      ▼                        ▼
   CANCELING ◀─────────────────┘
      │   │
      │   ├──SUCCEED──▶ SUCCEEDED ✓
      │   └──ABORT────▶ ABORTED   ✓
      │
      └──CANCELED────▶ CANCELED  ✓     (✓ = terminal state)
```

### How it's implemented

The transition logic is a **2-D function-pointer table** indexed by
`[state][event]` (`_goal_state_transition_map` in `goal_state_machine.c`). Each
cell is a handler that asserts the precondition and returns the next state; empty
cells mean "invalid transition." The public entry point is:

```c
rcl_action_goal_state_t
rcl_action_transition_goal_state(rcl_action_goal_state_t state,
                                 rcl_action_goal_event_t event);
// returns the new state, or GOAL_STATE_UNKNOWN if the transition is invalid
```

This is a clean, table-driven FSM — a good small thing to read in full
(`goal_state_machine.c` is ~120 lines). The `rcl_action_goal_handle_t` wraps it:
`rcl_action_update_goal_state(handle, event)` applies an event and stores the new
state; `rcl_action_goal_handle_get_status` / `_is_active` query it.

---

## 5. Public API map

### Action server (`action_server.h`)
- Lifecycle: `rcl_action_get_zero_initialized_server`, `rcl_action_server_init`,
  `rcl_action_server_fini`, `rcl_action_server_get_default_options`.
- Incoming: `rcl_action_take_goal_request`, `rcl_action_take_cancel_request`,
  `rcl_action_take_result_request`.
- Outgoing: `rcl_action_send_goal_response`, `rcl_action_send_cancel_response`,
  `rcl_action_send_result_response`, `rcl_action_publish_feedback`,
  `rcl_action_publish_status`.
- Goal management: `rcl_action_accept_new_goal` (returns a goal handle),
  `rcl_action_process_cancel_request`, `rcl_action_expire_goals`,
  `rcl_action_notify_goal_done`, `rcl_action_server_goal_exists`.

### Action client (`action_client.h`)
- Lifecycle: `rcl_action_get_zero_initialized_client`, `rcl_action_client_init`,
  `rcl_action_client_fini`, `rcl_action_client_get_default_options`.
- Send/take pairs (mirror the server): `rcl_action_send_goal_request` /
  `rcl_action_take_goal_response`, `…send_cancel_request` /
  `…take_cancel_response`, `…send_result_request` / `…take_result_response`,
  `rcl_action_take_feedback`, `rcl_action_take_status`.
- Entity callbacks for events (one per sub-entity).

### Goal handle (`goal_handle.h`)
`rcl_action_goal_handle_init/fini`, `rcl_action_update_goal_state`,
`rcl_action_goal_handle_get_status`, `…get_info`, `…is_active`,
`…is_cancelable`. One handle per in-flight goal; wraps the state machine (§4).

### Naming (`names.h`)
Derives the five sub-entity names from a single action name:
`rcl_action_get_goal_service_name`, `…cancel_service_name`,
`…result_service_name`, `…feedback_topic_name`, `…status_topic_name`. This
encodes the ROS naming convention that maps an action like `fibonacci` to its
underlying topics/services.

### Graph (`graph.h`)
`rcl_action_get_names_and_types` (list actions on the graph) and
`rcl_action_server_is_available` (client-side readiness check) — analogous to
`rcl`'s `graph.h` but action-aware.

### Types (`types.h`)
Goal state/event enums (§4), convenience typedefs over the generated
`action_msgs` types (`rcl_action_goal_info_t`, `rcl_action_goal_status_array_t`,
`rcl_action_cancel_request_t`, `rcl_action_cancel_response_t`), and the
human-readable `goal_state_descriptions` / `goal_event_descriptions` tables.

---

## 6. Wait-set integration (`wait.h`)

Because an action is **composite**, you cannot just drop it into an `rcl_wait_set_t`
as one item — its 3 services / 2 topics (server) or 3 clients / 2 subscriptions
(client) each need to be registered individually. `wait.h` provides the glue:

- `rcl_action_wait_set_add_action_client` / `…add_action_server` — add **all** of
  an action entity's sub-entities to a wait set in one call. (This is why the
  impl structs cache a `wait_set_*_index` for each sub-entity — so that after
  `rcl_wait` returns they can be located again.)
- `rcl_action_client_wait_set_get_num_entities` / `…server_…` — how many slots to
  reserve when sizing the wait set.
- `rcl_action_client_wait_set_get_entities_ready` /
  `rcl_action_server_wait_set_get_entities_ready` — after `rcl_wait`, report
  *which* sub-entities are ready (e.g. "a goal request is pending", "feedback is
  available") as a set of booleans.

This mirrors `rcl`'s "NULL slot == not ready" wait model (see [`rcl.md`](./rcl.md)
§6.2) but lifts it to the action level. An executor uses these to know which
`rcl_action_take_*` / `rcl_action_send_*` call to make.

---

## 7. Typical flows

**Server, handling a goal:**
1. `rcl_action_take_goal_request` (a goal arrived).
2. Decide accept/reject; `rcl_action_accept_new_goal` → `rcl_action_goal_handle_t`
   (state `ACCEPTED`); `rcl_action_send_goal_response`.
3. `rcl_action_update_goal_state(handle, GOAL_EVENT_EXECUTE)` → `EXECUTING`.
4. Work: `rcl_action_publish_feedback` repeatedly; `rcl_action_publish_status` on
   changes.
5. Finish: `rcl_action_update_goal_state(handle, GOAL_EVENT_SUCCEED)` →
   `SUCCEEDED`; answer the result request via `rcl_action_send_result_response`;
   `rcl_action_notify_goal_done`.
6. Later, `rcl_action_expire_goals` (driven by the expire timer) GC's terminal
   goals.

**Client, sending a goal:** `rcl_action_send_goal_request` →
`rcl_action_take_goal_response` → (`rcl_action_take_feedback` /
`rcl_action_take_status` while running) → `rcl_action_send_result_request` →
`rcl_action_take_result_response`; optional `rcl_action_send_cancel_request` /
`rcl_action_take_cancel_response`.

---

## 8. Tests

`test/` follows the source split: `test_action_server.cpp`,
`test_action_client.cpp`, `test_goal_handle.cpp`, `test_goal_state_machine.cpp`
(exercises every transition, valid and invalid), `test_names.cpp`,
`test_graph.cpp`, `test_types.cpp`. gtest + mocking, parameterized over the rmw
implementation, with fault injection on the error paths (overview §3.8).

---

## 9. Suggested reading order

1. **`include/rcl_action/types.h`** — goal states/events; the vocabulary of the
   package.
2. **`goal_state_machine.h` → `src/rcl_action/goal_state_machine.c`** — the
   table-driven FSM in full (§4); small and self-contained.
3. **`include/rcl_action/action_server.h` + `src/.../action_server_impl.h`** —
   see the composite (3 services + 2 topics + timer) and the send/take API.
4. **`action_client.h` + `action_client_impl.h`** — the mirror image.
5. **`wait.h`** — how the composite plugs into `rcl_wait` (§6).
6. **`names.h`** — the small but illuminating action-name → sub-entity-name
   mapping.
7. `test_goal_state_machine.cpp` and `test_action_server.cpp` for executable
   examples.
```
