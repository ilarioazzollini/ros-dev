# Ubuntu with ROS rolling
FROM osrf/ros:rolling-desktop-full
# FROM osrf/ros2:devel
# FROM ros:rolling-ros-base

# Choose home directory and set it as working directory
ENV HOME=/root
WORKDIR ${HOME}

# Install packages without prompting the user to answer any questions
ENV DEBIAN_FRONTEND="noninteractive"

# Timezone and locale
ENV TZ="Europe/Rome"
RUN apt-get update && apt-get install -y \
    tzdata \
    locales \
    && rm -rf /var/lib/apt/lists/*
RUN locale-gen en_US en_US.UTF-8 && update-locale LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8
ENV LANG=en_US.UTF-8

# Upgrade the ROS packages shipped in the base image to the current rolling
# release. The base image is a snapshot and its ROS packages (e.g. rosidl_*,
# rcl_interfaces) lag behind the rcl/rclcpp sources we build against. That
# mismatch breaks the generated interface targets (e.g. find_package() succeeds
# but does not create the "rcl_interfaces::rcl_interfaces" target). Baking the
# upgrade into the image keeps EVERY container - including fresh ones used for
# incremental builds - aligned with the sources.
RUN apt-get update && apt-get dist-upgrade -y \
    && rm -rf /var/lib/apt/lists/*

# Install other packages
RUN apt-get update && apt-get install -q -y \
    clang-format \
    clang-tidy \
    clangd \
    lldb \
    && rm -rf /var/lib/apt/lists/*

# Build/runtime dependency of rcl that is not shipped with the desktop-full
# image: rcl defaults to the dynamic logging backend, which find_package()s
# rcl_logging_implementation at configure time. Baking it in (rather than
# relying on rosdep) keeps incremental builds working in fresh containers.
RUN apt-get update && apt-get install -q -y \
    ros-rolling-rcl-logging-implementation \
    && rm -rf /var/lib/apt/lists/*

# Test/benchmark dependencies of rcl/rclcpp that are not shipped with the
# desktop-full image and that rosdep cannot resolve on noble
RUN apt-get update && apt-get install -q -y \
    ros-rolling-test-msgs \
    ros-rolling-ament-cmake-google-benchmark \
    ros-rolling-mimick-vendor \
    ros-rolling-performance-test-fixture \
    ros-rolling-osrf-testing-tools-cpp \
    && rm -rf /var/lib/apt/lists/*

# Automatically source ROS packages when opening new shell
RUN echo ' \n\
echo "Sourcing ROS2 packages..." \n\
source /opt/ros/rolling/setup.bash \n\
echo "Done!"\
' >> $HOME/.bashrc
