FROM ubuntu:18.04

ADD libcityhash.so.0 /lib/x86_64-linux-gnu/
ADD udepot-memcache-server /udepot/
ADD start-udepot.sh /

RUN mkdir /udepot-data
VOLUME ["/udepot-data"]

ENTRYPOINT ["/start-udepot.sh"]
