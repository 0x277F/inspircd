/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  InspIRCd is copyright (C) 2002-2007 ChatSpike-Dev.
 *                       E-mail:
 *                <brain@chatspike.net>
 *                <Craig@chatspike.net>
 *
 * Written by Craig Edwards, Craig McLure, and others.
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

#ifndef __CMD_SUMMON_H__
#define __CMD_SUMMON_H__

// include the common header files

#include <string>
#include <vector>
#include "inspircd.h"
#include "users.h"
#include "channels.h"

/** Handle /SUMMON stub
 */
class cmd_summon : public command_t
{
 public:
        cmd_summon (InspIRCd* Instance) : command_t(Instance,"SUMMON",0,0) { }
        CmdResult Handle(const char** parameters, int pcnt, userrec *user);
};

#endif
