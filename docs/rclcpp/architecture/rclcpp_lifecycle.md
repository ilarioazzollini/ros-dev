# `rclcpp_lifecycle` — Package Deep Dive

> Deep dive into the **`rclcpp_lifecycle`** package: the C++ API for ROS 2
> *managed (lifecycle) nodes*. Read the [architecture overview](./architecture.md)
> and [`rclcpp.md`](./rclcpp.md) first (especially the **node-interfaces**
> pattern — it is what makes this package possible). This is the C++ wrapper over
> [`rcl_lifecycle`](../../rcl/architecture/rcl_lifecycle.md), so read that too: the
> state machine it documents is the engine this package drives.
>
> Source: `repos/rclcpp/rclcpp_lifecycle/`, `rolling` line.

---

## 1. What it is

A **managed (lifecycle) node** has an explicit state machine — `Unconfigured →
Inactive → Active → Finalized` — so a system can bring it up, configure it,
activate/deactivate it, and shut it down in deterministic, observable steps (the
full state machine is in
[`rcl_lifecycle.md` §4](../../rcl/architecture/rcl_lifecycle.md)).

`rcl_lifecycle` already implements that state machine and its ROS communication
interface (the `change_state`/`get_state`/… services and the transition-event
publisher). `rclcpp_lifecycle` adds the **C++ developer experience**:

1. **`LifecycleNode`** — a node class that owns the state machine and that you can
   use almost exactly like `rclcpp::Node`.
2. **Six lifecycle callbacks** (`on_configure`, `on_activate`, …) where your code
   runs during each transition.
3. **Managed entities** (notably `LifecyclePublisher`) that automatically go
   silent when the node is not active.

---

## 2. The layering over `rcl_lifecycle`

| C++ (`rclcpp_lifecycle`) | wraps C (`rcl_lifecycle`) | adds |
| --- | --- | --- |
| `LifecycleNode` (+ its pimpl) | `rcl_lifecycle_state_machine_t` + com interface | a full node; runs C++ callbacks on transitions |
| `LifecycleNodeInterface` | the transition states (where callbacks run) | the 6 virtual `on_*` callbacks + `CallbackReturn` |
| `State` (`state.hpp`) | `rcl_lifecycle_state_t` | a C++ value wrapper |
| `Transition` (`transition.hpp`) | `rcl_lifecycle_transition_t` | a C++ value wrapper |

The actual state machine lives inside the node's private impl
(`LifecycleNode::LifecycleNodeInterfaceImpl`), which holds an
`rcl_lifecycle_state_machine_t` and the `change_state` service. The C++ layer's
job is to **run the right user callback during a transition and report the result
back to the state machine** so it can pick the next state (§5).

---

## 3. `LifecycleNode` is a *sibling* of `rclcpp::Node`, not a subclass

This is the key architectural point, and it is a direct payoff of the
node-interfaces pattern ([`rclcpp.md` §6](./rclcpp.md)).

`LifecycleNode` does **not** inherit from `rclcpp::Node`. Instead it implements
the **same eleven `get_node_<aspect>_interface()` methods** (base, clock, graph,
logging, timers, topics, services, parameters, time-source, type-descriptions,
waitables). Because executors, `rclcpp_components`, and `rclcpp_action` all accept
*node interfaces* rather than a concrete `Node`, a `LifecycleNode` works
everywhere a regular node does:

```cpp
auto node = std::make_shared<rclcpp_lifecycle::LifecycleNode>("managed");
executor.add_node(node->get_node_base_interface());   // spins like any node
```

It additionally inherits from `LifecycleNodeInterface` (the callbacks, §4) and
adds the lifecycle API (§5). So you get "a `Node` plus a state machine" without a
class-hierarchy coupling to `Node`. (`type_traits/is_manageable_node.hpp` lets
generic code detect node-like types that support management.)

---

## 4. The six lifecycle callbacks (`LifecycleNodeInterface`)

`node_interfaces/lifecycle_node_interface.hpp` defines the user hooks, each
returning a `CallbackReturn`:

```cpp
enum class CallbackReturn : uint8_t { SUCCESS, FAILURE, ERROR };

virtual CallbackReturn on_configure (const State & previous_state);
virtual CallbackReturn on_cleanup   (const State & previous_state);
virtual CallbackReturn on_activate  (const State & previous_state);
virtual CallbackReturn on_deactivate(const State & previous_state);
virtual CallbackReturn on_shutdown  (const State & previous_state);
virtual CallbackReturn on_error     (const State & previous_state);
```

These run **inside the transition states** of the
[`rcl_lifecycle` state machine](../../rcl/architecture/rcl_lifecycle.md#4-the-state-machine):
e.g. triggering `configure` moves the machine into `Configuring`, your
`on_configure` runs, and its return value selects the result transition
(`on_configure_success` → `Inactive`, `_failure` → back to `Unconfigured`,
`_error` → `ErrorProcessing`). The default implementations return `SUCCESS`.

Two ways to supply behavior:

- **Override the virtuals** in a `LifecycleNode` subclass, or
- **Register `std::function`s** without subclassing:
  `register_on_configure(...)`, `register_on_activate(...)`, etc.

---

## 5. Driving transitions

`LifecycleNode` exposes both convenience methods and a general trigger:

- Convenience: `configure()`, `cleanup()`, `activate()`, `deactivate()`,
  `shutdown()` — each triggers the matching transition and returns the resulting
  `State`.
- General: `trigger_transition(const Transition&)` / `trigger_transition(id)` /
  `trigger_transition(label)`.
- Introspection: `get_current_state()`, `get_available_states()`,
  `get_available_transitions()`.

Transitions can be driven **two ways**, both ending up in the same place:

1. **Programmatically**, by calling the methods above in your own code.
2. **Externally**, via the `change_state` service that the impl stands up —
   exactly the `rcl_lifecycle` com interface
   ([`rcl_lifecycle.md` §5](../../rcl/architecture/rcl_lifecycle.md)) — so tools
   like `ros2 lifecycle set` and a lifecycle manager can drive the node remotely.

Inside the impl (`LifecycleNodeInterfaceImpl`), a transition flows as:
`change_state`/`trigger_transition` → tell `rcl_lifecycle` to enter the transition
state → `on_change_state` looks up and runs the matching registered C++ callback →
maps its `CallbackReturn` to the success/failure/error result transition → tells
`rcl_lifecycle` to advance → the transition-event message is published. A
`std::recursive_mutex` guards the state machine.

---

## 6. Managed entities — keeping inactive nodes quiet

A lifecycle node should not emit data while it is merely `Inactive`. The package
makes this automatic with a small "managed entity" abstraction
(`managed_entity.hpp`):

```cpp
class ManagedEntityInterface {            // on_activate() / on_deactivate()
  virtual void on_activate()   = 0;
  virtual void on_deactivate() = 0;
};
class SimpleManagedEntity : public ManagedEntityInterface {
  bool is_activated() const;              // tracks the enabled/disabled flag
};
```

The headline implementer is **`LifecyclePublisher<MessageT>`**
(`lifecycle_publisher.hpp`), which derives from **both** `SimpleManagedEntity`
**and** `rclcpp::Publisher<MessageT>`, and **overrides `publish()`** to drop the
message (with a warning log) when the entity is not activated:

```cpp
void publish(const MessageT & msg) {
  if (!this->is_activated()) { log_publisher_not_enabled(); return; }
  rclcpp::Publisher<MessageT, Alloc>::publish(msg);
}
```

`LifecycleNode::create_publisher<T>()` returns a `LifecyclePublisher`, and the
node activates/deactivates its managed entities as part of the
`on_activate`/`on_deactivate` transitions. **Net effect:** publishers created on a
lifecycle node are silent until the node is `Active`, with no extra user code.

---

## 7. `State` and `Transition` (`state.hpp`, `transition.hpp`)

Thin C++ value wrappers over `rcl_lifecycle_state_t` / `rcl_lifecycle_transition_t`
(themselves backed by `lifecycle_msgs`). They expose `id()` and `label()` and are
what the callbacks receive (`const State & previous_state`) and what
`get_current_state()` / `get_available_transitions()` return. They own or
reference the underlying C struct and handle its lifetime.

---

## 8. Package layout

```
rclcpp_lifecycle/
├── CMakeLists.txt          # builds librclcpp_lifecycle
│                           # deps: rclcpp, rcl_lifecycle, lifecycle_msgs, rcl_interfaces
├── include/rclcpp_lifecycle/
│   ├── lifecycle_node.hpp (+ _impl.hpp)   # LifecycleNode: sibling of Node + state machine
│   ├── node_interfaces/
│   │   └── lifecycle_node_interface.hpp   # the 6 on_* callbacks + CallbackReturn
│   ├── managed_entity.hpp                 # ManagedEntityInterface / SimpleManagedEntity
│   ├── lifecycle_publisher.hpp            # LifecyclePublisher (activation-gated publish)
│   ├── state.hpp · transition.hpp         # C++ wrappers over rcl_lifecycle types
│   ├── type_traits/is_manageable_node.hpp
│   └── visibility_control.h
└── src/
    ├── lifecycle_node.cpp
    ├── lifecycle_node_interface_impl.{hpp,cpp}   # owns rcl_lifecycle_state_machine_t + change_state srv
    ├── node_interfaces/lifecycle_node_interface.cpp
    ├── managed_entity.cpp · state.cpp · transition.cpp
```

---

## 9. Tests

`test/` covers the full transition matrix (valid/invalid transitions, callback
return values selecting the next state), callback registration vs. overriding,
the `change_state` service path, and `LifecyclePublisher` activation gating
(messages dropped while inactive, delivered while active). gtest, spinning a real
`LifecycleNode`.

---

## 10. Suggested reading order

1. **`rcl_lifecycle.md`** (the C engine) if you haven't — the state machine and
   com interface this package drives.
2. **`node_interfaces/lifecycle_node_interface.hpp`** — the six callbacks +
   `CallbackReturn`; the user-facing contract (§4).
3. **`lifecycle_node.hpp`** — note the eleven `get_node_*_interface()` methods
   (sibling-of-`Node`, §3) and the transition API (§5).
4. **`src/lifecycle_node_interface_impl.{hpp,cpp}`** — how a transition runs a C++
   callback and reports back to `rcl_lifecycle` (§5); the integration heart.
5. **`managed_entity.hpp` + `lifecycle_publisher.hpp`** — the activation-gating
   mechanism (§6).
6. A `test_*` that walks a node through configure→activate→deactivate→shutdown.
```
