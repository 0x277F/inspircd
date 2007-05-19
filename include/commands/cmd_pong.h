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

#ifndef __CMD_PONG_H__
#define __CMD_PONG_H__

// include the common header files

#include "inspircd.h"
#include "users.h"
#include "channels.h"

/** Handle /PONG
 */
class cmd_pong : public command_t
{
 public:
	cmd_pong (InspIRCd* Instance) : command_t(Instance,"PONG",0,1) { syntax = "<ping-text>"; }
	CmdResult Handle(const char** parameters, int pcnt, userrec *user);
};

#endif
