//
// Copyright (C) 2001 - 2010, 2012, 2013 Rob Caelers <robc@krandor.nl>
// Copyright (C) 2007 Ray Satiro <raysatiro@yahoo.com>
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "ActivityMonitor.hh"
#include "ActivityMonitorListener.hh"

#include "debug.hh"
#include "timeutil.h"
#include <cassert>
#include <cmath>

#include <cstdio>
#include <sys/types.h>
#if STDC_HEADERS
#  include <cstddef>
#  include <cstdlib>
#else
#  if HAVE_STDLIB_H
#    include <stdlib.h>
#  endif
#endif
#if HAVE_UNISTD_H
#  include <unistd.h>
#endif

#include "IInputMonitor.hh"
#include "InputMonitorFactory.hh"

using namespace std;

//! Constructor.
ActivityMonitor::ActivityMonitor()

{
  TRACE_ENTER("ActivityMonitor::ActivityMonitor");

  noise_threshold = 1 * G_USEC_PER_SEC;
  activity_threshold = 2 * G_USEC_PER_SEC;
  idle_threshold = 5 * G_USEC_PER_SEC;

  input_monitor = InputMonitorFactory::get_monitor(IInputMonitorFactory::CAPABILITY_ACTIVITY);
  if (input_monitor != nullptr)
    {
      input_monitor->subscribe_activity(this);
    }

  TRACE_EXIT();
}

//! Destructor.
ActivityMonitor::~ActivityMonitor()
{
  TRACE_ENTER("ActivityMonitor::~ActivityMonitor");

  delete input_monitor;

  TRACE_EXIT();
}

//! Terminates the monitor.
void
ActivityMonitor::terminate()
{
  TRACE_ENTER("ActivityMonitor::terminate");

  if (input_monitor != nullptr)
    {
      input_monitor->terminate();
    }

  TRACE_EXIT();
}

//! Suspends the activity monitoring.
void
ActivityMonitor::suspend()
{
  TRACE_ENTER_MSG("ActivityMonitor::suspend", activity_state);
  lock.lock();
  activity_state = ACTIVITY_SUSPENDED;
  lock.unlock();
  activity_state.publish();
  TRACE_RETURN(activity_state);
}

//! Resumes the activity monitoring.
void
ActivityMonitor::resume()
{
  TRACE_ENTER_MSG("ActivityMonitor::resume", activity_state);
  lock.lock();
  activity_state = ACTIVITY_IDLE;
  lock.unlock();
  activity_state.publish();
  TRACE_RETURN(activity_state);
}

//! Forces state te be idle.
void
ActivityMonitor::force_idle()
{
  TRACE_ENTER_MSG("ActivityMonitor::force_idle", activity_state);
  lock.lock();
  if (activity_state != ACTIVITY_SUSPENDED)
    {
      activity_state = ACTIVITY_IDLE;
      last_action_time = 0;
    }
  lock.unlock();
  activity_state.publish();
  TRACE_RETURN(activity_state);
}

//! Returns the current state
ActivityState
ActivityMonitor::get_current_state()
{
  TRACE_ENTER_MSG("ActivityMonitor::get_current_state", activity_state);
  lock.lock();

  // First update the state...
  if (activity_state == ACTIVITY_ACTIVE)
    {
      gint64 now = g_get_real_time();
      gint64 tv = now - last_action_time;

      TRACE_MSG("Active: " << (tv / G_USEC_PER_SEC) << "." << tv << " " << (idle_threshold / G_USEC_PER_SEC) << " "
                           << idle_threshold);
      if (tv > idle_threshold)
        {
          // No longer active.
          activity_state = ACTIVITY_IDLE;
        }
    }

  lock.unlock();
  activity_state.publish();
  TRACE_RETURN(activity_state);
  return activity_state;
}

//! Sets the operation parameters.
void
ActivityMonitor::set_parameters(int noise, int activity, int idle, int sensitivity)
{
  noise_threshold = noise * 1000;
  activity_threshold = activity * 1000;
  idle_threshold = idle * 1000;

  this->sensitivity = sensitivity;

  // The easy way out.
  activity_state = ACTIVITY_IDLE;
}

//! Sets the operation parameters.
void
ActivityMonitor::get_parameters(int &noise, int &activity, int &idle, int &sensitivity)
{
  noise = noise_threshold / 1000;
  activity = activity_threshold / 1000;
  idle = idle_threshold / 1000;
  sensitivity = this->sensitivity;
}

//! Shifts the internal time (after system clock has been set)
void
ActivityMonitor::shift_time(int delta)
{
  gint64 d = delta * G_USEC_PER_SEC;

  Diagnostics::instance().log("activity_monitor: shift");
  lock.lock();

  if (last_action_time != 0)
    last_action_time += d;

  if (first_action_time != 0)
    first_action_time += d;

  lock.unlock();
}

//! Sets the callback listener.
void
ActivityMonitor::set_listener(ActivityMonitorListener *l)
{
  lock.lock();
  listener = l;
  lock.unlock();
}

//! Activity is reported by the input monitor.
void
ActivityMonitor::action_notify()
{
  lock.lock();

  gint64 now = g_get_real_time();

  switch (activity_state)
    {
    case ACTIVITY_IDLE:
      {
        first_action_time = now;
        last_action_time = now;

        if (activity_threshold == 0)
          {
            activity_state = ACTIVITY_ACTIVE;
          }
        else
          {
            activity_state = ACTIVITY_NOISE;
          }
      }
      break;

    case ACTIVITY_NOISE:
      {
        gint64 tv = now - last_action_time;
        if (tv > noise_threshold)
          {
            first_action_time = now;
          }
        else
          {
            tv = now - first_action_time;
            if (tv >= activity_threshold)
              {
                activity_state = ACTIVITY_ACTIVE;
              }
          }
      }
      break;

    default:
      break;
    }

  last_action_time = now;
  lock.unlock();
  call_listener();
}

//! Mouse activity is reported by the input monitor.
void
ActivityMonitor::mouse_notify(int x, int y, int wheel_delta)
{
  lock.lock();
  const int delta_x = x - prev_x;
  const int delta_y = y - prev_y;
  prev_x = x;
  prev_y = y;

  if (abs(delta_x) >= sensitivity || abs(delta_y) >= sensitivity || wheel_delta != 0 || button_is_pressed)
    {
      action_notify();
    }
  lock.unlock();
}

//! Mouse button activity is reported by the input monitor.
void
ActivityMonitor::button_notify(bool is_press)
{
  lock.lock();

  button_is_pressed = is_press;

  if (is_press)
    {
      action_notify();
    }

  lock.unlock();
}

//! Keyboard activity is reported by the input monitor.
void
ActivityMonitor::keyboard_notify(bool repeat)
{
  (void)repeat;

  lock.lock();
  action_notify();
  lock.unlock();
}

//! Calls the callback listener.
void
ActivityMonitor::call_listener()
{
  ActivityMonitorListener *l = nullptr;

  lock.lock();
  l = listener;
  lock.unlock();

  if (l != nullptr)
    {
      // Listener is set.
      if (!l->action_notify())
        {
          // Remove listener.
          lock.lock();
          listener = nullptr;
          lock.unlock();
        }
    }
}