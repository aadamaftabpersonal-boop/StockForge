# Production-style image for the C server: build stage compiles, runtime stage carries only the
# binary and its shared libraries, running as an unprivileged user.
FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends build-essential pkg-config libmariadb-dev libsodium-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile ./
COPY src ./src
COPY vendor ./vendor
COPY tools ./tools
RUN make build/rel/stockforged build/rel/reconcile

FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends libmariadb3 libsodium23 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --no-create-home --shell /usr/sbin/nologin stockforge
COPY --from=build /src/build/rel/stockforged /src/build/rel/reconcile /usr/local/bin/
USER stockforge
ENV SF_BIND=0.0.0.0 SF_PORT=8080
EXPOSE 8080
HEALTHCHECK --interval=5s --timeout=3s --retries=12 CMD ["bash", "-c", "exec 3<>/dev/tcp/127.0.0.1/8080 && printf 'GET /healthz HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n' >&3 && head -c 12 <&3 | grep -q '200'"]
ENTRYPOINT ["/usr/local/bin/stockforged"]
