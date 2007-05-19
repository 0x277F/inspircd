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

#ifndef __CMD_QUIT_H__
#define __CMD_QUIT_H__

// include the common header files

#include "users.h"
#include "channels.h"

/** Handle /QUIT
 */
class cmd_quit : public command_t
{
 public:
	cmd_quit (InspIRCd* Instance) : command_t(Instance,"QUIT",0,0,true) { syntax = "[<message>]"; }
	CmdResult Handle(const char** parameters, int pcnt, userrec *user);
};

#endif
