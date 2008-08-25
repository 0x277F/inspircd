#include "inspircd.h"
#include "users.h"
#include "channels.h"
#include "modules.h"

#include <set>

#include <sstream>

#include <algorithm>

/* $ModDesc: Implementation of callerid (umode +g & /accept, ala hybrid etc) */

class callerid_data : public classbase
{
 public:
	time_t lastnotify;
	std::set<User*> accepting;

	callerid_data() : lastnotify(0) { }
	callerid_data(const std::string& str, InspIRCd* ServerInstance)
	{
		irc::commasepstream s(str);
		std::string tok;
		if (s.GetToken(tok))
		{
			lastnotify = ConvToInt(tok);
		}
		while (s.GetToken(tok))
		{
			if (tok.empty())
			{
				continue;
			}
			User* u = ServerInstance->FindUUID(tok);
			if (!u)
			{
				u = ServerInstance->FindNick(tok);
			}
			if (!u)
			{
				continue;
			}
			accepting.insert(u);
		}
	}

	std::string ToString(bool displayable) const
	{
		std::ostringstream oss;
		oss << lastnotify;
		for (std::set<User*>::const_iterator i = accepting.begin(); i != accepting.end(); ++i)
		{
			// Encode UIDs.
			oss << "," << (displayable ? (*i)->nick : (*i)->uuid);
		}
		oss << std::ends;
		return oss.str();
	}
};

callerid_data* GetData(User* who, bool extend = true)
{
	callerid_data* dat;
	if (who->GetExt("callerid_data", dat))
		return dat;
	else
	{
		if (extend)
		{
			dat = new callerid_data;
			who->Extend("callerid_data", dat);
			return dat;
		}
		else
			return NULL;
	}
}

void RemoveData(User* who)
{
	callerid_data* dat;
	who->GetExt("callerid_data", dat);

	if (!dat)
		return;

	who->Shrink("callerid_data");
	delete dat;
}

void RemoveFromAllAccepts(InspIRCd* ServerInstance, User* who)
{
	for (user_hash::iterator i = ServerInstance->Users->clientlist->begin(); i != ServerInstance->Users->clientlist->end(); ++i)
	{
		callerid_data* dat = GetData(i->second, false);

		if (!dat)
			continue;

		std::set<User*>::iterator iter = dat->accepting.find(who);

		if (iter == dat->accepting.end())
			continue;

		dat->accepting.erase(iter);
	}
}

class User_g : public SimpleUserModeHandler
{
public:
	User_g(InspIRCd* Instance) : SimpleUserModeHandler(Instance, 'g') { }
};

class CommandAccept : public Command
{
private:
	unsigned int& maxaccepts;
public:
	CommandAccept(InspIRCd* Instance, unsigned int& max) : Command(Instance, "ACCEPT", 0, 1), maxaccepts(max)
	{
		source = "m_callerid.so";
		syntax = "{[+|-]<nicks>}|*}";
		TRANSLATE2(TR_CUSTOM, TR_END);
	}

	virtual void EncodeParameter(std::string& parameter, int index)
	{
		if (index != 0)
			return;
		std::string out = "";
		irc::commasepstream nicks(parameter);
		std::string tok;
		while (nicks.GetToken(tok))
		{
			if (tok == "*")
			{
				continue; // Drop list requests, since remote servers ignore them anyway.
			}
			if (!out.empty())
				out.append(",");
			bool dash = false;
			if (tok[0] == '-')
			{
				dash = true;
				tok.erase(0, 1); // Remove the dash.
			}
			User* u = ServerInstance->FindNick(tok);
			if (u)
			{
				if (dash)
					out.append("-");
				out.append(u->uuid);
			}
			else
			{
				if (dash)
					out.append("-");
				out.append(tok);
			}
		}
		parameter = out;
	}

	/** Will take any number of nicks (up to MaxTargets), which can be seperated by commas.
	 * - in front of any nick removes, and an * lists. This effectively means you can do:
	 * /accept nick1,nick2,nick3,*
	 * to add 3 nicks and then show your list
	 */
	CmdResult Handle(const std::vector<std::string> &parameters, User* user)
	{
		if (ServerInstance->Parser->LoopCall(user, this, parameters, 0))
			return CMD_SUCCESS;
		/* Even if callerid mode is not set, we let them manage their ACCEPT list so that if they go +g they can
		 * have a list already setup. */

		std::string tok = parameters[0];

		if (tok == "*")
		{
			if (IS_LOCAL(user))
				ListAccept(user);
			return CMD_LOCALONLY;
		}
		else if (tok[0] == '-')
		{
			User* whotoremove = ServerInstance->FindNick(tok.substr(1));
			if (whotoremove)
				return (RemoveAccept(user, whotoremove, false) ? CMD_SUCCESS : CMD_FAILURE);
			else
				return CMD_FAILURE;
		}
		else
		{
			User* whotoadd = ServerInstance->FindNick(tok[0] == '+' ? tok.substr(1) : tok);
			if (whotoadd)
				return (AddAccept(user, whotoadd, false) ? CMD_SUCCESS : CMD_FAILURE);
			else
			{
				user->WriteNumeric(401, "%s %s :No such nick/channel", user->nick.c_str(), tok.c_str());
				return CMD_FAILURE;
			}
		}
	}

	void ListAccept(User* user)
	{
		callerid_data* dat = GetData(user, false);
		if (dat)
		{
			for (std::set<User*>::iterator i = dat->accepting.begin(); i != dat->accepting.end(); ++i)
				user->WriteNumeric(281, "%s %s", user->nick.c_str(), (*i)->nick.c_str());
		}
		user->WriteNumeric(282, "%s :End of ACCEPT list", user->nick.c_str());
	}

	bool AddAccept(User* user, User* whotoadd, bool quiet)
	{
		callerid_data* dat = GetData(user, true);
		if (dat->accepting.size() >= maxaccepts)
		{
			if (!quiet)
				user->WriteNumeric(456, "%s :Accept list is full (limit is %d)", user->nick.c_str(), maxaccepts);

			return false;
		}
		if (!dat->accepting.insert(whotoadd).second)
		{
			if (!quiet)
				user->WriteNumeric(457, "%s %s :is already on your accept list", user->nick.c_str(), whotoadd->nick.c_str());

			return false;
		}
		return true;
	}

	bool RemoveAccept(User* user, User* whotoremove, bool quiet)
	{
		callerid_data* dat = GetData(user, false);
		if (!dat)
		{
			if (!quiet)
				user->WriteNumeric(458, "%s %s :is not on your accept list", user->nick.c_str(), whotoremove->nick.c_str());

			return false;
		}
		std::set<User*>::iterator i = dat->accepting.find(whotoremove);
		if (i == dat->accepting.end())
		{
			if (!quiet)
				user->WriteNumeric(458, "%s %s :is not on your accept list", user->nick.c_str(), whotoremove->nick.c_str());

			return false;
		}
		dat->accepting.erase(i);
		return true;
	}
};

class ModuleCallerID : public Module
{
private:
	CommandAccept *mycommand;
	User_g* myumode;

	// Configuration variables:
	unsigned int maxaccepts; // Maximum ACCEPT entries.
	bool operoverride; // Operators can override callerid.
	bool tracknick; // Allow ACCEPT entries to update with nick changes.
	unsigned int notify_cooldown; // Seconds between notifications.

public:
	ModuleCallerID(InspIRCd* Me) : Module(Me)
	{
		OnRehash(NULL, "");
		mycommand = new CommandAccept(ServerInstance, maxaccepts);
		myumode = new User_g(ServerInstance);

		if (!ServerInstance->Modes->AddMode(myumode))
		{
			delete mycommand;
			delete myumode;
			throw ModuleException("Could not add usermode +g");
		}
		try
		{
			ServerInstance->AddCommand(mycommand);
		}
		catch (const ModuleException& e)
		{
			delete mycommand;
			delete myumode;
			throw ModuleException("Could not add command!");
		}

		Implementation eventlist[] = { I_OnRehash, I_OnUserPreNick, I_OnUserQuit, I_On005Numeric, I_OnUserPreNotice, I_OnUserPreMessage, I_OnCleanup };
		ServerInstance->Modules->Attach(eventlist, this, 7);
	}

	virtual ~ModuleCallerID()
	{
		ServerInstance->Modes->DelMode(myumode);
		delete myumode;
	}

	virtual Version GetVersion()
	{
		return Version("$Id$", VF_COMMON | VF_VENDOR, API_VERSION);
	}

	virtual void On005Numeric(std::string& output)
	{
		output += " CALLERID=g";
	}

	int PreText(User* user, User* dest, std::string& text, bool notice)
	{
		if (!dest->IsModeSet('g'))
			return 0;

		if (operoverride && IS_OPER(user))
			return 0;

		callerid_data* dat = GetData(dest, true);
		std::set<User*>::iterator i = dat->accepting.find(dest);

		if (i == dat->accepting.end())
		{
			time_t now = time(NULL);
			/* +g and *not* accepted */
			user->WriteNumeric(716, "%s %s :is in +g mode (server-side ignore).", user->nick.c_str(), dest->nick.c_str());
			if (now > (dat->lastnotify + (time_t)notify_cooldown))
			{
				user->WriteNumeric(717, "%s %s :has been informed that you messaged them.", user->nick.c_str(), dest->nick.c_str());
				dest->WriteNumeric(718, "%s %s %s@%s :is messaging you, and you have umode +g", dest->nick.c_str(), user->nick.c_str(), user->ident.c_str(), user->dhost.c_str());
				dat->lastnotify = now;
			}
			return 1;
		}
		return 0;
	}

	virtual int OnUserPreMessage(User* user, void* dest, int target_type, std::string& text, char status, CUList &exempt_list)
	{
		if (IS_LOCAL(user) && target_type == TYPE_USER)
			return PreText(user, (User*)dest, text, true);

		return 0;
	}

	virtual int OnUserPreNotice(User* user, void* dest, int target_type, std::string& text, char status, CUList &exempt_list)
	{
		if (IS_LOCAL(user) && target_type == TYPE_USER)
			return PreText(user, (User*)dest, text, true);

		return 0;
	}

	virtual void OnCleanup(int type, void* item)
	{
		if (type != TYPE_USER)
			return;

		User* u = (User*)item;
		/* Cleanup only happens on unload (before dtor), so keep this O(n) instead of O(n^2) which deferring to OnUserQuit would do.  */
		RemoveData(u);
	}

	virtual void OnSyncUserMetaData(User* user, Module* proto, void* opaque, const std::string& extname, bool displayable)
	{
		if (extname == "callerid_data")
		{
			callerid_data* dat = GetData(user, false);
			if (dat)
			{
				std::string str = dat->ToString(displayable);
				proto->ProtoSendMetaData(opaque, TYPE_USER, user, extname, str);
			}
		}
	}

	virtual void OnDecodeMetaData(int target_type, void* target, const std::string& extname, const std::string& extdata)
	{
		if (target_type == TYPE_USER && extname == "callerid_data")
		{
			User* u = (User*)target;
			callerid_data* dat = new callerid_data(extdata, ServerInstance);
			u->Extend("callerid_data", dat);
		}
	}

	virtual int OnUserPreNick(User* user, const std::string& newnick)
	{
		if (!tracknick)
			RemoveFromAllAccepts(ServerInstance, user);
		return 0;
	}

	virtual void OnUserQuit(User* user, const std::string& message, const std::string& oper_message)
	{
		RemoveData(user);
		RemoveFromAllAccepts(ServerInstance, user);
	}

	virtual void OnRehash(User* user, const std::string& parameter)
	{
		ConfigReader Conf(ServerInstance);
		maxaccepts = Conf.ReadInteger("callerid", "maxaccepts", "16", 0, true);
		operoverride = Conf.ReadFlag("callerid", "operoverride", "0", 0);
		tracknick = Conf.ReadFlag("callerid", "tracknick", "0", 0);
		notify_cooldown = Conf.ReadInteger("callerid", "cooldown", "60", 0, true);
	}
};

MODULE_INIT(ModuleCallerID)


