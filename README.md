Play With Seastar
=======

Introduction
------------

Play With Seastar was created for people who want to get started with Seastar
programming. Seastar is a framework for high-performance server applications on
modern hardware. Find more about it at http://www.seastar-project.org/ and
https://github.com/scylladb/seastar

seastar_tutorial.pdf is a guide for Seastar programming that may help you.
Original version of the document can be found here:
https://github.com/scylladb/seastar/blob/master/doc/tutorial.md

If you got stuck and cannot find an answer anywhere, feel free to contact other
Seastar developers at Seastar mailing list: seastar-dev@googlegroups.com

This repository contains a simple Seastar program that will asynchronously
sleep for one second and print hello world. Other example programs may be
included in the future.

Building Play With Seastar
--------------------

Installing required packages on Fedora 21 and later:
```
yum install gcc-c++ libaio-devel ragel hwloc-devel numactl-devel libpciaccess-devel cryptopp-devel xen-devel boost-devel libxml2-devel xfsprogs-devel gnutls-devel
```

Installing required packages on Ubuntu 14.04:
```
sudo apt-get install libaio-dev ragel libhwloc-dev libnuma-dev libpciaccess-dev libcrypto++-dev libboost-all-dev libxen-dev libxml2-dev xfslibs-dev
```

Build example program(s):
```
make
```

