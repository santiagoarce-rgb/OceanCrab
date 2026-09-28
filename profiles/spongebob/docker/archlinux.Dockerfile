FROM archlinux:base-devel

ARG PUID=1000
ARG PGID=1000

RUN pacman --sync --refresh --sysupgrade --noconfirm && \
	pacman --sync --refresh --noconfirm vulkan-devel \
					pipewire-jack \
					cmake \
					ninja \
					sdl2-compat \
					ccache && \
	groupadd --gid ${PGID} build && \
	useradd --create-home --uid ${PUID} --gid ${PGID} build

ENV PATH="/usr/lib/ccache/bin:$PATH"
ENV CMAKE_C_COMPILER_LAUNCHER="ccache"
ENV CMAKE_CXX_COMPILER_LAUNCHER="ccache"
ENV CCACHE_DIR="/home/build/.ccache"

COPY --chmod=755 entrypoint.sh /entrypoint.sh

USER build
WORKDIR /home/build
VOLUME /home/build

CMD ["/usr/bin/bash", "/entrypoint.sh"]
