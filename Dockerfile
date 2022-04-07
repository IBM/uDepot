FROM ubuntu:18.04

RUN apt update && apt upgrade -y --no-install-recommends &&\
    apt install -y --no-install-recommends build-essential default-jdk python libcunit1-dev libaio-dev libssl-dev \
    libboost-dev zlib1g-dev libgoogle-perftools-dev git python3-pip linux-headers-$(uname -r) &&\
    pip3 install --no-cache-dir --upgrade numpy &&\
    apt-get autoremove -y --no-install-recommends &&\
    apt-get clean all &&\
    rm -rf /var/lib/apt/lists/*
