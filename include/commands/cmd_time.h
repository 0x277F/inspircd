/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  InspIRCd is copyright (C) 2002-2007 ChatSpike-Dev.
 *		       E-mail:
 *		<brain@chatspike.net>
 *		<Craig@chatspike.net>
 *
 * Written by Craig Edwards, Craig McLure, and others.
 * This program is free but copyrighted software; see
 *	    the file COPYING for details.
 *
 * ---------------------------------------------------
 */

#ifndef __CMD_TIME_H__
#define __CMD_TIME_H__

// include the common header files

#include "users.h"
#include "channels.h"

/** Handle /TIME
 */
class cmd_time : public command_t
{
 public:
	cmd_time (InspIRCd* Instance) : command_t(Instance,"TIME",0,0) { syntax = "[<servername>]"; }
	CmdResult Handle(const char** parameters, int pcnt, userrec *user);
};

#endif
