#!/bin/bash
# Builds the QNX build image and creates the "qnx-build" container.
#
#   qnx/docker/setup.sh
#
# The container mounts your Mac home directory at the same path layout QNX's
# docs use (/home/<user>), so ~/qnx800, ~/.qnx and this repository are
# visible inside it. Run commands in it with:
#
#   docker exec -it qnx-build bash --rcfile /etc/qnx-bashrc

set -euo pipefail

IMAGE=surgisafe-qnx-build
CONTAINER=qnx-build
HERE="$(cd "$(dirname "$0")" && pwd)"

docker build --platform linux/amd64 -t "$IMAGE" \
    --build-arg USER_NAME="$(id -un)" \
    --build-arg USER_ID="$(id -u)" \
    --build-arg GROUP_ID="$(id -g)" \
    "$HERE"

if docker container inspect "$CONTAINER" > /dev/null 2>&1; then
    echo "Container $CONTAINER already exists; replacing it."
    docker rm -f "$CONTAINER" > /dev/null
fi

docker create -it --name "$CONTAINER" --platform linux/amd64 \
    --hostname qnx-build --restart unless-stopped \
    -v "$HOME:/home/$(id -un)" \
    "$IMAGE:latest" > /dev/null
docker start "$CONTAINER" > /dev/null

echo "Container $CONTAINER is running."
echo "Open a shell in it with: docker exec -it $CONTAINER bash --rcfile /etc/qnx-bashrc"
