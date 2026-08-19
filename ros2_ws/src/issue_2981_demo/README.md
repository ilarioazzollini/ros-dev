# issue_2981_demo

Before/after capability demos for [ros2/rclcpp#2981](https://github.com/ros2/rclcpp/issues/2981) ("Save / Dump / Load parameters to/from file"). Shows, with real code you can build and run, what already works today and what doesn't.

In order to run all of these demos, follow these steps:

Clone my forks of `rcl` and `rclcpp` and switch to the relevant branch. For `rclcpp`:

```bash
cd ros-dev/repos/
git clone git@github.com:ilarioazzollini/rclcpp.git
cd rclcpp
git checkout ilo/rclcpp-issue-2981
```

And for `rcl`:
```bash
cd ros-dev/repos/
git clone git@github.com:ilarioazzollini/rcl.git
cd rcl
git checkout ilo/rclcpp-issue-2981
```

Link the repos in the workspace:
```bash
ln -s /root/ros-dev/repos/rcl /root/ros-dev/ros2_ws/src/rcl

ln -s /root/ros-dev/repos/rclcpp /root/ros-dev/ros2_ws/src/rclcpp
```

Build and source the whole workspace:
```bash
bash /root/ros-dev/scripts/clean_build_test.sh
source /root/ros-dev/ros2_ws/install/setup.bash
```

## `load_demo`: load parameters from a YAML file

With this demo, we show basic YAML file load capabilities already available in the `rclcpp` API, with no ROS nodes involved. See [`src/load_demo.cpp`](src/load_demo.cpp) for details.

In order to run the executable:

```bash
ros2 run issue_2981_demo load_demo /root/ros-dev/ros2_ws/src/issue_2981_demo/params/sprayer_params.yaml
```

Loads a params YAML file like [`params/sprayer_params.yaml`](params/sprayer_params.yaml) via `rclcpp::parameter_map_from_yaml_file()` and prints what comes back.

## `param_holder_node`: a node holding some parameters

With this demo, we show the following CLI features related to a ROS node:
- how to load parameters at startup with `ros2 run <package> <node> --ros-args --params-file <file>`
- how to print parameters to stdout (or save parameters to file) at runtime with `ros2 param dump <node>`
- how to load parameters at runtime with `ros2 param load <node> <file>`

See [`src/param_holder_node.cpp`](src/param_holder_node.cpp) for details.

When we run the `param_holder_node` executable, it brings up a `/sprayer` node, and we can initialize its parameters at startup via a YAML file by running in a first terminal:

```bash
ros2 run issue_2981_demo param_holder_node --ros-args --params-file /root/ros-dev/ros2_ws/src/issue_2981_demo/params/sprayer_params_gentle.yaml
```

Then, in a second terminal, we can run:

```bash
ros2 param dump /sprayer
```

and see its parameters printed to the standard output channel on our terminal. This means that we can also easily save them to a yaml file by:

```bash
ros2 param dump /sprayer > /root/ros-dev/ros2_ws/src/issue_2981_demo/params/sprayer_params_dumped.yaml
```

> Note that this is a valid yaml file, and we can test it by simply running load_demo on it again:
> ```bash
> ros2 run issue_2981_demo load_demo /root/ros-dev/ros2_ws/src/issue_2981_demo/params/sprayer_params_dumped.yaml
> ```

Now, we can load different parameters into the sprayer node, by running on the second terminal:

```bash
ros2 param load /sprayer /root/ros-dev/ros2_ws/src/issue_2981_demo/params/sprayer_params.yaml
```

and check that they were set successfully by running `ros2 param dump /sprayer` again.

> Note: this last step actually reproduces a real bug in `ros2 param load` itself (the
> `rclpy` code path, not `rcl_yaml_param_parser`) -- `spray_pattern` fails to set:
> `Wrong parameter type, parameter {spray_pattern} is of type {string}, setting it to
> {bool} is not allowed`. `rclpy` re-infers the new value's type from a re-stringified
> copy of it and loses the fact it was quoted `"no"` in the source YAML, so it tries to
> set it as a bool instead. `spray_pattern` stays at its previous value (`mist`) rather
> than switching. Out of scope for the current work.

## `switch_config_demo`: still loading, but from C++ code

With this demo, we show that `ros2 param load` isn't just a CLI trick: the underlying capability is a plain `rclcpp` API, `rclcpp::SyncParametersClient::load_parameters(yaml_filename)` (`parameter_client.hpp`) -- its own doc comment says *"This function behaves like command-line tool `ros2 param load` would."* See [`src/switch_config_demo.cpp`](src/switch_config_demo.cpp) for details.

With `param_holder_node` running (see above), we can call it directly, no shell CLI involved, by opening a terminal and running:

```bash
ros2 run issue_2981_demo switch_config_demo /sprayer /root/ros-dev/ros2_ws/src/issue_2981_demo/params/sprayer_params_gentle.yaml
```

This reproduces the exact same live switch as the `ros2 param load` example above, purely from a C++ program. Under the hood it's the same two pieces already covered in this repo: parse the YAML with `rclcpp::parameter_map_from_yaml_file()` (see `load_demo` above), then `set_parameters()` against the target node over the ROS graph.

## `self_reload_demo`: same-process self-reload from C++ code

With this demo, we show that a node doesn't even need the parameter services to reload its own configuration: both variants above go through them (a ROS graph RPC), even though `switch_config_demo` happens to run against the same machine. `self_reload_demo` shows the more direct case, arguably closer to what the issue itself asks for ("a decentralized, in-node way..."): a node reloading its own parameters straight from a file, with no services and no second process at all. See [`src/self_reload_demo.cpp`](src/self_reload_demo.cpp) for details.

We can run it on its own, in a single terminal, with no other node needed:

```bash
ros2 run issue_2981_demo self_reload_demo /root/ros-dev/ros2_ws/src/issue_2981_demo/params/sprayer_params_gentle.yaml
```

It declares parameters matching [`params/sprayer_params.yaml`](params/sprayer_params.yaml)'s values, prints them, reloads `sprayer_params_gentle.yaml` into itself with `rclcpp::parameter_map_from_yaml_file()` + `set_parameters()`, and prints the result. Nothing here is new API -- it's the same load path `load_demo` already exercises, combined with the plain `Node::set_parameters()` every node already has. It never got its own file until now because it isn't a capability gap, just an un-demoed combination of two things this package already shows separately.

## `param_holder_node_save`: save a node's parameters to a YAML file

With this demo, we show the new capability #2981 adds: a node can now serialize its own parameters to YAML from inside its own process -- correctly, with no external CLI and no hand-rolled string concatenation. See [`src/param_holder_node_save.cpp`](src/param_holder_node_save.cpp) for details.

When we run the `param_holder_node_save` executable, it brings up the same `/sprayer` node as `param_holder_node`, with the same parameters -- including `spray_pattern`, deliberately set to the string `"no"`. In a terminal, run:

```bash
ros2 run issue_2981_demo param_holder_node_save --ros-args -p save_path:=/root/ros-dev/ros2_ws/src/issue_2981_demo/params/sprayer_params_saved.yaml
```

and press Ctrl-C. Rather than just exiting, the node calls the new `rclcpp::serialize_parameters()` free function, prints the resulting YAML, and writes it to `save_path` (default `sprayer_params_saved.yaml`, override with `--ros-args -p save_path:=/some/path.yaml`).

We can check that it round-trips correctly by loading the saved file back with `load_demo`:

```bash
ros2 run issue_2981_demo load_demo /root/ros-dev/ros2_ws/src/issue_2981_demo/params/sprayer_params_saved.yaml
```

and see that `spray_pattern` comes back as the string `"no"`, exactly as it started. That's the concrete problem this feature solves: a plain, unquoted `no` is a valid YAML boolean literal, so a serializer that isn't a real YAML emitter -- one built by hand from `get_parameters()` / `Parameter::value_to_string()`, say -- would silently turn it into `false` instead. `rclcpp::serialize_parameters()` goes through the same YAML library (`libyaml`, via `rcl_yaml_param_parser`) the parser already uses, so it quotes correctly.

## Summary

Before 2981, this package shows that **loading** parameters at runtime already had several ways to do it: the `ros2 param load` CLI (`param_holder_node`), the same thing from C++ code against another node (`switch_config_demo`), and a node reloading its own parameters with no ROS graph involved at all (`self_reload_demo`). What was missing was the mirror image: **saving** a node's live parameters back to a YAML file, correctly, from in-process code. `ros2 param dump` covers it externally, but there was no `rclcpp`/`rcl` call for it, and hand-rolling one is not YAML-safe -- an unquoted string like `"no"` silently becomes the boolean `false`.

`param_holder_node_save` shows that this gap is now closed: `rclcpp::serialize_parameters()`, backed by a real YAML emitter in `rcl_yaml_param_parser`, produces correct output that a node can write anywhere it likes.

### Possible future improvements

- report or even fix the rclpy bug
- 2981 adds the SAVE half: `rclcpp::serialize_parameters(params_interface,
base_interface) -> std::string`. The LOAD half already works today, but only
as a *composition* the caller has to know to write themselves:
    ```cpp
    auto map = rclcpp::parameter_map_from_yaml_file(
    yaml_filename, base_interface->get_fully_qualified_name());
    auto params = rclcpp::parameters_from_map(map);
    auto results = params_interface->set_parameters(params);
    ```
    The idea: give the same-process case its own named free function too, so it's
    discoverable and symmetric with `serialize_parameters()`, rather than requiring
    users to already know the 3-line composition exists.
- Same idea for `rclcpp::SyncParametersClient::load_parameters(yaml_filename)`. Is there a `rclcpp::SyncParametersClient::save_parameters(yaml_filename)` already? If not, would it be useful to add it?
