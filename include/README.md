# Third-party Single-header Libraries

This directory is expected to contain the single-header dependencies used by the service:

- `httplib.h` from [cpp-httplib](https://github.com/yhirose/cpp-httplib)
- `json.hpp` from [nlohmann/json](https://github.com/nlohmann/json)

The provided `Dockerfile` downloads the exact versions required during the image build step. If you plan to build the project locally without Docker, download the same headers manually and place them in this directory.

