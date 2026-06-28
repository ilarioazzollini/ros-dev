# ros-dev

A minimal, containerized development environment for working on ROS 2 source
repositories. Clone the repo(s) you want to work on into `repos/`, link the ones
you care about into a colcon workspace, and build and test them inside Docker.

For a worked, end-to-end example of using this environment to contribute to ROS 2,
see the blog post:
[Contributing to ROS 2](https://ilarioazzollini.github.io/2026-06-13-contributing-to-ros-2/).

*Prerequisites*:
- Host PC with Ubuntu 24.04 (although it is *not necessary*)
- Install [git](https://git-scm.com/install/linux)
- Install [docker](https://docs.docker.com/engine/install/ubuntu/) and also follow the [post-installation steps](https://docs.docker.com/engine/install/linux-postinstall/)
- Install [Visual Studio Code](https://code.visualstudio.com/docs/setup/linux) (*not necessary*)

# 1. Fork and clone the ROS 2 repo(s)

Fork the repository (or repositories) you want to work on on GitHub (uncheck the
"copy the rolling branch only" checkbox if you may want to backport to other ROS 2
versions), then clone each one into `repos/`:

```bash
git clone git@github.com:<your-username>/<repo>.git repos/<repo>
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

# 4. Set up the workspace

The build/test scripts operate on `ros2_ws/`, a colcon workspace. Link the repos
you want to build into `ros2_ws/src/` (everything under `src/` is built and
tested). Inside the container:

```bash
ln -s /root/ros-dev/repos/<repo> /root/ros-dev/ros2_ws/src/<repo>
```

Link only the repos relevant to your current work. To switch focus later, remove
the symlinks you no longer need and add the ones you do — the rest of your cloned
repos can stay in `repos/` untouched.

# 5. Build and test inside the container

The scripts under `scripts/` build (and optionally test) whatever is linked into
`ros2_ws/src/`. They cap build parallelism so the heavy C++ compiles fit in the
Docker VM's RAM.

**Build only:**

```bash
bash scripts/clean_build.sh   # full, from-scratch build (the first time / after dependency changes)
bash scripts/build.sh         # fast incremental build (day-to-day)
```

**Build and test** — same as above, but then run the suites with `colcon test`
and `colcon test-result --verbose`, which exits non-zero if any test failed.
Use these to confirm the suite is green before proposing a PR or merge:

```bash
bash scripts/clean_build_test.sh   # full build + test
bash scripts/build_test.sh         # incremental build + test
```

Re-run the `clean_*` variants after changing dependencies or whenever the
workspace gets into a bad state.

## Notes

- The Docker image upgrades the base image's ROS packages to the current rolling
  release (`apt dist-upgrade`). The base image is a snapshot whose ROS packages
  lag behind the upstream sources; without the upgrade, the generated interface
  targets don't match and the build fails (e.g. `find_package` succeeds but an
  expected interface target is missing).
- The image also bakes in the test/benchmark dependencies that the desktop-full
  image omits and that `rosdep` cannot resolve on Ubuntu 24.04 (`test_msgs`,
  `mimick_vendor`, `osrf_testing_tools_cpp`, `performance_test_fixture`,
  `ament_cmake_google_benchmark`), plus `rcl_logging_implementation` for the
  default dynamic logging backend.
- The builds run with `--parallel-workers 1` and `MAKEFLAGS=-j2` to stay within
  ~4 GB of RAM. If you give Docker more memory, speed them up with e.g.
  `COLCON_PARALLEL_WORKERS=4 MAKEFLAGS="-j8" bash scripts/build.sh`.
