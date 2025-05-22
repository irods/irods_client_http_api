# syntax=docker/dockerfile:1.5

FROM rockylinux/rockylinux:8

SHELL [ "/usr/bin/bash", "-c" ]

# Make sure we're starting with an up-to-date image
RUN --mount=type=cache,target=/var/cache/dnf,sharing=locked \
    --mount=type=cache,target=/var/cache/yum,sharing=locked \
    dnf update -y || [ "$?" -eq 100 ] && \
    rm -rf /tmp/*

RUN --mount=type=cache,target=/var/cache/dnf,sharing=locked \
    --mount=type=cache,target=/var/cache/yum,sharing=locked \
    dnf install -y \
        epel-release \
    && \
    rm -rf /tmp/*

RUN --mount=type=cache,target=/var/cache/dnf,sharing=locked \
    --mount=type=cache,target=/var/cache/yum,sharing=locked \
    dnf install -y \
        redhat-lsb-core \
        wget \
        rpm-build \
    && \
    rm -rf /tmp/*

RUN --mount=type=cache,target=/var/cache/dnf,sharing=locked \
    --mount=type=cache,target=/var/cache/yum,sharing=locked \
    dnf install -y \
        dnf-plugin-config-manager \
        dnf-plugins-core \
    && \
    rpm --import https://packages.irods.org/irods-signing-key.asc && \
    dnf config-manager -y --add-repo https://packages.irods.org/renci-irods.yum.repo && \
    dnf config-manager -y --set-enabled renci-irods && \
    dnf config-manager -y --set-enabled powertools && \
    rm -rf /tmp/*

ARG irods_version=4.3.2-0.el8
RUN --mount=type=cache,target=/var/cache/dnf,sharing=locked \
    --mount=type=cache,target=/var/cache/yum,sharing=locked \
    dnf install -y \
        irods-devel-${irods_version} \
        irods-runtime-${irods_version} \
        irods-externals-clang13.0.1-0 \
        irods-externals-cmake3.21.4-0 \
        irods-externals-fmt-libcxx8.1.1-1 \
        irods-externals-json3.10.4-0 \
        irods-externals-jwt-cpp0.6.99.1-0 \
        irods-externals-nanodbc-libcxx2.13.0-2 \
        irods-externals-spdlog-libcxx1.9.2-2 \
        libcurl-devel \
        openssl-devel \
        ninja-build \
    && \
    rm -rf /tmp/*

ARG cmake_path="/opt/irods-externals/cmake3.21.4-0/bin"
ENV PATH=${cmake_path}:$PATH

COPY --chmod=755 build_packages.sh /
ENTRYPOINT ["/build_packages.sh"]
