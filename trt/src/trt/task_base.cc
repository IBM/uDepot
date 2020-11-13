/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

// vim: set expandtab softtabstop=4 tabstop=4 shiftwidth=4:

#include "trt/task_base.hh"
#include "trt/scheduler.hh"

namespace trt {

TaskBase::TaskBase(TaskBase *parent, bool t_detached, TaskType type)
 : t_type_(type)
 , t_last_scheduler(nullptr)
 , t_parent_(parent)
 , t_detached_(parent == nullptr || t_detached)
 #if !defined(NDEBUG)
 , t_dbg_id_(dbg_task_id())
 #endif
 { }


TaskBase::~TaskBase() {}

} // end namespace
