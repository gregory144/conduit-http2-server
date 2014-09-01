# Overview

An interoperable HTTP2 server.


## Build from git

1. ./autogen.sh
1. ./configure CC=clang
1. ./make
1. ./src/conduit


## Dependencies

* libuv
* pthreads
* openssl libssl/libcrypto (1.0.2 for ALPN)
* zlib

## TODO

#### Required features

* logging categories (turn on/off tls logging, gzip logging, etc)
* listen on secure port + non-secure port at the same time
* handle 1.1 traffic
* upgrade from 1.1
* graceful shutdown
* error on violation of incoming max concurrent streams
* better goaway handling, goaway on bad hpack indexes
* better error handling for libuv calls
* doxygen documentation
* complete spec compliance

#### Up next

* output data frame padding config option
* runtime configuration from config file
* access/error log
* switch from heap based priority queue to linked list?

* finite state machine for handling stream status
* plugin system that allows access to connections, streams, frames, requests, responses (callbacks)

#### Performance:

* fast huffman coding + decoding - more than one bit at a time
* better hpack encoding algorithm - use indexing
* stream priority
* improve gzip compression to use deflateBound to predict size of output
