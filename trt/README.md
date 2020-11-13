trt: a task runtime system.
---------------------------

TRT is a run-time system for tasks.

A task is small unit of excution, essentially an execution stack. Contrarily to
threads, tasks are not scheduled by the OS. Task have been used as a  basis for
building systems that exploit parallelism (e.g., see Cilk), and are also a good
match for doing asynchronous I/O.

TRT is a task run-time for supporting both. It allows to spawn tasks to
parallelize work, issue asynchronous I/O operations, and  defer task execution
until a condition (such as the result of an I/O operation being available) is
meet.

Its goals are:

 1. Efficiency
 1. Support resource management, different scheduling policies
 1. Use high-performance IO systems such as: native linux aio, spdk, dpdk, etc.

Dependencies
------------
 - c++11
 - uses boost's intrusive lists

Design Notes
------------

Each core runs a scheduler that schedule tasks. Each scheduler starts with a
main task.  There is a distinction between normal tasks and poller tasks, with
the latter being tasks that poll I/O devices (e.g., network). This distinction
is important only for scheduling purposes. Scheduling poller tasks leads to new
tasks by checking the network for incoming requests, while scheduling normal
tasks leads to completion of existing tasks in the system. This allows the
scheduler some control over the number of in-flight tasks.  This is an
oversimplification, and does not apply, for example,  in the case where running
tasks might their own network requests and expect network replies. We leave
things simple for now; more complex policies will be considered when the need
arises.

Task migration requires synchronization and breaks locality, but allows
countering imbalance. There are a range of different options such as work
stealing, work sharing, allow tasks to be migrated only at creation time and
have them executed on a single core.  Currently, there is no migration policy
implemented, but we assume that at any of the execution points, the tasks might
be scheduled on a different core.

Trt provides a future interface. Futures subscribe to asynchronous objects. An
asynchronous object represents a pending operation. Future users may wait on one
or more futures via a waitset.

A representetive application running on top of trt is a key-value store. It
receives requests from the network and (potentially) does storage I/O.

A task will go through these steps:

 1. created by the network poller
 1. execute
 1. issue and wait on storage I/O
 1. execute
 1. send back a reply over the network

We can use different data structures to build the cache, such as hashes, trees,
etc.  Let's say that we use a hash. In traditional hash tables there is a choice
between open adressing and seperate chaining. Because the hash is a cache, we can
also discard entries. Let's call the entries we can discard _clean_.

If a task requests a key mapped in an empty entry, an asynchronous object is
generated and placed in the entry. The task gets a future to wait on the
asynchronous object.  If a second task requests the same key it will get another
future on the same asynchronous object.  If another task requests a different
key that collides with the first, we can use seperate chaining or open
addressing to deal with this, but eventually, we will have to discard values
from our cache.

Different networking options:

 - DPDK
 - UDP
 - TCP
 - Websockets


Choices
-------

Here, I'm enumerating some (mostly minor) choices made during the design and
implementation of trt.

1. Decided against using boost::context and/or boost::fibers
   (https://github.com/olk/boost-fiber), to avoid having a dependenct on boost
   and external librearies (fibers are not yet included in boost proper), and to
   be able to ignore things that we don't care about in this prototype such as
   exceptions, consistent interface to std::{future,promise}, multi-arch, etc.
   Eventually it would be nice to build on top of boost fibers, however.

1. There are also
   [folly::fibers](https://github.com/facebook/folly/tree/master/folly/fibers),
   which have very similar goals.

1. Another similar project is ScyllaDB's
   [seastar](https://github.com/scylladb/seastar). It supprots blocking code
   using a similar approach to trt's:
   https://github.com/scylladb/seastar/blob/master/core/thread.hh

1. When we want to send a reply, can we do it from the core the task is
   currently executed, or do we need to route it back? We assume that we can,
   for simplicity.  (E.g., socket operations [are
   thread-safe](http://pubs.opengroup.org/onlinepubs/9699919799/functions/V2_chap02.html#tag_15_09_01),
   so we can use the same socket form mutliple threads.)

   On the other hand, an alternative model where the net task creates tasks and
   then waits for them to return replies also seems sensible. In this case, the
   creation of a task can be modeled as a future. Then the net task needs to
   check whether some of the futures corresponding to tasks have available
   values. select(). Waitsets.

1.  Did not implement split stacks. Initially I wanted to support split stacks
    and, it didn't look too diffult (see `libcc/generic-morestack.c`,
    `gcc/testsuite/gcc.dg/split-5.c`). However, after reading about other languages
    abanding them I decided it's not worth it.

    Walter Bright (from the D language) had [a great
    point](http://forum.dlang.org/post/jtcn0a$2sft$1@digitalmars.com):

        > Also, they do not save RAM, they save address space. RAM is not committed
        > until a stack memory page is actually used.


    [Some](https://mail.mozilla.org/pipermail/rust-dev/2013-November/006314.html)
    [more](https://gcc.gnu.org/ml/gcc/2015-09/msg00176.html)
    [links](https://gcc.gnu.org/ml/gcc/2015-09/msg00177.html).
