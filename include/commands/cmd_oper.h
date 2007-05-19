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

#ifndef __CMD_OPER_H__
#define __CMD_OPER_H__

// include the common header files

#include "users.h"
#include "channels.h"

bool OneOfMatches(const char* host, const char* ip, const char* hostlist);

/** Handle /OPER
 */
class cmd_oper : public command_t
{
 public:
	cmd_oper (InspIRCd* Instance) : command_t(Instance,"OPER",0,2) { syntax = "<username> <password>"; }
	CmdResult Handle(const char** parameters, int pcnt, userrec *user);
};

#endif
