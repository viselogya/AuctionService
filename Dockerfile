FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        curl \
        libpq-dev \
        libpqxx-dev && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app

RUN mkdir -p include

# Fetch single-header dependencies
RUN curl -L https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.15.3/httplib.h -o include/httplib.h && \
    curl -L https://github.com/nlohmann/json/releases/download/v3.11.2/json.hpp -o include/json.hpp

COPY CMakeLists.txt ./
COPY src ./src

RUN mkdir -p build && \
    cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    cmake --build . --config Release

EXPOSE 8080

CMD ["./build/auction_service"]

