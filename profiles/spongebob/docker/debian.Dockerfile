ARG DEBIAN_VERSION=13
FROM debian:${DEBIAN_VERSION}

ARG PUID=1000
ARG PGID=1000

ENV DEBIAN_FRONTEND="noninteractive"

RUN apt-get update && \
	apt-get full-upgrade --yes && \
	apt-get install --yes build-essential \
				ccache \
				cmake \
				glslang-tools \
				libsdl2-compat-dev \
				libvulkan-dev \
				ninja-build \
				spirv-tools \
				vulkan-tools \
				vulkan-validationlayers  && \
	groupadd --gid ${PGID} build && \
	useradd --create-home --uid ${PUID} --gid ${PGID} build

ENV PATH="/usr/lib/ccache:$PATH"
ENV CMAKE_C_COMPILER_LAUNCHER="ccache"
ENV CMAKE_CXX_COMPILER_LAUNCHER="ccache"
ENV CCACHE_DIR="/home/build/.ccache"

COPY --chmod=755 entrypoint.sh /entrypoint.sh

USER build
WORKDIR /home/build
VOLUME /home/build

CMD ["/usr/bin/bash", "/entrypoint.sh"]
