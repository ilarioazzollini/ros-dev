# Ubuntu with ROS rolling
FROM osrf/ros:rolling-desktop-full

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

# Install other packages
RUN apt-get update && apt-get install -q -y \
    clang-format \
    clang-tidy \
    clangd \
    lldb \
    && rm -rf /var/lib/apt/lists/*

# Automatically source ROS packages when opening new shell
RUN echo ' \n\
echo "Sourcing ROS2 packages..." \n\
source /opt/ros/rolling/setup.bash \n\
echo "Done!"\
' >> $HOME/.bashrc
