# An almost-RESTCONF server

![License](https://img.shields.io/github/license/cesnet/rousette)
[![Gerrit](https://img.shields.io/badge/patches-via%20Gerrit-blue)](https://gerrit.cesnet.cz/q/project:CzechLight/rousette)
[![Zuul CI](https://img.shields.io/badge/zuul-checked-blue)](https://zuul.gerrit.cesnet.cz/t/public/buildsets?project=CzechLight/rousette)

## Usage

One day, this might become a [RESTCONF](https://www.rfc-editor.org/rfc/rfc8040.html) server on top of [sysrepo](https://www.sysrepo.org/).
Before that happens, it will, hopefully, be a small HTTP wrapper around sysrepo which publishes some data in a RESTCONF format.

### Access control model

Rousette implements [RFC 8341 (NACM)](https://www.rfc-editor.org/rfc/rfc8341).
The server is supposed to be run behind a proxy that performs authentication and all the requests to this server should contain HTTP/2 header `x-remote-user` containing the username.
The access rights for users (and groups) are configurable via `ietf-netconf-acm` YANG model.

Furthermore, the server currently implements read access users..
The anonymous access is implemented as a special user (`x-remote-user` is set to the value of CMake's `ANONYMOUS_USER` option).
Such user must be in group `ANONYMOUS_GROUP` (CMake option) and there must be some specific access right set up in `ietf-netconf-acm` model (these are currently very opinionated for our use-case).

1. The first entry of `rule-list` list must be configured for `ANONYMOUS_GROUP`.
2. All the rules except the last one in this rule-list entry must enable only "read" access operation.
3. The last rule in the first rule-set must be a wildcard rule that disables all operations over all modules.

The anonymous user access is disabled whenever these rules are not met.

## Dependencies

- [nghttp2-asio](https://github.com/nghttp2/nghttp2-asio) - asynchronous C++ library for HTTP/2
- [sysrepo-cpp](https://github.com/sysrepo/sysrepo-cpp) - object-oriented bindings of the [*sysrepo*](https://github.com/sysrepo/sysrepo) library
- [libyang-cpp](https://github.com/CESNET/libyang-cpp) - C++ bindings for *libyang*
- systemd - the shared library for logging to `sd-journal`
- [spdlog](https://github.com/gabime/spdlog) - Very fast, header-only/compiled, C++ logging library
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

The development is being done on Gerrit [here](https://gerrit.cesnet.cz/q/project:CzechLight/rousette).
Instructions on how to submit patches can be found [here](https://gerrit.cesnet.cz/Documentation/intro-gerrit-walkthrough-github.html).
GitHub Pull Requests are not used.
