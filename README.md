# Payout Module

The Payout Module acts as a thin JSON API layer with human readable messages
around the rather low-level binary based serial [SSP protocol][itl-ssp]
from Innovative Technology used for communication with the [SMART Hopper][itl-hw-hopper]
and [NV200 Banknote validator][itl-hw-validator] devices.

A prebuilt [Doxygen HTML help][payout-api] can be found in docs/doxygen.

- [Source](https://github.com/metalab-kassomat/kassomat-payout)
- Written in C, can be built with make
- Tested on ARM and x86
- Linux daemon
- [JSON API](https://github.com/metalab-kassomat/kassomat-payout/blob/master/docs/overview.md) (accessible via Redis)
- UI: none
- libs
  - [hiredis](https://github.com/redis/hiredis) (Redis client)
  - [libevent2](http://libevent.org) (Event Dispatching)
  - [libjansson](http://www.digip.org/jansson/) (JSON library)
  - [libuuid](https://sourceforge.net/projects/libuuid)
  - ITL example code (aka "vendor hardware library")

## Dependencies

* mandatory for build: `sudo apt install build-essential libhiredis-dev libevent-dev libjansson-dev uuid-dev` 

* runtime: `sudo apt install redis-server`

* development: `sudo apt install doxygen graphviz uuid-runtime valgrind redis-tools`

[read more ...](docs/overview.md)

[itl-ssp]: http://innovative-technology.com/product-files/ssp-manuals/smart-payout-ssp-manual.pdf
[itl-hw-hopper]: http://innovative-technology.com/products/products-main/210-smart-hopper
[itl-hw-validator]: http://innovative-technology.com/products/products-main/90-nv200
[payout-api]: https://metalab-kassomat.github.io/kassomat-payout/doxygen/index.html
