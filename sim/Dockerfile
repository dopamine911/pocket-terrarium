# Build from the repository root: docker build -f sim/Dockerfile -t pocket-terrarium-sim .
FROM debian:trixie-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc make libc6-dev git ca-certificates libsdl2-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
# LVGL is normally a local, ignored checkout; fetch the same pinned version in a clean build.
RUN git clone --depth 1 --branch v9.2.2 https://github.com/lvgl/lvgl.git sim/lvgl \
    && test "$(git -C sim/lvgl rev-parse HEAD)" = 7f07a129e8d77f4984fff8e623fd5be18ff42e74 \
    && rm -rf sim/lvgl/.git
COPY common/ common/
COPY sim/*.c sim/*.h sim/Makefile sim/
ARG WORLD=terrarium
ARG BOARD=rect
ARG JOBS=4
RUN case "$BOARD" in \
      rect) flag=; bin=fishsim ;; \
      round) flag=ROUND=1; bin=fishsim-round ;; \
      watch) flag=WATCH=1; bin=fishsim-watch ;; \
      square) flag=SQUARE=1; bin=fishsim-square ;; \
      *) echo "BOARD must be rect, round, watch or square" >&2; exit 2 ;; \
    esac \
    && case "$WORLD" in terrarium) ;; aquarium) bin="$bin-aquarium" ;; *) exit 2 ;; esac \
    && make -C sim -j"$JOBS" WORLD="$WORLD" $flag \
    && install -D "sim/$bin" /out/fishsim

FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends libsdl2-2.0-0 \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /home/sim/.cache/pocket-terrarium \
    && chown -R 1000:1000 /home/sim
WORKDIR /opt/pocket/sim
COPY --from=build /out/fishsim ./fishsim
COPY model/out/model_q4.bin model/out/tokenizer.bin model/out/model_q4_lizard.bin model/out/tokenizer_lizard.bin /opt/pocket/model/out/
COPY assets/sounds/sounds.bin /opt/pocket/assets/sounds/sounds.bin
COPY LICENSE NOTICE /opt/pocket/
ENV HOME=/home/sim USER=sim LOGNAME=sim SDL_RENDER_DRIVER=software
USER 1000:1000
ENTRYPOINT ["stdbuf", "-oL", "-eL", "./fishsim"]
CMD ["--zoom", "2"]
