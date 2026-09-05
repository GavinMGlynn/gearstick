# The gearstick central server, built from this tree. See deploy/README.md.
#
#   docker build -f deploy/Dockerfile -t gearstick-server .
#   docker run -d -p 47800:47800/udp -v gearstick-store:/var/lib/gearstick gearstick-server
#
# Two stages: a build with the compiler and the pinned submodules, and a run
# image with the one binary and the shipped library. The server links SDL
# for its sockets and nothing that needs a display.
FROM debian:bookworm AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git python3 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN git submodule update --init --depth 1 || true
RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGEARSTICK_SERVER=ON \
    && cmake --build build --target gearstick_server

FROM debian:bookworm-slim
RUN useradd --system --home /var/lib/gearstick gearstick \
    && mkdir -p /var/lib/gearstick && chown gearstick /var/lib/gearstick
COPY --from=build /src/build/gearstick_server /usr/local/bin/gearstick_server
COPY --from=build /src/assets /var/lib/gearstick/assets
USER gearstick
WORKDIR /var/lib/gearstick
VOLUME /var/lib/gearstick
EXPOSE 47800/udp
CMD ["gearstick_server", "--headless", "--port", "47800", "--players", "4", \
     "--store", "/var/lib/gearstick/gearstick.db"]
