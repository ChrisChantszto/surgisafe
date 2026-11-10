# Linux x86-64 build environment for QNX SDP 8.0 on macOS (including Apple
# Silicon, via Docker Desktop's amd64 emulation).
#
# QNX SDP 8.0 host tools run only on Linux/Windows x86-64, so this container
# provides the Linux host. The SDP itself is NOT baked into the image: it is
# installed into ~/qnx800 on the Mac (shared with the container through the
# home-directory mount), exactly where QNX's own docs expect it.
#
# Based on QNX's docker-mac example (gitlab.com/qnx/projects/docker-mac),
# updated to Ubuntu 22.04 and trimmed to what SurgiSafe-RT needs.

FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive

# Build tools, plus the libraries QNX Software Center's Linux client needs
# (it is an Eclipse-based application, even when driven from the command
# line).
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential make git curl ca-certificates sudo locales \
        python3 python3-venv python3-pip \
        file rsync unzip xz-utils bc procps \
        openssh-client \
        libgtk-3-0 libxtst6 libnss3 libasound2 \
    && locale-gen en_US.UTF-8 \
    && rm -rf /var/lib/apt/lists/*

ENV LANG=en_US.UTF-8 \
    LC_ALL=en_US.UTF-8

# A user matching the Mac user, so files created in the shared home
# directory keep the right owner. macOS's default group id (20) clashes with
# an existing Ubuntu group, hence -o (non-unique) and a neutral group name.
ARG USER_NAME
ARG USER_ID
ARG GROUP_ID
RUN groupadd -o --gid "${GROUP_ID}" qnxdev \
    && useradd -o --uid "${USER_ID}" --gid "${GROUP_ID}" --groups sudo \
        --no-create-home --home-dir "/home/${USER_NAME}" --shell /bin/bash \
        "${USER_NAME}" \
    && echo "${USER_NAME} ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/qnxdev \
    && chmod 0440 /etc/sudoers.d/qnxdev

# Load the QNX environment automatically in interactive shells, once the SDP
# has been installed.
RUN printf '%s\n' \
    'if [ -f "$HOME/qnx800/qnxsdp-env.sh" ]; then' \
    '  source "$HOME/qnx800/qnxsdp-env.sh" > /dev/null' \
    'fi' \
    'PS1="[QNX] \\u:\\w\\$ "' \
    > /etc/qnx-bashrc

USER ${USER_NAME}
WORKDIR /home/${USER_NAME}
CMD ["/bin/bash", "--rcfile", "/etc/qnx-bashrc"]
