# Introduction

This repo should serve as a minimal quickstart guide about how to practically contribute to the Robot Operating System (ROS) 2 project. In particular, we are going to focus on [*rclcpp*](https://github.com/ros2/rclcpp), that is the ROS Client Library for C++ packages, included with a standard install of any ROS 2 distro. In practice, *rclcpp* provides the standard C++ API for interacting with ROS 2.

*Prerequisites*:
- Host PC with Ubuntu 24.04 (although it is *not necessary*)
- Install [git](https://git-scm.com/install/linux)
- Install [docker](https://docs.docker.com/engine/install/ubuntu/) and also follow the [post-installation steps](https://docs.docker.com/engine/install/linux-postinstall/)
- Install [Visual Studio Code](https://code.visualstudio.com/docs/setup/linux) (*not necessary*)

# 1. Fork and clone the ROS 2 repo(s)

First of all we should go to Github and fork the repository (or repositories) that we are going to work on.

For instance, say we are going to work on *rclcpp*. We can create a fork from our own account (uncheck the "copy the rolling branch only" checkbox, as we may want to backport the changes to other versions of ROS 2 as well), and then we can clone it here (locally on our PC).

```bash
cd ros-dev
git clone git@github.com:ilarioazzollini/rclcpp.git
```

# 2. Build the Docker image

Change directory to `ros-dev` and:

```bash
docker build \
  --no-cache \
  --platform linux/amd64 \
  -f docker/Dockerfile \
  -t ros-rolling-dev \
  .
```

# 3. Run a Docker container from the image

Change directory to `ros-dev` and:

```bash
docker run \
  --platform linux/amd64 \
  -it \
  --rm \
  --privileged \
  --network=host \
  -v ${PWD}:/root/ros-dev \
  -w /root/ros-dev \
  --name ros-dev-container \
  ros-rolling-dev \
  bash
```

# Build rcl and rclcpp inside the container

Two helper scripts under `scripts/` build `rcl` + `rclcpp`. Both create the
`ros2_ws/src/{rcl,rclcpp}` symlinks into `repos/` automatically and cap build
parallelism so the heavy C++ compiles fit in the Docker VM's RAM.

Run a **full, from-scratch build** the first time (it wipes previous artifacts,
refreshes dependencies with `rosdep`, and rebuilds everything):

```bash
bash scripts/clean_build.sh
```

For day-to-day work, use the **fast incremental build** (rebuilds only what
changed):

```bash
bash scripts/build.sh
```

Re-run `clean_build.sh` after changing dependencies or whenever the workspace
gets into a bad state.

## Notes

- The Docker image upgrades the base image's ROS packages to the current rolling
  release (`apt dist-upgrade`). The base image is a snapshot whose ROS packages
  lag behind the `rcl`/`rclcpp` sources; without the upgrade, the generated
  interface targets don't match and the build fails (e.g. `find_package`
  succeeds but the `rcl_interfaces::rcl_interfaces` target is missing).
- The image also bakes in the test/benchmark dependencies that the desktop-full
  image omits and that `rosdep` cannot resolve on Ubuntu 24.04 (`test_msgs`,
  `mimick_vendor`, `osrf_testing_tools_cpp`, `performance_test_fixture`,
  `ament_cmake_google_benchmark`), plus `rcl_logging_implementation`, which
  `rcl` needs for its default dynamic logging backend.
- The builds run with `--parallel-workers 1` and `MAKEFLAGS=-j2` to stay within
  ~4 GB of RAM. If you give Docker more memory, speed them up with e.g.
  `COLCON_PARALLEL_WORKERS=4 MAKEFLAGS="-j8" bash scripts/build.sh`.
