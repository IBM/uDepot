FROM ubuntu:18.04

RUN apt update && apt upgrade -y --no-install-recommends &&\
    apt install -y --no-install-recommends build-essential default-jdk python libcunit1-dev libaio-dev libssl-dev \
    libboost-dev zlib1g-dev libgoogle-perftools-dev git python3-pip &&\
    pip3 install --no-cache-dir --upgrade numpy &&\
    apt-get autoremove -y --no-install-recommends &&\
    apt-get clean all &&\
    rm -rf /var/lib/apt/lists/*

RUN git config --global --add safe.directory /udepot/trt/external/dpdk
RUN git config --global --add safe.directory /udepot/trt/external/spdk
