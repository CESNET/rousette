# An almost-RESTCONF server

One day, this might become a RESTCONF server on top of [sysrepo](https://www.sysrepo.org/).
Before that happens, it will, hopefully, be a small HTTP wrapper around sysrepo which publishes some data in a RESTCONF format.

# Security

Rousette is designed to be reachable ONLY via a reverse proxy. Rousette does NOT handle any kind of authentication.
There is an nftables configuration files, which makes it impossible for a non-root user to connect to it and bypass the
reverse proxy. Not using any kind of reverse proxy means serious security implications.
