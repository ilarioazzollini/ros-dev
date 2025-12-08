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
    -f docker/Dockerfile \
    -t ros-rolling-dev \
    .
```

# 3. Run a Docker container from the image

Change directory to `ros-dev` and:

```bash
docker run \
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

# Build rclcpp inside the container

```bash
ln -s /root/ros-dev/rclcpp/ /root/ros-dev/ros2_ws/src/rclcpp
cd /root/ros-dev/ros2_ws
apt-get update && apt-get upgrade
rosdep install --from-paths src -y --ignore-src
colcon build
colcon test
source install/local_setup.bash
```
