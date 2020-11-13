/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

// vim: set expandtab softtabstop=4 tabstop:4 shiftwidth:4:

#if defined(TRT_CONFIG_SDT)
#include <sys/sdt.h>
#else
#define DTRACE_PROBE(provider,probe)        \
    do {} while (0)
#define DTRACE_PROBE1(provider,probe,parm1) \
    do {} while (0)
#define DTRACE_PROBE2(provider,probe,parm1,parm2)   \
    do {} while (0)
#define DTRACE_PROBE3(provider,probe,parm1,parm2,parm3) \
    do {} while (0)
#define DTRACE_PROBE4(provider,probe,parm1,parm2,parm3,parm4)   \
    do {} while (0)
#define DTRACE_PROBE5(provider,probe,parm1,parm2,parm3,parm4,parm5) \
    do {} while (0)
#define DTRACE_PROBE6(provider,probe,parm1,parm2,parm3,parm4,parm5,parm6) \
    do {} while (0)
#define DTRACE_PROBE7(provider,probe,parm1,parm2,parm3,parm4,parm5,parm6,parm7) \
    do {} while (0)
#define DTRACE_PROBE8(provider,probe,parm1,parm2,parm3,parm4,parm5,parm6,parm7,parm8) \
    do {} while (0)
#define DTRACE_PROBE9(provider,probe,parm1,parm2,parm3,parm4,parm5,parm6,parm7,parm8,parm9) \
    do {} while (0)
#define DTRACE_PROBE10(provider,probe,parm1,parm2,parm3,parm4,parm5,parm6,parm7,parm8,parm9,parm10) \
    do {} while (0)
#define DTRACE_PROBE11(provider,probe,parm1,parm2,parm3,parm4,parm5,parm6,parm7,parm8,parm9,parm10,parm11) \
    do {} while (0)
#define DTRACE_PROBE12(provider,probe,parm1,parm2,parm3,parm4,parm5,parm6,parm7,parm8,parm9,parm10,parm11,parm12) \
    do {} while (0)
#endif
