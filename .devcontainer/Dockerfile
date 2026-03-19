FROM mcr.microsoft.com/devcontainers/base:ubuntu-24.04

# Install xmake and common build dependencies in the image so every rebuild has it.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        xmake \
        build-essential \
        ninja-build \
        git \
        pkg-config \
        ca-certificates \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*
