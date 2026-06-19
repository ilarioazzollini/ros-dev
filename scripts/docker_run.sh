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
