# An almost-RESTCONF server

![License](https://img.shields.io/github/license/cesnet/rousette)
[![Gerrit](https://img.shields.io/badge/patches-via%20Gerrit-blue)](https://gerrit.cesnet.cz/q/project:CzechLight/rousette)
[![Zuul CI](https://img.shields.io/badge/zuul-checked-blue)](https://zuul.gerrit.cesnet.cz/t/public/buildsets?project=CzechLight/rousette)

## Usage

One day, this might become a [RESTCONF](https://www.rfc-editor.org/rfc/rfc8040.html) server on top of [sysrepo](https://www.sysrepo.org/).
Before that happens, it will, hopefully, be a small HTTP wrapper around sysrepo which publishes some data in a RESTCONF format.

Since this service only talks cleartext HTTP/2, it's recommended to run it behind a reverse proxy.

### Access control model

Rousette implements [RFC 8341 (NACM)](https://www.rfc-editor.org/rfc/rfc8341).
The access rights for users (and groups) are configurable via `ietf-netconf-acm` YANG model.

The reverse proxy must pass the `authorization` header as-is and delegate authentication/authorization to the RESTCONF server.
The server currently supports two authentication/authorization methods:

- a systemwide PAM setup through the *Basic* HTTP authentication, i.e., via the `authorization` header, which is checked against the system's PAM configuration
- a special anonymous access.

When the request does not contain the `authorization` header, and anonymous access is enabled (see below), the server will perform extra safety checks.
When certain conditions are met, the anonymous access will be mapped to a NACM account named in the `ANONYMOUS_USER` CMake option.
Such user must be in group `ANONYMOUS_USER_GROUP` (CMake option) and there must be some specific access rights set up in `ietf-netconf-acm` model (these are currently very opinionated for our use-case):

1. The first entry of `rule-list` list must be configured for `ANONYMOUS_USER_GROUP`.
2. All the rules except the last one in this rule-list entry must enable only "read" access operation.
3. The last rule in the first rule-set must be a wildcard rule that disables all operations over all modules.

The anonymous user access is disabled whenever these rules are not met.

## Dependencies

- [nghttp2-asio](https://github.com/nghttp2/nghttp2-asio) - asynchronous C++ library for HTTP/2
- [sysrepo-cpp](https://github.com/sysrepo/sysrepo-cpp) - object-oriented bindings of the [*sysrepo*](https://github.com/sysrepo/sysrepo) library
- [libyang-cpp](https://github.com/CESNET/libyang-cpp) - C++ bindings for *libyang*
- systemd - the shared library for logging to `sd-journal`
- [PAM](http://www.linux-pam.org/) - for authentication
- [spdlog](https://github.com/gabime/spdlog) - Very fast, header-only/compiled, C++ logging library
- Boost's system and thread
- C++20 compiler (e.g., GCC 10.x+, clang 10+)
- CMake 3.19+
- optionally for built-in tests, [Doctest](https://github.com/onqtam/doctest/) as a C++ unit test framework
- optionally for built-in tests, [trompeloeil](https://github.com/rollbear/trompeloeil) for mock objects in C++
- optionally for built-in tests, [`pam_matrix` and `pam_wrapper`](https://cwrap.org/pam_wrapper.html) for PAM mocking

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
