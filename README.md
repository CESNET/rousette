# An almost-RESTCONF server
![License](https://img.shields.io/github/license/cesnet/rousette)
[![Gerrit](https://img.shields.io/badge/patches-via%20Gerrit-blue)](https://gerrit.cesnet.cz/q/project:CzechLight/rousette)
[![Zuul CI](https://img.shields.io/badge/zuul-checked-blue)](https://zuul.gerrit.cesnet.cz/t/public/buildsets?project=CzechLight/rousette)
## Usage
One day, this might become a RESTCONF server on top of [sysrepo](https://www.sysrepo.org/).
Before that happens, it will, hopefully, be a small HTTP wrapper around sysrepo which publishes some data in a RESTCONF format.
## Dependencies
- [nghttp2](https://github.com/nghttp2/nghttp2) with enabled Asio library
Do not use the [standalone version of nghttp2-asio](https://github.com/nghttp2/nghttp2-asio).
- [sysrepo-cpp](https://github.com/sysrepo/sysrepo-cpp) - object-oriented bindings of the [*sysrepo*](https://github.com/sysrepo/sysrepo) library.
- [libyang-cpp](https://github.com/CESNET/libyang-cpp) - C++ bindings for *libyang*
- systemd library
- spdlog
- Boost's system and thread
- C++20 compiler (e.g., GCC 10.x+, clang 10+)
- CMake 3.19+
- optionally for built-in tests, [Doctest](https://github.com/onqtam/doctest/) as a C++ unit test framework
- optionally for built-in tests, [trompeloeil](https://github.com/rollbear/trompeloeil) for mock objects in C++
## Building
The standard way of building *rousette* looks like this:
```
mkdir build
cd build
cmake ..
make
make install
```
## Contributing
The development is being done on Gerrit [here](https://gerrit.cesnet.cz/q/project:CzechLight/rousette). Instructions 
on how to submit patches can be found [here](https://gerrit.cesnet.cz/Documentation/intro-gerrit-walkthrough-github.html). GitHub Pull Requests are not used.
