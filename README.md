# An almost-RESTCONF server
![License](https://img.shields.io/github/license/cesnet/rousette)
[![Gerrit](https://img.shields.io/badge/patches-via%20Gerrit-blue)](https://gerrit.cesnet.cz/q/project:CzechLight/rousette)
[![Zuul CI](https://img.shields.io/badge/zuul-checked-blue)](https://zuul.gerrit.cesnet.cz/t/public/buildsets?project=CzechLight/rousette)

## Dependencies
- [nghttp2](https://github.com/nghttp2/nghttp2) with enabled Asio library
- [sysrepo](https://github.com/sysrepo/sysrepo) - the `devel` branch (even for the `master` branch of *sysrepo-cpp*)
- [sysrepo-cpp](https://github.com/sysrepo/sysrepo-cpp) - object-oriented bindings of the [*sysrepo*](https://github.com/sysrepo/sysrepo) library.
- [libyang-cpp](https://github.com/CESNET/libyang-cpp) - C++ bindings for *libyang*
- [systemd] library
- C++20 compiler (e.g., GCC 10.x+, clang 10+)
- CMake 3.19+
- optionally for built-in tests, [trompeloeil](https://github.com/rollbear/trompeloeil) for mock objects in C++

## Building
*nghttp2*, *sysrepo*, *sysrepo-cpp* and *libyang-cpp* use *CMake* for building. 
The standard way of building *sysrepo-cpp* looks like this:
```
mkdir build
cd build
cmake ..
make
make install
```
## Usage
One day, this might become a RESTCONF server on top of [sysrepo](https://www.sysrepo.org/).
Before that happens, it will, hopefully, be a small HTTP wrapper around sysrepo which publishes some data in a RESTCONF format.

## Contributing
The development is being done on Gerrit [here](https://gerrit.cesnet.cz/q/project:CzechLight/rousette). Instructions 
on how to submit patches can be found
[here](https://gerrit.cesnet.cz/Documentation/intro-gerrit-walkthrough-github.html). GitHub Pull Requests are not used.
