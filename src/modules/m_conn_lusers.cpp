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
#include "inspircd.h"

/* $ModDesc: Sends the /LUSERS on connect */



// This has to be the simplest module ever.
// The RFC doesnt specify that you should send the /LUSERS numerics
// on connect, but someone asked for it, so its in a module.

class ModuleConnLUSERS : public Module
{
 private:
	 
	 
 public:
	ModuleConnLUSERS(InspIRCd* Me)
		: Module::Module(Me)
	{
		
	}
	
	virtual ~ModuleConnLUSERS()
	{
	}
	
	virtual Version GetVersion()
	{
		return Version(1,1,0,1,VF_VENDOR,API_VERSION);
	}

	void Implements(char* List)
	{
		List[I_OnUserConnect] = 1;
	}
	
	virtual void OnUserConnect(userrec* user)
	{
		// if we're using a protocol module, we cant just call
		// the command handler because the protocol module
		// has hooked it. We must call OnPreCommand in the
		// protocol module. Yes, at some point there will
		// be a way to get the current protocol module's name
		// from the core and probably a pointer to its class.
		Module* Proto = ServerInstance->FindModule("m_spanningtree.so");
		if (Proto)
		{
			Proto->OnPreCommand("LUSERS", NULL, 0, user, true, "LUSERS");
		}
		else
		{
			ServerInstance->CallCommandHandler("LUSERS", NULL, 0, user);
		}
	}
};


//
// The ModuleConnLUSERSFactory class inherits from ModuleFactory
// and creates a ModuleConnLUSERS object when requested.
//

class ModuleConnLUSERSFactory : public ModuleFactory
{
 public:
	ModuleConnLUSERSFactory()
	{
	}
	
	~ModuleConnLUSERSFactory()
	{
	}
	
	virtual Module * CreateModule(InspIRCd* Me)
	{
		return new ModuleConnLUSERS(Me);
	}
	
};


//
// The "C" linkage factory0() function creates the ModuleConnLUSERSFactory
// class for this library
//

extern "C" void * init_module( void )
{
	return new ModuleConnLUSERSFactory;
}

