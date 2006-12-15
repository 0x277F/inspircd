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

#include "users.h"
#include "channels.h"
#include "modules.h"


/* $ModDesc: Sends a numeric on connect which cripples a common type of trojan/spambot */

class ModuleAntiBear : public Module
{
 private:

 public:
	ModuleAntiBear(InspIRCd* Me) : Module::Module(Me)
	{
		
	}
	
	virtual ~ModuleAntiBear()
	{
	}
	
	virtual Version GetVersion()
	{
		return Version(1,1,0,0,VF_VENDOR,API_VERSION);
	}

	void Implements(char* List)
	{
		List[I_OnUserRegister] = 1;
	}
	
	virtual int OnUserRegister(userrec* user)
	{
		user->WriteServ("439 %s :This server has anti-spambot mechanisms enabled.", user->nick);
		user->WriteServ("931 %s :Malicious bots, spammers, and other automated systems of dubious origin are NOT welcome here.", user->nick);
		return 0;
	}
};

class ModuleAntiBearFactory : public ModuleFactory
{
 public:
	ModuleAntiBearFactory()
	{
	}
	
	~ModuleAntiBearFactory()
	{
	}
	
	virtual Module * CreateModule(InspIRCd* Me)
	{
		return new ModuleAntiBear(Me);
	}
	
};


//
// The "C" linkage factory0() function creates the ModuleAntiBearFactory
// class for this library
//

extern "C" void * init_module( void )
{
	return new ModuleAntiBearFactory;
}
