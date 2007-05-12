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

#include <stdio.h>
#include <string>
#include "users.h"
#include "channels.h"
#include "modules.h"
#include "inspircd.h"

/* $ModDesc: Provides support for USERIP command */

/** Handle /USERIP
 */
class cmd_userip : public command_t
{
 public:
	cmd_userip (InspIRCd* Instance) : command_t(Instance,"USERIP", 'o', 1)
	{
		this->source = "m_userip.so";
		syntax = "<nick>{,<nick>}";
	}

	CmdResult Handle (const char** parameters, int pcnt, userrec *user)
	{
		char Return[MAXBUF],junk[MAXBUF];
		snprintf(Return,MAXBUF,"340 %s :",user->nick);
		for (int i = 0; i < pcnt; i++)
		{
			userrec *u = ServerInstance->FindNick(parameters[i]);
			if ((u) && (u->registered == REG_ALL))
			{
				snprintf(junk,MAXBUF,"%s%s=+%s@%s ",u->nick,IS_OPER(u) ? "*" : "",u->ident,u->GetIPString());
				strlcat(Return,junk,MAXBUF);
			}
		}
		user->WriteServ(Return);

		/* Dont send to the network */
		return CMD_FAILURE;
	}
};

class ModuleUserIP : public Module
{
	cmd_userip* mycommand;
 public:
	ModuleUserIP(InspIRCd* Me)
		: Module::Module(Me)
	{
		
		mycommand = new cmd_userip(ServerInstance);
		ServerInstance->AddCommand(mycommand);
	}

	void Implements(char* List)
	{
		List[I_On005Numeric] = 1;
	}

	virtual void On005Numeric(std::string &output)
	{
		output = output + std::string(" USERIP");
	}
	
	virtual ~ModuleUserIP()
	{
	}
	
	virtual Version GetVersion()
	{
		return Version(1,1,0,1,VF_VENDOR,API_VERSION);
	}
	
};

// stuff down here is the module-factory stuff. For basic modules you can ignore this.

class ModuleUserIPFactory : public ModuleFactory
{
 public:
	ModuleUserIPFactory()
	{
	}
	
	~ModuleUserIPFactory()
	{
	}
	
	virtual Module * CreateModule(InspIRCd* Me)
	{
		return new ModuleUserIP(Me);
	}
	
};


extern "C" void * init_module( void )
{
	return new ModuleUserIPFactory;
}

