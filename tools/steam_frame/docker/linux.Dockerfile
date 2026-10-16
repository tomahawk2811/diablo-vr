# The pinned Linux build environment for the Steam Frame build of the VR mode: Ubuntu 24.04 with
# its gcc 13 (the game needs C++23 and <format>) and CMake 3.31.6, plus what the two translation
# layers need. dxvk-native turns the renderer's Direct3D 11 into Vulkan and vkd3d compiles its HLSL.
# Binaries built here need glibc 2.39 and GLIBCXX 3.4.32 at most. The Frame has glibc 2.39 itself
# and 2.41 with GLIBCXX 3.4.33 inside Steam Linux Runtime 4, where Steam titles run.
# The image builds for the Docker host's architecture, so on an Apple Silicon Mac it is arm64,
# the Steam Frame's. Host testing runs on lavapipe (Mesa's software Vulkan) under Xvfb.
#
#   docker build -t diablo-vr-linux -f tools/steam_frame/docker/linux.Dockerfile tools/steam_frame/docker
#   tools/steam_frame/build.sh
ARG BASE_IMAGE=ubuntu:24.04
FROM ${BASE_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive \
    CC=gcc \
    CXX=g++

ARG CMAKE_VERSION=3.31.6
ARG CMAKE_SHA256_AARCH64=b4cc788d63112b2749b40627e719eb5d3b8ed8f00c36d77189f4019cfe64bc9e
ARG CMAKE_SHA256_X86_64=5a1133ff103c71eb5120e2cc3de922733e7d8a26a98ae716397e8676adb367bf
ARG MESON_VERSION=1.9.1
ARG GLSLANG_VERSION=16.6.0
ARG GLSLANG_SHA256=9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2
ARG KHRONOS_SDK=vulkan-sdk-1.4.350.0
ARG VULKAN_HEADERS_SHA256=70270d10bf2c1e074a06ee37a50b75d332993d1b80a1d9526eeed2da6d82ed22
ARG SPIRV_HEADERS_SHA256=9905d9341f20388adb852c77dd982f2c4d539fd68e6c1f1bcebf034715f2d1d5
ARG VKD3D_VERSION=2.1
ARG VKD3D_SHA256=7510146aff2adfb4ae07ab890701a607e5ff7c66e57100cfc9f630ec92eeda6a

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ninja-build pkg-config \
        git ca-certificates wget file rsync bzip2 xz-utils patch gettext \
        python3 python3-pip \
        flex bison perl libjson-perl mingw-w64-tools \
        libvulkan-dev vulkan-tools \
        libasound2-dev libpulse-dev libudev-dev libdbus-1-dev \
        libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev \
        libxinerama-dev libxss-dev libxfixes-dev libxkbcommon-dev \
        libwayland-dev wayland-protocols libdrm-dev libgbm-dev libegl1-mesa-dev libgl1-mesa-dev \
    && rm -rf /var/lib/apt/lists/* \
    && git config --global --add safe.directory '*'

# meson pinned, as the versions of everything else that builds dxvk are.
RUN pip3 install --no-cache-dir --break-system-packages "meson==${MESON_VERSION}"

RUN set -eux; \
    arch="$(uname -m)"; \
    case "$arch" in aarch64) sha="${CMAKE_SHA256_AARCH64}" ;; x86_64) sha="${CMAKE_SHA256_X86_64}" ;; *) exit 1 ;; esac; \
    wget -q "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-${arch}.tar.gz"; \
    echo "${sha}  cmake-${CMAKE_VERSION}-linux-${arch}.tar.gz" | sha256sum -c -; \
    mkdir -p /opt/cmake; \
    tar -xzf "cmake-${CMAKE_VERSION}-linux-${arch}.tar.gz" -C /opt/cmake --strip-components=1; \
    rm "cmake-${CMAKE_VERSION}-linux-${arch}.tar.gz"
ENV PATH=/opt/cmake/bin:$PATH

# glslang: dxvk compiles its built-in shaders with it. Old ones (Ubuntu 22.04's 11.8) emit SPIR-V
# that current Mesa rejects, which shows up as a failed built-in compute pipeline at device
# creation, so this one is pinned too.
RUN set -eux; \
    cd /tmp; \
    wget -q "https://github.com/KhronosGroup/glslang/archive/refs/tags/${GLSLANG_VERSION}.tar.gz" -O glslang.tar.gz; \
    echo "${GLSLANG_SHA256}  glslang.tar.gz" | sha256sum -c -; \
    tar -xzf glslang.tar.gz; \
    cmake -S "glslang-${GLSLANG_VERSION}" -B build-glslang -GNinja -DCMAKE_BUILD_TYPE=Release \
        -DENABLE_OPT=OFF -DENABLE_GLSLANG_BINARIES=ON -DBUILD_TESTING=OFF -DBUILD_EXTERNAL=OFF; \
    cmake --build build-glslang --target install; \
    rm -rf /tmp/*

# Pinned Vulkan and SPIR-V headers, for vkd3d and for the game's OpenXR binding.
RUN set -eux; \
    cd /tmp; \
    wget -q "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/${KHRONOS_SDK}.tar.gz" -O vulkan-headers.tar.gz; \
    echo "${VULKAN_HEADERS_SHA256}  vulkan-headers.tar.gz" | sha256sum -c -; \
    wget -q "https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/${KHRONOS_SDK}.tar.gz" -O spirv-headers.tar.gz; \
    echo "${SPIRV_HEADERS_SHA256}  spirv-headers.tar.gz" | sha256sum -c -; \
    tar -xzf vulkan-headers.tar.gz; \
    tar -xzf spirv-headers.tar.gz; \
    cmake -S "Vulkan-Headers-${KHRONOS_SDK}" -B build-vkh -GNinja -DCMAKE_INSTALL_PREFIX=/usr/local -DVULKAN_HEADERS_ENABLE_MODULE=OFF; \
    cmake --build build-vkh --target install; \
    cmake -S "SPIRV-Headers-${KHRONOS_SDK}" -B build-spvh -GNinja -DCMAKE_INSTALL_PREFIX=/usr/local; \
    cmake --build build-spvh --target install; \
    rm -rf /tmp/*

# vkd3d, for its HLSL compiler: Direct3D's own compiler (d3dcompiler_47) is a Windows DLL, and the
# renderer compiles its shaders when it starts. Static and position independent, so the game links
# it in and the package carries no extra library. The patch makes SampleGrad on array textures
# take gradients without a component for the layer, as Direct3D's compiler does.
COPY vkd3d-0001-samplegrad-array-gradients.patch /tmp/
RUN set -eux; \
    cd /tmp; \
    wget -q "https://dl.winehq.org/vkd3d/source/vkd3d-${VKD3D_VERSION}.tar.xz"; \
    echo "${VKD3D_SHA256}  vkd3d-${VKD3D_VERSION}.tar.xz" | sha256sum -c -; \
    tar -xJf "vkd3d-${VKD3D_VERSION}.tar.xz"; \
    cd "vkd3d-${VKD3D_VERSION}"; \
    patch -p1 < /tmp/vkd3d-0001-samplegrad-array-gradients.patch; \
    ./configure --prefix=/opt/vkd3d --disable-shared --enable-static --with-pic \
        --disable-tests --disable-demos --without-ncurses --without-opengl --without-xcb --without-spirv-tools \
        CPPFLAGS=-I/usr/local/include CFLAGS="-O2 -fPIC"; \
    make -j"$(nproc)"; \
    make install; \
    rm -rf /tmp/*

# Host testing: a virtual X server and Mesa's software Vulkan driver (lavapipe), and ImageMagick
# for screenshots of it.
# libsdl2-dev is there for its headers only: dxvk-native's window system code compiles against
# them and looks SDL2 up at run time, where it finds the one the game was built with.
RUN apt-get update && apt-get install -y --no-install-recommends \
        xvfb xauth mesa-vulkan-drivers libsdl2-dev imagemagick \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
