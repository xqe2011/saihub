FROM ubuntu:24.04 AS dev

ARG TARGETARCH
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    cmake \
    curl \
    git \
    iproute2 \
    iputils-ping \
    libsqlite3-dev \
    ninja-build \
    pkg-config \
    python3 \
    tar \
    unzip \
    zip \
    && rm -rf /var/lib/apt/lists/*

ENV VCPKG_ROOT=/opt/vcpkg
RUN git clone --depth 1 https://github.com/microsoft/vcpkg.git ${VCPKG_ROOT} \
    && ${VCPKG_ROOT}/bootstrap-vcpkg.sh -disableMetrics

WORKDIR /src/cloud/cpp
COPY vcpkg.json ./
RUN case "${TARGETARCH}" in \
      amd64) TRIPLET="x64-linux" ;; \
      arm64) TRIPLET="arm64-linux" ;; \
      *) echo "unsupported TARGETARCH: ${TARGETARCH}" >&2; exit 1 ;; \
    esac \
    && ${VCPKG_ROOT}/vcpkg install --triplet="${TRIPLET}"

COPY CMakeLists.txt ./
COPY src ./src
COPY tests ./tests
COPY bench ./bench

RUN case "${TARGETARCH}" in \
      amd64) TRIPLET="x64-linux" ;; \
      arm64) TRIPLET="arm64-linux" ;; \
      *) echo "unsupported TARGETARCH: ${TARGETARCH}" >&2; exit 1 ;; \
    esac \
    && cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake \
        -DVCPKG_TARGET_TRIPLET="${TRIPLET}" \
    && cmake --build build -j

# Prepare minimal runtime rootfs for the `run` stage
RUN mkdir -p /rootfs \
    && cp build/saihub-cloud /rootfs/saihub-cloud \
    && strip --strip-all /rootfs/saihub-cloud \
    && ldd build/saihub-cloud | awk '/=> \//{print $3} /^[[:space:]]*\//{print $1}' | xargs -I{} cp --parents -L {} /rootfs/

FROM scratch AS run
COPY --from=dev /rootfs /
EXPOSE 8787
ENTRYPOINT ["/saihub-cloud"]
