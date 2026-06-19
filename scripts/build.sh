#!/usr/bin/env bash
# Fast incremental build of rcl + rclcpp for day-to-day development.
# Skips the apt/rosdep/clean steps and lets colcon rebuild only what changed.
# Run ./clean_build.sh instead the first time, after changing dependencies, or
# whenever the workspace is in a bad state.
set -e

# Make the local rcl and rclcpp sources visible to the workspace. Only create
# the symlinks if missing: re-touching them bumps their mtime and would force an
# unnecessary CMake reconfigure on every incremental build.
[ -L /root/ros-dev/ros2_ws/src/rcl ]    || ln -s /root/ros-dev/repos/rcl    /root/ros-dev/ros2_ws/src/rcl
[ -L /root/ros-dev/ros2_ws/src/rclcpp ] || ln -s /root/ros-dev/repos/rclcpp /root/ros-dev/ros2_ws/src/rclcpp

cd /root/ros-dev/ros2_ws

# Source the workspace overlay so colcon sees the already-built rcl/rclcpp (falls
# back to the underlay on the very first build, before install/ exists).
if [ -f install/setup.bash ]; then
    source install/setup.bash
else
    source /opt/ros/rolling/setup.bash
fi

# Cap build parallelism so heavy rclcpp/C++ compiles fit in the Docker VM's RAM
# (avoids "cc1plus killed" OOM errors). Raise these if you give Docker more
# memory: e.g. COLCON_PARALLEL_WORKERS=4 MAKEFLAGS="-j8" ./build.sh
export COLCON_PARALLEL_WORKERS="${COLCON_PARALLEL_WORKERS:-1}"
export MAKEFLAGS="${MAKEFLAGS:--j2}"

colcon build \
    --packages-select rcl rclcpp \
    --symlink-install \
    --parallel-workers "${COLCON_PARALLEL_WORKERS}" \
    --cmake-args \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
source install/local_setup.bash
