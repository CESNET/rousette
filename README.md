# An almost-RESTCONF server
![License](https://img.shields.io/github/license/cesnet/rousette)

[![Gerrit](https://img.shields.io/badge/patches-via%20Gerrit-blue)](https://gerrit.cesnet.cz/q/project:CzechLight/rousette)

[![Zuul CI](https://img.shields.io/badge/zuul-checked-blue)](https://zuul.gerrit.cesnet.cz/t/public/buildsets?project=CzechLight/rousette)

One day, this might become a RESTCONF server on top of [sysrepo](https://www.sysrepo.org/).
Before that happens, it will, hopefully, be a small HTTP wrapper around sysrepo which publishes some data in a RESTCONF format.

### Build steps:
##### Create directory for build
```
mkdir testbrousette
cd testbrousette
```
##### Get and build nghttp2 and asio lib
```
wget https://github.com/nghttp2/nghttp2/releases/download/v1.41.0/nghttp2-1.41.0.tar.xz
tar xvf nghttp2-1.41.0.tar.xz
cd nghttp2-1.41.0
mkdir build && cd build
cmake .. -DENABLE_ASIO_LIB=ON && make && make install
```
##### Get and build dependencies of rousette
```
cd ../../
git clone "https://gerrit.cesnet.cz/CzechLight/dependencies"
cd dependencies
git submodule update --init --recursive
```
##### Build sysrepo
```
cd sysrepo
cmake .
make && make install
```
##### Build sysrepo-cpp
```
cd ../sysrepo-cpp
cmake .
make && make install
```
##### Build libyang-cpp
```
cd ../libyang-cpp
cmake .
make && make install
```
##### Install systemd-devel

##### Get and build rousette
```
cd ../../
git clone https://gerrit.cesnet.cz/CzechLight/rousette
cd rousette
cmake .
make
```
