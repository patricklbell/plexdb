FROM ubuntu:24.04

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

# Required network domains for apt and toolchain setup:
#   archive.ubuntu.com        – Ubuntu package repository
#   security.ubuntu.com       – Ubuntu security updates
#   apt.llvm.org              – LLVM/Clang 19 apt repository + signing key
#   github.com                – CMake FetchContent (Catch2, lexy deps)
#   objects.githubusercontent.com – GitHub release/archive downloads
#   api.github.com            – GitHub API (cmake version resolution)
#   registry.npmjs.org        – npm packages (cql_node_example)
#   pypi.org                  – Python packages (cql_py_example)
#   files.pythonhosted.org    – Python package downloads
#   astral.sh                 – uv installer script
#   storage.googleapis.com    – used by some package install scripts

# @note sudo intentionally omitted (see blog.danieldemmel.me/coding-agents-in-secured-vscode-dev-containers)
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    gnupg \
    lsb-release \
    && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
    | gpg --dearmor -o /usr/share/keyrings/llvm.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] https://apt.llvm.org/noble/ llvm-toolchain-noble-19 main" \
    > /etc/apt/sources.list.d/llvm.list

RUN apt-get update && apt-get install -y --no-install-recommends \
    # Build system
    cmake \
    ninja-build \
    # C++ compiler (clang-19 matches copilot-setup-steps.yml)
    clang-19 \
    clang-tools-19 \
    # io_uring support (required by plexdb)
    liburing-dev \
    # gRPC / Protobuf (required by log_otel plugin)
    libgrpc++-dev \
    libprotobuf-dev \
    protobuf-compiler \
    protobuf-compiler-grpc \
    # Node.js / npm (required by cql_node_example)
    nodejs \
    npm \
    # Python runtime (required by cql_py_example and cql_tests)
    python3 \
    python3-pip \
    # Developer utilities
    git \
    vim \
    less \
    htop \
    ripgrep \
    fd-find \
    tree \
    jq \
    unzip \
    wget \
    locales \
    docker-cli \
    && rm -rf /var/lib/apt/lists/*

RUN update-alternatives --install /usr/bin/clang   clang   /usr/bin/clang-19   100 \
    && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-19 100

# generate UTF-8 locale (mirrors common cloud container image conventions)
RUN sed -i '/en_US.UTF-8/s/^# //g' /etc/locale.gen && locale-gen
ENV LANG=en_US.UTF-8
ENV LC_ALL=en_US.UTF-8

ARG USERNAME=vscode
ARG USER_UID=1000
ARG USER_GID=$USER_UID

RUN if getent group $USER_GID >/dev/null 2>&1; then \
        useradd --uid $USER_UID --gid $USER_GID -m $USERNAME; \
    else \
        groupadd --gid $USER_GID $USERNAME \
        && useradd --uid $USER_UID --gid $USER_GID -m $USERNAME; \
    fi

RUN mkdir -p /app && chown $USER_UID:$USER_GID /app

USER $USERNAME

ENV SHELL=/bin/bash

# Security hardening: unset VS Code IPC / credential-forwarding environment variables
# that would allow the agent to escape the container.
# Sourced from .bashrc BEFORE the Debian interactive guard so it applies to
# non-interactive login shells (the way Claude Code and GitHub Copilot invoke bash).
RUN mkdir -p /home/${USERNAME}/.config && cat << 'HARDEN' > /home/vscode/.config/security-harden.sh
# VS Code IPC sockets – can execute commands on the host
unset VSCODE_IPC_HOOK_CLI

# VS Code Git extension IPC – credential access via host
unset VSCODE_GIT_IPC_HANDLE \
      GIT_ASKPASS \
      VSCODE_GIT_ASKPASS_MAIN \
      VSCODE_GIT_ASKPASS_NODE \
      VSCODE_GIT_ASKPASS_EXTRA_ARGS

# Remote Containers extension IPC – host command execution bridge
unset REMOTE_CONTAINERS_IPC \
      REMOTE_CONTAINERS_SOCKETS \
      REMOTE_CONTAINERS_DISPLAY_SOCK

# GUI forwarding (low risk but unnecessary)
unset WAYLAND_DISPLAY

# Browser helper – can trigger actions on host via --openExternal
# Set to empty rather than unset to prevent fallback to defaults
export BROWSER=

# Agent forwarding – empty to prevent fallback to default socket paths
export SSH_AUTH_SOCK=
export GPG_AGENT_INFO=
HARDEN

# Source the hardening script on line 1 of .bashrc, before the interactive guard
RUN sed -i '1i source ~/.config/security-harden.sh 2>/dev/null || true' ~/.bashrc \
    && sed -i '2i export PATH="$HOME/.local/bin:$PATH"' ~/.bashrc \
    && mkdir -p /home/vscode/.local/bin

WORKDIR /app

CMD ["sleep", "infinity"]
