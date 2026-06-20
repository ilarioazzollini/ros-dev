# `rcl_yaml_param_parser` — Package Deep Dive

> Deep dive into the **`rcl_yaml_param_parser`** package. Read the
> [architecture overview](./architecture.md) first for the stack and the shared
> conventions. This is the **smallest, most self-contained package** in the repo
> and a great second read after [`rcl.md`](./rcl.md).
>
> Source: package version `10.5.1` (`repos/rcl/rcl_yaml_param_parser/`), `rolling`
> line.

---

## 1. What it is

`rcl_yaml_param_parser` does exactly one thing: **parse a YAML parameter file (or
a single YAML value string) into a plain C data structure** that holds ROS 2
parameters, organized per node. It is the backend behind `ros2 run … --ros-args
--params-file my_params.yaml`: `rcl`'s argument parsing calls into this package to
turn the file into the `rcl_params_t` structure, which is then applied to nodes.

It is a **leaf utility**: it does *not* depend on `rcl`, `rmw` entities, or any
ROS communication. It depends only on `rcutils` (allocators, string arrays,
return codes) and `libyaml` (the actual YAML tokenizer). Because of this it can
be reused completely standalone.

```
   rcl (arguments.c)  ──uses──▶  rcl_yaml_param_parser  ──uses──▶  libyaml
                                         │
                                         └──uses──▶ rcutils (allocator, string_array)
```

---

## 2. The one important departure from repo conventions

Unlike the rest of the repository, this package **does not use the handle +
opaque-impl (PIMPL) pattern** (overview §3.2). The data structures are **fully
public and transparent** — every field of `rcl_params_t`, `rcl_node_params_t`,
and `rcl_variant_t` is defined directly in the public `types.h` and meant to be
read by callers. That makes sense: the whole point of the package is to hand you
a data structure you then walk.

Other consequences of being a low-level leaf:

- Functions return **`rcutils_ret_t`** or **`bool`**, not `rcl_ret_t`. (There is
  no dependency on `rcl/types.h`.)
- The lifecycle naming is slightly different: you'll see
  `rcl_yaml_node_struct_init` / `_fini` / `_copy` rather than the
  `rcl_X_init` / `rcl_get_zero_initialized_X` quartet.
- Allocators are still threaded through explicitly (overview §3.6) — the
  allocator is even *stored inside* `rcl_params_t` so every later mutation uses
  the same one.

The `src/impl/*.h` headers here are **internal helper declarations** (shared
between `.c` files), not PIMPL definitions.

---

## 3. Package layout

```
rcl_yaml_param_parser/
├── CMakeLists.txt
├── package.xml                 # deps: libyaml_vendor, yaml, rcutils, rmw
├── include/rcl_yaml_param_parser/
│   ├── parser.h                # THE public API (9 functions)
│   ├── types.h                 # the public data structures
│   └── visibility_control.h
├── src/
│   ├── parser.c                # public API impl: init/copy/fini/parse/get/print
│   ├── parse.c                 # core: walk libyaml events, parse values
│   ├── node_params.c           # manage the per-node parameter arrays
│   ├── namespace.c             # build/track parameter namespaces while parsing
│   ├── yaml_variant.c          # init/copy/fini a single rcl_variant_t value
│   ├── add_to_arrays.c         # append-with-growth for typed value arrays
│   └── impl/*.h                # internal headers shared across the .c files
└── test/                       # gtest + performance (benchmark) tests
```

Build facts: one shared library `librcl_yaml_param_parser`; `libyaml` is linked
**`PRIVATE`** (an implementation detail not exposed to consumers).

---

## 4. The data model (`types.h`)

The structures nest from process → node → parameter → value:

```
rcl_params_t                      (everything parsed from one file/process)
├── allocator                     (rcutils_allocator_t — reused for all mutations)
├── node_names[]      ┐ parallel
├── params[]          ┘ arrays    (one rcl_node_params_t per node)
│      │
│      └── rcl_node_params_t      (all params of ONE node)
│             ├── parameter_names[]   ┐ parallel
│             └── parameter_values[]  ┘ arrays
│                    │
│                    └── rcl_variant_t   (ONE parameter's value)
├── num_nodes / capacity_nodes    (size vs. allocated — grow on demand)
```

Key design points:

- **Parallel arrays, not maps.** `node_names[i]` pairs with `params[i]`; inside a
  node, `parameter_names[j]` pairs with `parameter_values[j]`. Lookups are linear
  scans (fine for the small sizes parameter files reach).
- **Capacity vs. count.** Both levels carry a `num_*` and a `capacity_*`, so the
  structure grows amortized as parsing discovers more nodes/params
  (`rcl_yaml_node_struct_reallocate`, `node_params_reallocate`).

### `rcl_variant_t` — a tagged union by convention

A parameter value is a struct of **pointers, exactly one of which is non-NULL**:

```c
typedef struct rcl_variant_s {
  bool * bool_value;
  int64_t * integer_value;
  double * double_value;
  char * string_value;
  rcl_byte_array_t * byte_array_value;
  rcl_bool_array_t * bool_array_value;
  rcl_int64_array_t * integer_array_value;
  rcl_double_array_t * double_array_value;
  rcutils_string_array_t * string_array_value;
} rcl_variant_t;
```

So the supported parameter types are: `bool`, `int64`, `double`, `string`, and
arrays of each (`byte[]`, `bool[]`, `int64[]`, `double[]`, `string[]`). To read a
variant you check which pointer is set. The typed array structs
(`rcl_bool_array_t`, `rcl_int64_array_t`, `rcl_double_array_t`,
`rcl_byte_array_t`) are each just `{ values*, size }`.

---

## 5. Public API (`parser.h`)

Nine functions, in three groups:

### Lifecycle of the structure
| Function | Purpose |
| --- | --- |
| `rcl_yaml_node_struct_init(allocator)` | Allocate an empty `rcl_params_t`. |
| `rcl_yaml_node_struct_init_with_capacity(capacity, allocator)` | Same, pre-sized for `capacity` nodes. |
| `rcl_yaml_node_struct_reallocate(params, new_capacity, allocator)` | Grow the node capacity. |
| `rcl_yaml_node_struct_copy(params)` | Deep-copy an entire structure. |
| `rcl_yaml_node_struct_fini(params)` | Free everything. |

### Parsing (the core entry points)
| Function | Purpose |
| --- | --- |
| `rcl_parse_yaml_file(file_path, params)` | Parse a whole `.yaml` file into `params`. Returns `bool`. |
| `rcl_parse_yaml_value(node_name, param_name, yaml_value, params)` | Parse a **single** value string for one `node/param` (used for per-parameter overrides, e.g. `-p name:=value`). |

### Access / debug
| Function | Purpose |
| --- | --- |
| `rcl_yaml_node_struct_get(node_name, param_name, params)` | Return the `rcl_variant_t*` for a param, zero-initializing it if absent. |
| `rcl_yaml_node_struct_print(params)` | Dump the structure to stdout (debugging). |

Typical usage:

```c
rcl_params_t * params = rcl_yaml_node_struct_init(rcutils_get_default_allocator());
if (!rcl_parse_yaml_file("/path/to/params.yaml", params)) { /* handle error */ }
rcl_variant_t * v = rcl_yaml_node_struct_get("my_node", "max_speed", params);
if (v && v->double_value) { double speed = *v->double_value; /* ... */ }
rcl_yaml_node_struct_fini(params);
```

---

## 6. How parsing works (source map)

The implementation is split by responsibility:

- **`parser.c`** — implements the public API. `rcl_parse_yaml_file` opens the
  file, hands it to a `yaml_parser_t` (libyaml), and drives the event loop.
- **`parse.c`** — the core. Walks the stream of libyaml events
  (`get_value`, `parse_value`, plus map/sequence handlers) and decides the type
  of each scalar, converting it into the right `rcl_variant_t` pointer. This is
  where YAML text becomes typed C values.
- **`namespace.c`** — tracks the current parameter **namespace** as the parser
  descends into nested maps (`add_name_to_ns` / `rem_name_from_ns` /
  `replace_ns`), so a nested key becomes a dotted parameter name.
- **`node_params.c`** — manages a single node's parameter arrays (init / grow /
  fini).
- **`add_to_arrays.c`** — appends a value to a typed value array, growing it
  (`add_val_to_bool_arr`, `…_int_arr`, `…_double_arr`, `…_string_arr`) — this is
  how YAML sequences become the typed array variants.
- **`yaml_variant.c`** — init / deep-copy / fini for a single `rcl_variant_t`.

Flow at a glance:

```
rcl_parse_yaml_file
  └─ libyaml emits events (stream/document/mapping/sequence/scalar)
       parse.c walks them:
         ├─ map keys      → namespace.c builds the dotted param name
         ├─ scalar value  → parse.c infers type → yaml_variant.c stores it
         └─ sequence item → add_to_arrays.c appends to the typed array
       node_params.c / parser.c grow the params_st arrays as needed
```

---

## 7. Tests

Mirrors the source split: `test/test_parser.cpp`, `test_parse.cpp`,
`test_parse_yaml.cpp`, `test_node_params.cpp`, `test_namespace.cpp`,
`test_yaml_variant.cpp`, plus multi-node / multi-param scenarios
(`test_parser_multiple_nodes.cpp`, `test_parser_multiple_params.cpp`). gtest +
`mimick_vendor` for failure injection.

Notably this package also ships **performance/benchmark tests** via
`performance_test_fixture` (`test/benchmark/benchmark_parse_yaml.cpp`,
`benchmark_variant.cpp`) — parsing is on the node startup path, so it is
benchmarked.

---

## 8. Suggested reading order

1. **`include/rcl_yaml_param_parser/types.h`** — the whole data model fits on one
   screen; understand the nesting (§4).
2. **`include/rcl_yaml_param_parser/parser.h`** — the 9-function API (§5).
3. **`src/parser.c`** — see init/fini/copy and how `rcl_parse_yaml_file` drives
   libyaml.
4. **`src/parse.c`** — the type-inference core; then `namespace.c` and
   `add_to_arrays.c` for the two subtleties (nested names, sequences).
5. A `test_*.cpp` for an executable example of the structure being populated and
   walked.

This package is small enough to read end-to-end in one sitting — a good way to
build confidence before tackling the larger packages.
```
