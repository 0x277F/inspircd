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
#include "commands/cmd_info.h"

extern "C" command_t* init_command(InspIRCd* Instance)
{
	return new cmd_info(Instance);
}

/** Handle /INFO
 */
CmdResult cmd_info::Handle (const char** parameters, int pcnt, userrec *user)
{
	user->WriteServ( "371 %s :. o O ( The Inspire Internet Relay Chat Server  ) O o .", user->nick);
	user->WriteServ( "371 %s :      ( Putting the ricer into ircer since 2007 )", user->nick);
	user->WriteServ( "371 %s : ", user->nick);
	user->WriteServ( "371 %s :Core Developers:", user->nick);
	user->WriteServ( "371 %s :        Craig Edwards (Brain)", user->nick);
	user->WriteServ( "371 %s :        Craig McLure", user->nick);
	user->WriteServ( "371 %s :        w00t", user->nick);
	user->WriteServ( "371 %s :        Om", user->nick);
	user->WriteServ( "371 %s :        Special", user->nick);
	user->WriteServ( "371 %s :        pippijn", user->nick);
	user->WriteServ( "371 %s :        peavey", user->nick);
	user->WriteServ( "371 %s : ", user->nick);
	user->WriteServ( "371 %s :Contributors:", user->nick);
	user->WriteServ( "371 %s :        typobox43", user->nick);
	user->WriteServ( "371 %s :        Jazza", user->nick);
	user->WriteServ( "371 %s :        jamie", user->nick);
	user->WriteServ( "371 %s :        LeaChim", user->nick);
	user->WriteServ( "371 %s :        satmd", user->nick);
	user->WriteServ( "371 %s :        nenolod", user->nick);
	user->WriteServ( "371 %s : ", user->nick);
	user->WriteServ( "371 %s :Testers:", user->nick);
	user->WriteServ( "371 %s :        CC", user->nick);
	user->WriteServ( "371 %s :        Piggles", user->nick);
	user->WriteServ( "371 %s :        Foamy", user->nick);
	user->WriteServ( "371 %s :        Hart", user->nick);
	user->WriteServ( "371 %s :        RageD", user->nick);
	user->WriteServ( "371 %s :        [ed]", user->nick);
	user->WriteServ( "371 %s :        Azhrarn", user->nick);
	user->WriteServ( "371 %s :        nenolod", user->nick);
	user->WriteServ( "371 %s :        luigiman", user->nick);
	user->WriteServ( "371 %s :        Chu", user->nick);
	user->WriteServ( "371 %s :        aquanight", user->nick);
	user->WriteServ( "371 %s :        xptek", user->nick);
	user->WriteServ( "371 %s :        Grantlinks", user->nick);
	user->WriteServ( "371 %s :        Rob", user->nick);
	user->WriteServ( "371 %s :        angelic", user->nick);
	user->WriteServ( "371 %s :        Jason", user->nick);
	user->WriteServ( "371 %s :        ThaPrince", user->nick);
	user->WriteServ( "371 %s :        eggy", user->nick);
	user->WriteServ( "371 %s :        skenmy", user->nick);
	user->WriteServ( "371 %s : ", user->nick);
	user->WriteServ( "371 %s :Contains portions of FireDNS written by", user->nick);
	user->WriteServ( "371 %s :Ian Gulliver, (c) 2002.", user->nick);
	user->WriteServ( "371 %s : ", user->nick);
	user->WriteServ( "371 %s :Thanks to irc-junkie and searchirc", user->nick);
	user->WriteServ( "371 %s :for the nice comments and the help", user->nick);
	user->WriteServ( "371 %s :you gave us in attracting users to", user->nick);
	user->WriteServ( "371 %s :this software.", user->nick);
	user->WriteServ( "371 %s : ", user->nick);
	user->WriteServ( "371 %s :Best experienced with: An IRC client.", user->nick);
	FOREACH_MOD(I_OnInfo,OnInfo(user));
	user->WriteServ( "374 %s :End of /INFO list", user->nick);
	return CMD_SUCCESS;
}
