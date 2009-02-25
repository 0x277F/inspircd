/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  InspIRCd: (C) 2002-2009 InspIRCd Development Team
 * See: http://www.inspircd.org/wiki/index.php/Credits
 *
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

#include "inspircd.h"
#include "u_listmode.h"

/* $ModDep: ../../include/u_listmode.h */

/* $ModDesc: Implements extban/invex +I O: - opertype bans */

class ModuleOperInvex : public Module
{
 private:
 public:
	ModuleOperInvex(InspIRCd* Me) : Module(Me)
	{
		Implementation eventlist[] = { I_OnUserPreJoin, I_On005Numeric, I_OnCheckInvite };
		ServerInstance->Modules->Attach(eventlist, this, 3);
	}

	virtual ~ModuleOperInvex()
	{
	}

	virtual Version GetVersion()
	{
		return Version("$Id$", VF_COMMON|VF_VENDOR, API_VERSION);
	}

	virtual int OnCheckInvite(User *user, Channel *c)
	{
		if (!IS_LOCAL(user) || !IS_OPER(user))
			return 0;

		Module* ExceptionModule = ServerInstance->Modules->Find("m_inviteexception.so");
		if (ExceptionModule)
		{
			if (ListModeRequest(this, ExceptionModule, user->oper, 'O', c).Send())
			{
				// Oper type is exempt
				return 1;
			}
		}

		return 0;
	}

	virtual int OnUserPreJoin(User *user, Channel *c, const char *cname, std::string &privs, const std::string &key)
	{
		if (!IS_LOCAL(user) || !IS_OPER(user))
			return 0;

		if (!c)
			return 0;

		if (c->IsExtBanned(user->oper, 'O'))
		{
			user->WriteNumeric(ERR_BANNEDFROMCHAN, "%s %s :Cannot join channel (You're banned)", user->nick.c_str(),  c->name.c_str());
			return 1;
		}

		return 0;
	}

	virtual void On005Numeric(std::string &output)
	{
		ServerInstance->AddExtBanChar('O');
	}
};


MODULE_INIT(ModuleOperInvex)

