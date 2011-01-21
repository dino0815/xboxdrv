/*
**  Xbox360 USB Gamepad Userspace Driver
**  Copyright (C) 2011 Ingo Ruhnke <grumbel@gmx.de>
**
**  This program is free software: you can redistribute it and/or modify
**  it under the terms of the GNU General Public License as published by
**  the Free Software Foundation, either version 3 of the License, or
**  (at your option) any later version.
**
**  This program is distributed in the hope that it will be useful,
**  but WITHOUT ANY WARRANTY; without even the implied warranty of
**  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**  GNU General Public License for more details.
**
**  You should have received a copy of the GNU General Public License
**  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "xboxdrv_thread.hpp"

#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <errno.h>
#include <iostream>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <vector>
#include <assert.h>

#include "controller_config.hpp"
#include "helper.hpp"
#include "log.hpp"
#include "modifier.hpp"
#include "options.hpp"
#include "uinput.hpp"
#include "xbox_generic_controller.hpp"

#include "modifier/axismap_modifier.hpp"
#include "modifier/buttonmap_modifier.hpp"
#include "modifier/dpad_rotation_modifier.hpp"
#include "modifier/four_way_restrictor_modifier.hpp"
#include "modifier/square_axis_modifier.hpp"

extern bool global_exit_xboxdrv;

// FIXME: isolate problametic code to a separate file, instead of pragma
#pragma GCC diagnostic ignored "-Wold-style-cast"

XboxdrvThread::XboxdrvThread(uInput* uinput,
                             std::auto_ptr<XboxGenericController> controller,
                             const Options& opts) :
  m_thread(),
  m_controller(controller),
  m_config(),
  m_loop(true),
  m_oldmsg(),
  m_oldrealmsg(),
  m_child_exec(opts.exec),
  m_pid(-1),
  m_timeout(opts.timeout)
{
  memset(&m_oldmsg,     0, sizeof(m_oldmsg));
  memset(&m_oldrealmsg, 0, sizeof(m_oldrealmsg));

  if (uinput)
  {
    ControllerConfigPtr config(new ControllerConfig(*uinput));
    m_config.add_config(config);

    create_modifier(opts, &config->get_modifier());
  
    // introspection of the config
    std::cout << "Active Modifier:" << std::endl;
    for(std::vector<ModifierPtr>::iterator i = config->get_modifier().begin(); 
        i != config->get_modifier().end(); 
        ++i)
    {
      std::cout << (*i)->str() << std::endl;
    }
  }
}

XboxdrvThread::~XboxdrvThread()
{
  if (m_thread.get())
  {
    log_info << "waiting for thread to join: " << m_thread->get_id() << std::endl;
    stop_thread(); 
    log_info << "joined thread: " << m_thread->get_id() << std::endl;
  }
}

// FIXME: duplicate code
namespace {

void set_rumble(XboxGenericController* controller, int gain, uint8_t lhs, uint8_t rhs)
{
  lhs = std::min(lhs * gain / 255, 255);
  rhs = std::min(rhs * gain / 255, 255);
  
  //std::cout << (int)lhs << " " << (int)rhs << std::endl;

  controller->set_rumble(lhs, rhs);
}

} // namespace

void
XboxdrvThread::create_modifier(const Options& opts, std::vector<ModifierPtr>* modifier)
{
  if (!opts.controller.back().calibration_map.empty())
  {
    boost::shared_ptr<AxismapModifier> axismap(new AxismapModifier);

    for(std::map<XboxAxis, AxisFilterPtr>::const_iterator i = opts.controller.back().calibration_map.begin();
        i != opts.controller.back().calibration_map.end(); ++i)
    {
      axismap->add_filter(i->first, i->second); 
    }

    modifier->push_back(axismap);
  }

  if (opts.controller.back().deadzone)
  {
    boost::shared_ptr<AxismapModifier> axismap(new AxismapModifier);

    XboxAxis axes[] = { XBOX_AXIS_X1,
                        XBOX_AXIS_Y1,
                      
                        XBOX_AXIS_X2,
                        XBOX_AXIS_Y2 };

    for(size_t i = 0; i < sizeof(axes)/sizeof(XboxAxis); ++i)
    {
      axismap->add_filter(axes[i],
                          AxisFilterPtr(new DeadzoneAxisFilter(-opts.controller.back().deadzone,
                                                               opts.controller.back().deadzone,
                                                               true)));
    }

    modifier->push_back(axismap);
  }

  if (opts.controller.back().deadzone_trigger)
  {
    boost::shared_ptr<AxismapModifier> axismap(new AxismapModifier);

    XboxAxis axes[] = { XBOX_AXIS_LT,
                        XBOX_AXIS_RT };

    for(size_t i = 0; i < sizeof(axes)/sizeof(XboxAxis); ++i)
    {
      axismap->add_filter(axes[i],
                          AxisFilterPtr(new DeadzoneAxisFilter(-opts.controller.back().deadzone_trigger,
                                                               opts.controller.back().deadzone_trigger,
                                                               true)));
    }

    modifier->push_back(axismap);
  }

  if (opts.controller.back().square_axis)
  {
    modifier->push_back(ModifierPtr(new SquareAxisModifier(XBOX_AXIS_X1, XBOX_AXIS_Y1)));
    modifier->push_back(ModifierPtr(new SquareAxisModifier(XBOX_AXIS_X2, XBOX_AXIS_Y2)));
  }

  if (!opts.controller.back().sensitivity_map.empty())
  {
    boost::shared_ptr<AxismapModifier> axismap(new AxismapModifier);

    for(std::map<XboxAxis, AxisFilterPtr>::const_iterator i = opts.controller.back().sensitivity_map.begin();
        i != opts.controller.back().sensitivity_map.end(); ++i)
    {
      axismap->add_filter(i->first, i->second); 
    }

    modifier->push_back(axismap);
  }

  if (opts.controller.back().four_way_restrictor)
  {
    modifier->push_back(ModifierPtr(new FourWayRestrictorModifier(XBOX_AXIS_X1, XBOX_AXIS_Y1)));
    modifier->push_back(ModifierPtr(new FourWayRestrictorModifier(XBOX_AXIS_X2, XBOX_AXIS_Y2)));
  }

  if (!opts.controller.back().relative_axis_map.empty())
  {
    boost::shared_ptr<AxismapModifier> axismap(new AxismapModifier);

    for(std::map<XboxAxis, AxisFilterPtr>::const_iterator i = opts.controller.back().relative_axis_map.begin();
        i != opts.controller.back().relative_axis_map.end(); ++i)
    {
      axismap->add_filter(i->first, i->second); 
    }

    modifier->push_back(axismap);
  }

  if (opts.controller.back().dpad_rotation)
  {
    modifier->push_back(ModifierPtr(new DpadRotationModifier(opts.controller.back().dpad_rotation)));
  }

  if (!opts.controller.back().autofire_map.empty())
  {
    boost::shared_ptr<ButtonmapModifier> buttonmap(new ButtonmapModifier);

    for(std::map<XboxButton, ButtonFilterPtr>::const_iterator i = opts.controller.back().autofire_map.begin();
        i != opts.controller.back().autofire_map.end(); ++i)
    {
      buttonmap->add_filter(i->first, i->second); 
    }

    modifier->push_back(buttonmap);
  }

  // axismap, buttonmap comes last, as otherwise they would mess up the button and axis names
  if (!opts.controller.back().buttonmap->empty())
  {
    modifier->push_back(opts.controller.back().buttonmap);
  }

  if (!opts.controller.back().axismap->empty())
  {
    modifier->push_back(opts.controller.back().axismap);
  }

  modifier->insert(modifier->end(), opts.controller.back().modifier.begin(), opts.controller.back().modifier.end());
}

void
XboxdrvThread::launch_child_process()
{
  if (!m_child_exec.empty())
  { // launch program if one was given
    m_pid = fork();
    if (m_pid == 0) // child
    {
      char** argv = static_cast<char**>(malloc(sizeof(char*) * m_child_exec.size() + 1));
      for(size_t i = 0; i < m_child_exec.size(); ++i)
      {
        argv[i] = strdup(m_child_exec[i].c_str());
      }
      argv[m_child_exec.size()] = NULL;

      if (execvp(m_child_exec[0].c_str(), argv) == -1)
      {
        std::cout << "error: " << m_child_exec[0] << ": " << strerror(errno) << std::endl;
        // FIXME: must signal the parent process
        _exit(EXIT_FAILURE);
      }
    }
  }
}

void
XboxdrvThread::watch_chid_process()
{
  if (m_pid != -1)
  {
    int status = 0;
    int ret = waitpid(m_pid, &status, WNOHANG);

    // greater 0 means something changed with the process
    if (ret > 0)
    {
      if (WIFEXITED(status))
      {
        if (WEXITSTATUS(status) != 0)
        {
          std::cout << "error: child program has stopped with exit status " << WEXITSTATUS(status) << std::endl;
        }
        else
        {
          std::cout << "child program exited successful" << std::endl;
        }
        global_exit_xboxdrv = true;
      }
      else if (WIFSIGNALED(status))
      {
        std::cout << "error: child program was terminated by " << WTERMSIG(status) << std::endl;
        global_exit_xboxdrv = true;
      }
    }
  }
}

void
XboxdrvThread::controller_loop(GamepadType type, uInput* uinput, const Options& opts)
{
  launch_child_process();

  try 
  {
    uint32_t last_time = get_time();
    while(m_loop && !global_exit_xboxdrv) // FIXME: should not directly depend on global_exit_xboxdrv
    {
      XboxGenericMsg msg;

      if (m_controller->read(msg, opts.verbose, m_timeout))
      {
        m_oldrealmsg = msg;
      }
      else
      {
        // no new data read, so copy the last read data
        msg = m_oldrealmsg;
      }

      /*
        if (code == cfg.config_toggle_button)
        {
        if (value)
        {
        reset_all_outputs();
        cfg.next_input_mapping();
        }
        }
      */

      // Calc changes in time
      uint32_t this_time = get_time();
      int msec_delta = this_time - last_time;
      last_time = this_time;

      // run the controller message through all modifier
      for(std::vector<ModifierPtr>::iterator i = m_config.get_config()->get_modifier().begin();
          i != m_config.get_config()->get_modifier().end(); 
          ++i)
      {
        (*i)->update(msec_delta, msg);
      }

      if (memcmp(&msg, &m_oldmsg, sizeof(XboxGenericMsg)) != 0)
      { // Only send a new event out if something has changed,
        // this is useful since some controllers send events
        // even if nothing has changed, deadzone can cause this
        // too
        m_oldmsg = msg;

        // output current Xbox gamepad state to stdout
        if (!opts.silent)
        {
          std::cout << msg << std::endl;
        }

        // send current Xbox state to uinput
        if (uinput || !m_config.empty())
        {
          m_config.get_config()->get_uinput().send(msg);
        }
                 
        if (opts.rumble)
        { // FIXME: kind of ugly here, should be a filter, but filters
          // can't talk back to the controller
          if (type == GAMEPAD_XBOX)
          {
            set_rumble(m_controller.get(), opts.rumble_gain, msg.xbox.lt, msg.xbox.rt);
          }
          else if (type == GAMEPAD_XBOX360 ||
                   type == GAMEPAD_XBOX360_WIRELESS)
          {
            set_rumble(m_controller.get(), opts.rumble_gain, msg.xbox360.lt, msg.xbox360.rt);
          }
          else if (type == GAMEPAD_FIRESTORM ||
                   type == GAMEPAD_FIRESTORM_VSB)
          {
            set_rumble(m_controller.get(), opts.rumble_gain,
                       std::min(255, abs((msg.xbox360.y1>>8)*2)), 
                       std::min(255, abs((msg.xbox360.y2>>8)*2)));
          }
        }
      }

      if (uinput)
      {
        uinput->update(msec_delta);
      }

      watch_chid_process();
    }
  }
  catch(const std::exception& err)
  {
    // catch read errors from USB and other stuff that can go wrong
    log_error << err.what() << std::endl;
  }
}

void
XboxdrvThread::start_thread(GamepadType type, uInput* uinput, const Options& opts)
{
  assert(m_thread.get() == 0);
  m_thread.reset(new boost::thread(boost::bind(&XboxdrvThread::controller_loop, this, 
                                               type, uinput, boost::cref(opts))));
}

void
XboxdrvThread::stop_thread()
{
  assert(m_thread.get());

  m_loop = false;
  m_thread->join();
  m_thread.reset();
}

bool
XboxdrvThread::try_join_thread()
{
  bool got_joined = m_thread->timed_join(boost::posix_time::time_duration(0,0,0,0));
  if (got_joined)
  {
    m_thread.reset();
    return true;
  }
  else
  {
    return false;
  }
}

/* EOF */
