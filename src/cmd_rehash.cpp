/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  InspIRCd: (C) 2002-2007 InspIRCd Development Team
 * See: http://www.inspircd.org/wiki/index.php/Credits
 *
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

#include "configreader.h"
#include "users.h"
#include "modules.h"
#include "commands/cmd_rehash.h"



extern "C" command_t* init_command(InspIRCd* Instance)
{
	return new cmd_rehash(Instance);
}

CmdResult cmd_rehash::Handle (const char** parameters, int pcnt, userrec *user)
{
	user->WriteServ("382 %s %s :Rehashing",user->nick,ServerConfig::CleanFilename(CONFIG_FILE));
	std::string parameter = "";
	std::string old_disabled = ServerInstance->Config->DisabledCommands;
	if (pcnt)
	{
		parameter = parameters[0];
	}
	else
	{
		ServerInstance->WriteOpers("%s is rehashing config file %s",user->nick,ServerConfig::CleanFilename(CONFIG_FILE));
		ServerInstance->CloseLog();
		ServerInstance->OpenLog(NULL,0);
		ServerInstance->Config->Read(false,user);
	}
	if (old_disabled != ServerInstance->Config->DisabledCommands)
		InitializeDisabledCommands(ServerInstance->Config->DisabledCommands, ServerInstance);

	FOREACH_MOD(I_OnRehash,OnRehash(parameter));

	return CMD_SUCCESS;
}

