# `rclcpp_action` — Package Deep Dive

> Deep dive into the **`rclcpp_action`** package: the typed C++ API for ROS 2
> *Actions*. Read the [architecture overview](./architecture.md) and
> [`rclcpp.md`](./rclcpp.md) first (especially the **Waitable/executor** model and
> the **node-interfaces** pattern). This package is the C++ wrapper over
> [`rcl_action`](../../rcl/architecture/rcl_action.md) — read that too, since the
> composite "3 services + 2 topics + goal state machine" structure it documents is
> exactly what this package exposes ergonomically.
>
> Source: `repos/rclcpp/rclcpp_action/`, `rolling` line.

---

## 1. What it is

`rclcpp_action` provides the **canonical C++ API for ROS Actions** — the
`rclcpp_action::Client<ActionT>` and `rclcpp_action::Server<ActionT>` you use to
write action clients and servers. It is a thin but important layer: `rcl_action`
already implements the *mechanism* of an action (the goal/cancel/result services,
feedback/status topics, and the per-goal state machine). `rclcpp_action` adds the
two things that mechanism lacks for C++ users:

1. **Type safety** — everything is templated on the action type `ActionT`, so you
   work with `ActionT::Goal`, `ActionT::Feedback`, `ActionT::Result` instead of
   `void*` and type-erased messages.
2. **C++ ergonomics** — the server is configured with three `std::function`
   callbacks; the client is **future- and callback-based** (`std::shared_future`
   for goal acceptance and results). Errors are exceptions.

And, crucially, it integrates the composite action into `rclcpp`'s event loop by
implementing the **`rclcpp::Waitable`** interface (§5).

---

## 2. The layering over `rcl_action`

Almost every `rclcpp_action` type owns and drives an `rcl_action` counterpart:

| C++ (`rclcpp_action`) | wraps C (`rcl_action`) | adds |
| --- | --- | --- |
| `Server<ActionT>` / `ServerBase` | `rcl_action_server_t` | typed callbacks, `Waitable` integration |
| `Client<ActionT>` / `ClientBase` | `rcl_action_client_t` | typed async API, futures, callbacks |
| `ServerGoalHandle<ActionT>` | a goal + `rcl_action`'s goal state machine | `succeed()` / `abort()` / `canceled()` / `publish_feedback()` |
| `ClientGoalHandle<ActionT>` | the client-side view of a goal | status tracking, `WrappedResult`, feedback/result callbacks |
| `GoalUUID` (`types.hpp`) | `rcl_action_goal_info_t` UUID | a hashable `std::array<uint8_t,16>` |

The split into a **non-templated base** (`ServerBase`, `ClientBase` — the
type-erased machinery, compiled in `src/`) and a **templated derived class**
(`Server<ActionT>`, `Client<ActionT>` — header-only) is the same pattern as
`rclcpp`'s pub/sub (see [`rclcpp.md` §3.3](./rclcpp.md)). The base talks to
`rcl_action` and the executor; the template adds the typing.

---

## 3. The server side

### Creation: three callbacks

A server is built with `rclcpp_action::create_server<ActionT>(node, name,
handle_goal, handle_cancel, handle_accepted)` (a free function, with an overload
taking the individual **node interfaces** — base/clock/logging/waitables — so it
works with any node-like object, including `LifecycleNode`). The three callbacks
*are* the server's behavior:

| Callback | Signature (returns) | When / purpose |
| --- | --- | --- |
| `GoalCallback` | `GoalResponse(const GoalUUID &, std::shared_ptr<const Goal>)` | a goal request arrived → accept or reject it |
| `CancelCallback` | `CancelResponse(shared_ptr<ServerGoalHandle<ActionT>>)` | a cancel request arrived → allow or refuse |
| `AcceptedCallback` | `void(shared_ptr<ServerGoalHandle<ActionT>>)` | an accepted goal is ready → start working on it |

The response enums encode the decision:

- `GoalResponse`: `REJECT`, `ACCEPT_AND_EXECUTE` (begin immediately),
  `ACCEPT_AND_DEFER` (accept but start later).
- `CancelResponse`: `REJECT`, `ACCEPT`.

### `ServerGoalHandle<ActionT>` — driving one goal

The `AcceptedCallback` receives a `ServerGoalHandle`, which is the C++ face of the
goal and its state machine. The user calls:

- `publish_feedback(feedback_msg)` — stream progress (only valid while executing),
- `succeed(result)` / `abort(result)` / `canceled(result)` — terminal outcomes,
- `execute()` — move a deferred goal into the executing state,
- queries: `is_active()`, `is_executing()`, `is_canceling()`,
  `get_goal()`, `get_goal_id()`.

Each of these maps onto a transition of `rcl_action`'s goal state machine
(documented in [`rcl_action.md` §4](../../rcl/architecture/rcl_action.md)) — e.g.
`succeed()` applies the `SUCCEED` event. The C++ layer adds validation (it warns
and ignores `publish_feedback` if the goal isn't executing) and ties the outcome
back to answering the result service request.

---

## 4. The client side

### Async, future-based API

`rclcpp_action::create_client<ActionT>(node, name)` builds a `Client<ActionT>`.
The API is asynchronous and returns futures:

- **`async_send_goal(goal, options)`** → `future<ClientGoalHandle::SharedPtr>`
  (resolves when the server accepts/rejects).
- **`async_get_result(goal_handle, result_callback)`** → `future<WrappedResult>`
  (resolves when the goal finishes).
- **`async_cancel_goal(goal_handle)`**, plus
  `async_cancel_all_goals()` / `async_cancel_goals_before(time)`.

Callbacks are passed in a `SendGoalOptions` struct:

```cpp
struct SendGoalOptions {
  GoalResponseCallback goal_response_callback;  // accepted or rejected
  FeedbackCallback     feedback_callback;       // each feedback message
  ResultCallback       result_callback;         // final result
};
```

So a caller can choose futures, callbacks, or both. (Mixing freely is the normal
pattern: `await` the goal-accepted future, then let the `result_callback` fire.)

### `ClientGoalHandle<ActionT>` and `WrappedResult`

The handle returned for an accepted goal exposes `get_status()`,
`get_goal_stamp()`, `async_get_result()`, and feedback/result awareness flags
(`is_feedback_aware()`, `is_result_aware()`). The final result comes as:

```cpp
struct WrappedResult {
  GoalUUID goal_id;
  ResultCode code;                     // SUCCEEDED | CANCELED | ABORTED
  typename ActionT::Result::SharedPtr result;
};
```

`ResultCode` values map straight to `action_msgs::msg::GoalStatus` constants.

---

## 5. Executor integration: `ServerBase` *is* a `Waitable`

This is the most important architectural point. In `rcl_action`, a server/client
is a *composite* of 3 services + 2 topics, and the C layer offers helper functions
(`rcl_action_wait_set_add_*`, `..._get_entities_ready`) to plug it into a wait set
(see [`rcl_action.md` §6](../../rcl/architecture/rcl_action.md)). `rclcpp_action`
lifts that into `rclcpp`'s execution model:

> **`ServerBase` and `ClientBase` derive from `rclcpp::Waitable`.**

That means an action server/client is added to a node's
**`NodeWaitablesInterface`** and handled by an `Executor` like any other waitable.
`ServerBase` implements the `Waitable` contract:

- `add_to_wait_set(rcl_wait_set_t &)` — register all sub-entities,
- `is_ready(const rcl_wait_set_t &)` — did any sub-entity fire,
- `take_data()` / `take_data_by_entity_id()` — pull the ready data,
- `execute(data)` — dispatch to the right internal handler:
  `execute_goal_request_received`, `execute_cancel_request_received`,
  `execute_result_request_received`, `execute_check_expired_goals`,
- `set_on_ready_callback(...)` — for the events executor.

So the executor loop from [`rclcpp.md` §7.2](./rclcpp.md) ("collect → wait → pick
→ execute") drives actions for free: when the underlying goal/cancel/result
service has a request, the executor calls `Server::execute`, which invokes your
`handle_goal` / `handle_cancel` / `handle_accepted` callbacks. **This is why you
just `rclcpp::spin(node)` and your action callbacks fire** — no action-specific
spinning is required.

---

## 6. `GoalUUID` and `types.hpp`

Goals are identified by a **`GoalUUID`** = `std::array<uint8_t, 16>`. `types.hpp`
provides:

- `convert()` between `GoalUUID` and `rcl_action_goal_info_t`,
- `to_string(GoalUUID)` for logging,
- `std::hash<GoalUUID>` and `std::less<GoalUUID>` specializations — so goal
  handles can be stored in `unordered_map`/`map` keyed by goal id (both client and
  server track in-flight goals this way).

It also aliases the common message types (`GoalStatus`, `GoalInfo`).

---

## 7. Generic (type-erased) variants

Mirroring `rclcpp`'s `generic_*` entities, the package offers
`GenericClient` / `create_generic_client` and `GenericClientGoalHandle` for code
that does not know `ActionT` at compile time (introspection tools, bridges). These
trade the typed API for runtime-specified types, using the same `ClientBase`
machinery underneath.

---

## 8. Package layout

```
rclcpp_action/
├── CMakeLists.txt          # builds librclcpp_action; deps: rclcpp, rcl_action, action_msgs, rcpputils
├── include/rclcpp_action/
│   ├── rclcpp_action.hpp           # umbrella header
│   ├── server.hpp                  # ServerBase (Waitable) + Server<ActionT> + GoalResponse/CancelResponse
│   ├── server_goal_handle.hpp      # ServerGoalHandle<ActionT> (succeed/abort/canceled/feedback)
│   ├── create_server.hpp           # create_server<ActionT>(...) factories
│   ├── client.hpp                  # ClientBase (Waitable) + Client<ActionT> + SendGoalOptions
│   ├── client_goal_handle.hpp(.impl) # ClientGoalHandle<ActionT> + WrappedResult + ResultCode
│   ├── create_client.hpp
│   ├── generic_client*.hpp / create_generic_client.hpp  # type-erased variants
│   ├── types.hpp                   # GoalUUID, conversions, hash/less
│   ├── qos.hpp                     # default QoS for the action sub-entities
│   ├── exceptions.hpp
│   └── visibility_control.hpp
└── src/
    ├── server.cpp · server_goal_handle.cpp     # the type-erased base machinery
    ├── client_base.cpp
    ├── generic_client*.cpp · create_generic_client.cpp
    ├── types.cpp · qos.cpp
```

(Most `*<ActionT>` logic is header-only templates; `src/` holds the non-templated
`ServerBase`/`ClientBase` and the generic variants.)

---

## 9. Tests

`test/` covers the server, client, and goal handles end-to-end (typed and
generic), goal acceptance/rejection, cancellation, feedback/result flow, and the
`Waitable`/executor integration — typically by spinning a real client against a
real server. gtest/gmock, parameterized over the rmw implementation.

---

## 10. Suggested reading order

1. **`rcl_action.md`** (the C layer) if you haven't — the composite + goal state
   machine this package wraps.
2. **`types.hpp`** — `GoalUUID` and the C↔C++ conversions; small vocabulary.
3. **`server.hpp`** — the three callbacks, the response enums, and that
   `ServerBase : public rclcpp::Waitable` (§5); then `server_goal_handle.hpp`.
4. **`create_server.hpp`** — the node-interfaces-based factory (ties to
   [`rclcpp.md` §6](./rclcpp.md)).
5. **`client.hpp` + `client_goal_handle.hpp`** — the async/future API and
   `WrappedResult`.
6. A `test_*` that spins a client against a server for the full lifecycle in one
   place.
```
