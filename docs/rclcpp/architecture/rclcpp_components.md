# `rclcpp_components` — Package Deep Dive

> Deep dive into the **`rclcpp_components`** package: the tooling that lets a node
> be compiled once and then **dynamically loaded and composed** into a process at
> runtime. Read the [architecture overview](./architecture.md) first for the
> shared C++ conventions, and [`rclcpp.md`](./rclcpp.md) — this package builds
> directly on `rclcpp` (and crucially on its **node-interfaces** pattern).
>
> Source: `repos/rclcpp/rclcpp_components/`, `rolling` line.

---

## 1. What components are, and the problem they solve

Normally a ROS 2 node is compiled into its own executable with a `main()` that
creates the node and spins it — **one process per node**. That is simple but
costly when many nodes run on one machine: every inter-node message crosses a
process boundary (serialize → transport → deserialize), even between nodes that
could share memory.

**Components** decouple "a node" from "a process." You write your node as a
class, compile it into a **shared library**, and register it. At runtime a
**container process** can load any number of such node classes into itself and
spin them together — so nodes in the same container can use zero-copy
**intra-process** communication ([`rclcpp.md` §7.1](./rclcpp.md)), and you choose
the process topology by configuration rather than at compile time.

This is the ROS 2 *composition* mechanism. `rclcpp_components` provides the three
pieces that make it work:

1. a way to **register** a node class as a loadable plugin (a macro + CMake),
2. a **factory abstraction** to construct it without knowing its concrete type,
3. a **component manager / container** that loads, runs, and unloads them over a
   ROS service interface.

```
   build time:  YourNode (class)  ──RCLCPP_COMPONENTS_REGISTER_NODE──▶  libYourNode.so
                                   + ament resource index entry

   run time:    container process
                  │  service: ~/_container/load_node  (LoadNode)
                  ▼
                ComponentManager ──class_loader──▶ dlopen libYourNode.so
                  │                               └─ NodeFactory::create_node_instance()
                  ▼
                node added to an Executor and spun in-process
```

---

## 2. How it differs from the other packages

- **It is mostly tooling, not new ROS concepts.** Unlike `rclcpp_action` /
  `rclcpp_lifecycle`, there is **no `rcl_components` counterpart** — composition
  is a pure C++/build-system concern layered on top of `rclcpp`.
- **The build system is part of the package.** Half the value is in `cmake/`
  macros and a generated `main()` template, not just headers (§5).
- **It leans on `class_loader`** (the ROS pluginlib-style `dlopen` wrapper) and
  **`composition_interfaces`** (the `LoadNode` / `UnloadNode` / `ListNodes`
  services). These two deps are what `rclcpp` itself does not provide.
- It still follows the usual `rclcpp` conventions: smart pointers, `rclcpp::Node`
  as a base (the manager *is* a node), exceptions
  (`ComponentManagerException`).

---

## 3. The factory abstraction (the type-erasure trick)

The loader must construct your node **without knowing its C++ type** (it only has
a string class name from a `.so`). Three small headers solve this:

| Header | Role |
| --- | --- |
| `node_factory.hpp` | `NodeFactory` — abstract interface with one method: `NodeInstanceWrapper create_node_instance(const rclcpp::NodeOptions &)`. This is the type the class loader actually loads. |
| `node_factory_template.hpp` | `NodeFactoryTemplate<NodeT>` — the concrete factory: `create_node_instance` does `std::make_shared<NodeT>(options)` and wraps it. One template instantiation per registered node class. |
| `node_instance_wrapper.hpp` | `NodeInstanceWrapper` — holds the constructed node as a `shared_ptr<void>` plus a `std::function` that returns its `NodeBaseInterface`. |

The key insight ties back to [`rclcpp.md` §6](./rclcpp.md): the wrapper does **not**
store an `rclcpp::Node`. It stores the opaque instance plus a getter for its
**`NodeBaseInterface`**. That is why **a component does not have to derive from
`rclcpp::Node`** — it only has to expose `get_node_base_interface()`. A
`LifecycleNode`, or any custom class with that method, can be a component. The
node-interfaces pattern is exactly what makes composition type-agnostic.

### Registering a node

User code adds one line in a `.cpp`:

```cpp
#include "rclcpp_components/register_node_macro.hpp"
RCLCPP_COMPONENTS_REGISTER_NODE(my_pkg::MyNode)
```

That macro expands to a `class_loader` registration:

```cpp
CLASS_LOADER_REGISTER_CLASS(
  rclcpp_components::NodeFactoryTemplate<my_pkg::MyNode>,  // concrete factory
  rclcpp_components::NodeFactory)                          // base the loader queries
```

So the loadable plugin is the *factory*, not the node itself.

---

## 4. The ComponentManager and containers (the runtime)

### `ComponentManager` (`component_manager.hpp`)

`ComponentManager` **is an `rclcpp::Node`** that exposes the composition services
defined in `composition_interfaces`:

| Service | Handler | What it does |
| --- | --- | --- |
| `~/_container/load_node` (`LoadNode`) | `on_load_node` | resolve the requested component (via the ament resource index) → `dlopen` its library with a `class_loader::ClassLoader` → get the `NodeFactory` → `create_node_instance(options)` → store the `NodeInstanceWrapper` and add the node to the executor → return a unique `node_id`. |
| `~/_container/unload_node` (`UnloadNode`) | `on_unload_node` | look up `node_id`, remove the node from the executor, drop the wrapper (and unload the library when no longer used). |
| `~/_container/list_nodes` (`ListNodes`) | `on_list_nodes` | report currently loaded components and their ids. |

State it keeps:

```cpp
std::map<std::string, std::unique_ptr<class_loader::ClassLoader>> loaders_;   // open .so files
std::map<uint64_t, rclcpp_components::NodeInstanceWrapper>        node_wrappers_; // loaded nodes
rclcpp::Service<LoadNode>::SharedPtr   loadNode_srv_;
rclcpp::Service<UnloadNode>::SharedPtr unloadNode_srv_;
rclcpp::Service<ListNodes>::SharedPtr  listNodes_srv_;
```

The base `ComponentManager` adds loaded nodes to a single shared executor. The
virtual `add_node_to_executor` / `remove_node_from_executor` hooks let subclasses
change that policy.

### `ComponentManagerIsolated` (`component_manager_isolated.hpp`)

A templated subclass (`ComponentManagerIsolated<ExecutorT =
SingleThreadedExecutor>`) that gives **each component its own dedicated executor**
(a `DedicatedExecutorWrapper` with its own thread) instead of sharing one. This
isolates components from each other's callback blocking — at the cost of a thread
per component.

### The container executables (`CMakeLists.txt`)

The package ships ready-to-run container processes, each just a `main()` that
creates a manager + executor and spins:

| Executable | Manager + executor |
| --- | --- |
| `component_container` | `ComponentManager` + `SingleThreadedExecutor` |
| `component_container_mt` | `ComponentManager` + `MultiThreadedExecutor` |
| `component_container_isolated` | `ComponentManagerIsolated` (per-component executors) |
| `component_container_event` | event-based container variant |

So `ros2 run rclcpp_components component_container` launches an empty container,
and `ros2 component load …` calls its `LoadNode` service.

---

## 5. The build-system half (`cmake/` + `node_main.cpp.in`)

Composition needs build-time support so the runtime can *find* components. The
package provides CMake macros (registered as ament extras via
`rclcpp_components-extras.cmake.in`):

- **`rclcpp_components_register_nodes(target "pkg::ClassA" "pkg::ClassB")`** —
  records the component class names of a shared library into the **ament resource
  index** (under a `rclcpp_components` marker), so a `ComponentManager` can
  discover which library provides a requested class. This is the "register" half
  that pairs with the `RCLCPP_COMPONENTS_REGISTER_NODE` macro in the source.
- **`rclcpp_components_register_node(target PLUGIN "pkg::Class" EXECUTABLE name
  [EXECUTOR ...])`** — does the same registration **and** generates a standalone
  executable for that single component. It defaults `EXECUTOR` to
  `SingleThreadedExecutor`. The generated `main()` comes from the configured
  template **`node_main.cpp.in`**, which: parses args, builds an executor, uses a
  `class_loader::ClassLoader` to load the component's factory by name, constructs
  the node, adds it to the executor, and spins.

This is why `node_main.cpp.in` lives in `src/` but is *installed* rather than
compiled directly — it is a template the macro instantiates per executable.

The result: the **same node class** can be (a) loaded into a shared container, (b)
loaded into an isolated container, or (c) run as its own standalone executable —
all chosen at build/launch time without changing the node's code.

---

## 6. Package layout

```
rclcpp_components/
├── CMakeLists.txt            # builds component_manager lib + container executables
├── rclcpp_components-extras.cmake.in   # registers the cmake macros as ament extras
├── cmake/
│   ├── rclcpp_components_register_node.cmake    # register + generate one executable
│   ├── rclcpp_components_register_nodes.cmake   # register class names into resource index
│   └── rclcpp_components_package_hook.cmake
├── include/rclcpp_components/
│   ├── register_node_macro.hpp     # RCLCPP_COMPONENTS_REGISTER_NODE (source side)
│   ├── node_factory.hpp            # NodeFactory interface (loaded by class_loader)
│   ├── node_factory_template.hpp   # NodeFactoryTemplate<NodeT> concrete factory
│   ├── node_instance_wrapper.hpp   # NodeInstanceWrapper (instance + base-interface getter)
│   ├── component_manager.hpp       # ComponentManager (a Node with load/unload/list services)
│   ├── component_manager_isolated.hpp  # per-component dedicated executors
│   └── visibility_control.hpp
└── src/
    ├── component_manager.cpp
    ├── component_container.cpp / _mt.cpp / _isolated.cpp / _event.cpp  # container mains
    └── node_main.cpp.in            # template for per-component standalone executables
```

---

## 7. End-to-end: loading a component

```
1. build:   RCLCPP_COMPONENTS_REGISTER_NODE(my_pkg::MyNode) in MyNode.cpp
            + rclcpp_components_register_nodes(my_node_lib "my_pkg::MyNode") in CMake
            → libmy_node_lib.so + an entry in the ament resource index

2. launch:  ros2 run rclcpp_components component_container        # empty container
3. load:    ros2 component load /ComponentManager my_pkg my_pkg::MyNode
              → LoadNode service request to the container

4. manager (on_load_node):
     resolve "my_pkg::MyNode" → library path (resource index)
     ClassLoader.load(library) → getAvailableClasses<NodeFactory>()
     factory = create instance of NodeFactoryTemplate<my_pkg::MyNode>
     wrapper = factory->create_node_instance(options)   // make_shared<MyNode>(options)
     add wrapper.get_node_base_interface() to the executor
     store in node_wrappers_[node_id]; return node_id

5. running: MyNode now spins inside the container, sharing intra-process comms
            with its co-located components
6. unload:  ros2 component unload /ComponentManager <node_id> → on_unload_node
```

---

## 8. Tests

`test/test_component_manager.cpp` and `test_component_manager_api.cpp` exercise
load/unload/list and error handling; `test/components/test_component.cpp` is a
sample component registered with the macro; `test/benchmark/benchmark_components.cpp`
measures load performance. gtest, with a real `ClassLoader` loading the test
component library.

---

## 9. Suggested reading order

1. **`node_factory.hpp` → `node_factory_template.hpp` → `node_instance_wrapper.hpp`**
   — the type-erasure core (§3); small and the conceptual heart of the package.
2. **`register_node_macro.hpp`** — see how a node becomes a `class_loader`
   plugin (one macro).
3. **`component_manager.hpp` → `src/component_manager.cpp`** — the `LoadNode`
   service flow (§4); the runtime heart.
4. **`cmake/rclcpp_components_register_node.cmake` + `src/node_main.cpp.in`** — the
   build-system half (§5); how the resource index + generated `main()` tie it
   together.
5. **`component_manager_isolated.hpp`** and the `component_container*.cpp` mains —
   the executor-topology variants.
6. `test/components/test_component.cpp` for a minimal working component.
```
