#!/usr/bin/env bash
# Full, from-scratch build of rcl + rclcpp.
# Wipes previous build artifacts, refreshes system/ROS dependencies and rebuilds
# everything. Use this the first time, after changing dependencies, or whenever
# the workspace is in a bad state. For day-to-day work prefer ./build.sh.
set -e

# Make the local rcl and rclcpp sources visible to the workspace (idempotent)
ln -sfn /root/ros-dev/repos/rcl    /root/ros-dev/ros2_ws/src/rcl
ln -sfn /root/ros-dev/repos/rclcpp /root/ros-dev/ros2_ws/src/rclcpp

cd /root/ros-dev/ros2_ws

# Cap build parallelism so heavy rclcpp/C++ compiles fit in the Docker VM's RAM
# (avoids "cc1plus killed" OOM errors). Raise these if you give Docker more
# memory: e.g. COLCON_PARALLEL_WORKERS=4 MAKEFLAGS="-j8" ./clean_build.sh
export COLCON_PARALLEL_WORKERS="${COLCON_PARALLEL_WORKERS:-1}"
export MAKEFLAGS="${MAKEFLAGS:--j2}"

# Start from a clean slate
rm -rf build install log

# Refresh system and ROS dependencies
apt-get update && apt-get dist-upgrade -y
rosdep update --rosdistro rolling
rosdep install \
    --from-paths src \
    --ignore-src \
    --rosdistro rolling \
    -r -y

colcon build \
    --packages-select rcl rclcpp \
    --symlink-install \
    --parallel-workers "${COLCON_PARALLEL_WORKERS}" \
    --cmake-args \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
source install/local_setup.bash
