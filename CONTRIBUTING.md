## Contributing In General

Our project welcomes external contributions. To contribute code or documentation, please submit a [pull request](https://github.com/ibm/smlp/pulls).

A good way to familiarize yourself with the codebase and contribution process is
to look for and tackle low-hanging fruit in the [issue tracker](https://github.com/ibm/smlp/issues).
Before embarking on a more ambitious contribution, please quickly get in touch with us.

**Note: We appreciate your effort, and want to avoid a situation where a contribution
requires extensive rework (by you or by us), sits in backlog for a long time, or
cannot be accepted at all!**

### Proposing new features

If you would like to implement a new feature, please [raise an issue](https://github.com/ibm/smlp/issues)
before sending a pull request so the feature can be discussed. This is to avoid
you wasting your valuable time working on a feature that the project developers
are not interested in accepting into the code base.

### Fixing bugs

If you would like to fix a bug, please [raise an issue](https://github.com/ibm/smlp/issues) before sending a
pull request so it can be tracked.

### Merge approval

The project maintainers use LGTM (Looks Good To Me) in comments on the code
review to indicate acceptance. A change requires LGTMs from one of the maintainers.

For a list of the maintainers, see the [MAINTAINERS.md](MAINTAINERS.md) page.

## Legal

Each source file must include a license header for the BSD 3-Clause
"New" or "Revised" License. Using the SPDX format is the simplest
approach.  e.g.

```
'''
Copyright <holder> All Rights Reserved.

SPDX-License-Identifier: BSD-3-Clause
'''
```

We have tried to make it as easy as possible to make contributions. This
applies to how we handle the legal aspects of contribution. We use the
same approach - the [Developer's Certificate of Origin 1.1 (DCO)](https://github.com/hyperledger/fabric/blob/master/docs/source/DCO1.1.txt) - that the Linux® Kernel [community](https://elinux.org/Developer_Certificate_Of_Origin)
uses to manage code contributions.

We simply ask that when submitting a patch for review, the developer
must include a sign-off statement in the commit message.

Here is an example Signed-off-by line, which indicates that the
submitter accepts the DCO:

```
Signed-off-by: John Doe <john.doe@example.com>
```

You can include this automatically when you commit a change to your
local git repository using the following command:

```
git commit -s
```

## Communication
Please feel free to connect with us over email.

## Setup
See the [README](README.md) and the [travis](.travis.yml) commands for a quick example of setting up a working environment.

## Testing
The travis unit test must complete before any pull request can be accepted, and this is automatically enforced.
TODO: Add performance regression tests.
