/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  InspIRCd: (C) 2002-2008 InspIRCd Development Team
 * See: http://www.inspircd.org/wiki/index.php/Credits
 *
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

#include "inspircd.h"

/* $ModDesc: Provides channel modes +a and +q */

#define PROTECT_VALUE 40000
#define FOUNDER_VALUE 50000

/** Handles basic operation of +qa channel modes
 */
class FounderProtectBase
{
 private:
	InspIRCd* MyInstance;
	std::string extend;
	std::string type;
	int list;
	int end;
 protected:
	bool& remove_own_privs;
	bool& remove_other_privs;
 public:
	FounderProtectBase(InspIRCd* Instance, const std::string &ext, const std::string &mtype, int l, int e, bool &remove_own, bool &remove_others) :
		MyInstance(Instance), extend(ext), type(mtype), list(l), end(e), remove_own_privs(remove_own), remove_other_privs(remove_others)
	{
	}

	ModePair ModeSet(User* source, User* dest, Channel* channel, const std::string &parameter)
	{
		User* x = MyInstance->FindNick(parameter);
		if (x)
		{
			if (!channel->HasUser(x))
			{
				return std::make_pair(false, parameter);
			}
			else
			{
				std::string item = extend+std::string(channel->name);
				if (x->GetExt(item))
				{
					return std::make_pair(true, x->nick);
				}
				else
				{
					return std::make_pair(false, parameter);
				}
			}
		}
		return std::make_pair(false, parameter);
	}

	void RemoveMode(Channel* channel, char mc, irc::modestacker* stack)
	{
		CUList* cl = channel->GetUsers();
		std::string item = extend + std::string(channel->name);
		std::vector<std::string> mode_junk;
		mode_junk.push_back(channel->name);
		irc::modestacker modestack(MyInstance, false);
		std::deque<std::string> stackresult;				

		for (CUList::iterator i = cl->begin(); i != cl->end(); i++)
		{
			if (i->first->GetExt(item))
			{
				if (stack)
					stack->Push(mc, i->first->nick);
				else
					modestack.Push(mc, i->first->nick);
			}
		}

		if (stack)
			return;

		while (modestack.GetStackedLine(stackresult))
		{
			for (size_t j = 0; j < stackresult.size(); j++)
			{
				mode_junk.push_back(stackresult[j]);
			}
			MyInstance->SendMode(mode_junk, MyInstance->FakeClient);
		}
	}

	void DisplayList(User* user, Channel* channel)
	{
		CUList* cl = channel->GetUsers();
		std::string item = extend+std::string(channel->name);
		for (CUList::reverse_iterator i = cl->rbegin(); i != cl->rend(); ++i)
		{
			if (i->first->GetExt(item))
			{
				user->WriteServ("%d %s %s %s", list, user->nick.c_str(), channel->name.c_str(), i->first->nick.c_str());
			}
		}
		user->WriteServ("%d %s %s :End of channel %s list", end, user->nick.c_str(), channel->name.c_str(), type.c_str());
	}

	User* FindAndVerify(std::string &parameter, Channel* channel)
	{
		User* theuser = MyInstance->FindNick(parameter);
		if ((!theuser) || (!channel->HasUser(theuser)))
		{
			parameter.clear();
			return NULL;
		}
		return theuser;
	}

	bool CanRemoveOthers(User* u1, User* u2, Channel* c)
	{
		std::string item = extend+std::string(c->name);
		return (u1->GetExt(item) && u2->GetExt(item));
	}

	ModeAction HandleChange(User* source, User* theuser, bool adding, Channel* channel, std::string &parameter)
	{
		std::string item = extend+std::string(channel->name);

		if (adding)
		{
			if (!theuser->GetExt(item))
			{
				theuser->Extend(item);
				parameter = theuser->nick;
				return MODEACTION_ALLOW;
			}
		}
		else
		{
			if (theuser->GetExt(item))
			{
				theuser->Shrink(item);
				parameter = theuser->nick;
				return MODEACTION_ALLOW;
			}
		}
		return MODEACTION_DENY;
	}
};

/** Abstraction of FounderProtectBase for channel mode +q
 */
class ChanFounder : public ModeHandler, public FounderProtectBase
{
 public:
	ChanFounder(InspIRCd* Instance, char my_prefix, bool &depriv_self, bool &depriv_others)
		: ModeHandler(Instance, 'q', 1, 1, true, MODETYPE_CHANNEL, false, my_prefix, 0),
		  FounderProtectBase(Instance, "cm_founder_", "founder", 386, 387, depriv_self, depriv_others) { }

	unsigned int GetPrefixRank()
	{
		return FOUNDER_VALUE;
	}

	ModePair ModeSet(User* source, User* dest, Channel* channel, const std::string &parameter)
	{
		return FounderProtectBase::ModeSet(source, dest, channel, parameter);
	}

	void RemoveMode(Channel* channel, irc::modestacker* stack)
	{
		FounderProtectBase::RemoveMode(channel, this->GetModeChar(), stack);
	}

	void RemoveMode(User* user, irc::modestacker* stack)
	{
	}

	ModeAction OnModeChange(User* source, User* dest, Channel* channel, std::string &parameter, bool adding, bool)
	{
		User* theuser = FounderProtectBase::FindAndVerify(parameter, channel);

		if (!theuser)
		{
			return MODEACTION_DENY;
		}

		if ((!adding) && FounderProtectBase::CanRemoveOthers(source, theuser, channel))
		{
			return FounderProtectBase::HandleChange(source, theuser, adding, channel, parameter);
		}
		 // source is a server, or ulined, we'll let them +-q the user.
		if (source == ServerInstance->FakeClient ||
				((source == theuser) && (!adding) && (FounderProtectBase::remove_own_privs)) ||
				(ServerInstance->ULine(source->nick.c_str())) ||
				(ServerInstance->ULine(source->server)) ||
				(!*source->server) ||
				(!IS_LOCAL(source)))
		{
			return FounderProtectBase::HandleChange(source, theuser, adding, channel, parameter);
		}
		else
		{
			// whoops, someones being naughty!
			source->WriteNumeric(468, "%s %s :Only servers may set channel mode +q", source->nick.c_str(), channel->name.c_str());
			parameter.clear();
			return MODEACTION_DENY;
		}
	}

	void DisplayList(User* user, Channel* channel)
	{
		FounderProtectBase::DisplayList(user,channel);
	}
};

/** Abstraction of FounderProtectBase for channel mode +a
 */
class ChanProtect : public ModeHandler, public FounderProtectBase
{
 public:
	ChanProtect(InspIRCd* Instance, char my_prefix, bool &depriv_self, bool &depriv_others)
		: ModeHandler(Instance, 'a', 1, 1, true, MODETYPE_CHANNEL, false, my_prefix, 0),
		  FounderProtectBase(Instance,"cm_protect_","protected user", 388, 389, depriv_self, depriv_others) { }

	unsigned int GetPrefixRank()
	{
		return PROTECT_VALUE;
	}

	ModePair ModeSet(User* source, User* dest, Channel* channel, const std::string &parameter)
	{
		return FounderProtectBase::ModeSet(source, dest, channel, parameter);
	}

	void RemoveMode(Channel* channel, irc::modestacker* stack)
	{
		FounderProtectBase::RemoveMode(channel, this->GetModeChar(), stack);
	}

	void RemoveMode(User* user, irc::modestacker* stack)
	{
	}

	ModeAction OnModeChange(User* source, User* dest, Channel* channel, std::string &parameter, bool adding, bool)
	{
		User* theuser = FounderProtectBase::FindAndVerify(parameter, channel);

		if (!theuser)
			return MODEACTION_DENY;

		std::string founder = "cm_founder_"+std::string(channel->name);

		if ((!adding) && FounderProtectBase::CanRemoveOthers(source, theuser, channel))
		{
			return FounderProtectBase::HandleChange(source, theuser, adding, channel, parameter);
		}
		// source has +q, is a server, or ulined, we'll let them +-a the user.
		if (source == ServerInstance->FakeClient ||
			((source == theuser) && (!adding) && (FounderProtectBase::remove_own_privs)) || 
			(ServerInstance->ULine(source->nick.c_str())) ||
			(ServerInstance->ULine(source->server)) ||
			(!*source->server) ||
			(source->GetExt(founder)) ||
			(!IS_LOCAL(source)))
		{
			return FounderProtectBase::HandleChange(source, theuser, adding, channel, parameter);
		}
		else
		{
			// bzzzt, wrong answer!
			source->WriteNumeric(482, "%s %s :You are not a channel founder", source->nick.c_str(), channel->name.c_str());
			return MODEACTION_DENY;
		}
	}

	virtual void DisplayList(User* user, Channel* channel)
	{
		FounderProtectBase::DisplayList(user, channel);
	}

};

class ModuleChanProtect : public Module
{
	
	bool FirstInGetsFounder;
	char QPrefix;
	char APrefix;
	bool DeprivSelf;
	bool DeprivOthers;
	bool booting;
	ChanProtect* cp;
	ChanFounder* cf;
	
 public:
 
	ModuleChanProtect(InspIRCd* Me)
		: Module(Me), FirstInGetsFounder(false), QPrefix(0), APrefix(0), DeprivSelf(false), DeprivOthers(false), booting(true), cp(NULL), cf(NULL)
	{
		/* Load config stuff */
		LoadSettings();
		booting = false;

		/* Initialise module variables */

		cp = new ChanProtect(ServerInstance, APrefix, DeprivSelf, DeprivOthers);
		cf = new ChanFounder(ServerInstance, QPrefix, DeprivSelf, DeprivOthers);

		if (!ServerInstance->Modes->AddMode(cp) || !ServerInstance->Modes->AddMode(cf))
		{
			delete cp;
			delete cf;
			throw ModuleException("Could not add new modes!");
		}

		Implementation eventlist[] = { I_OnUserKick, I_OnUserPart, I_OnUserPreJoin, I_OnPostJoin, I_OnAccessCheck };
		ServerInstance->Modules->Attach(eventlist, this, 5);
	}

	virtual void OnUserKick(User* source, User* user, Channel* chan, const std::string &reason, bool &silent)
	{
		// FIX: when someone gets kicked from a channel we must remove their Extensibles!
		user->Shrink("cm_founder_"+std::string(chan->name));
		user->Shrink("cm_protect_"+std::string(chan->name));
	}

	virtual void OnUserPart(User* user, Channel* channel, const std::string &partreason, bool &silent)
	{
		// FIX: when someone parts a channel we must remove their Extensibles!
		user->Shrink("cm_founder_"+std::string(channel->name));
		user->Shrink("cm_protect_"+std::string(channel->name));
	}

	void LoadSettings()
	{
		/* Create a configreader class and read our flag,
		 * in old versions this was heap-allocated and the
		 * object was kept between rehashes...now we just
		 * stack-allocate it locally.
		 */
		ConfigReader Conf(ServerInstance);

		FirstInGetsFounder = Conf.ReadFlag("options", "noservices", 0);

		std::string qpre = Conf.ReadValue("options", "qprefix", 0);
		QPrefix = qpre.empty() ? 0 : qpre[0];

		std::string apre = Conf.ReadValue("options", "aprefix", 0);
		APrefix = apre.empty() ? 0 : apre[0];

		if ((APrefix && QPrefix) && APrefix == QPrefix)
			throw ModuleException("What the smeg, why are both your +q and +a prefixes the same character?");

		if (cp && ServerInstance->Modes->FindPrefix(APrefix) == cp)
			throw ModuleException("Looks like the +a prefix you picked for m_chanprotect is already in use. Pick another.");

		if (cf && ServerInstance->Modes->FindPrefix(QPrefix) == cf)
			throw ModuleException("Looks like the +q prefix you picked for m_chanprotect is already in use. Pick another.");

		DeprivSelf = Conf.ReadFlag("options","deprotectself",0);
		DeprivOthers = Conf.ReadFlag("options","deprotectothers",0);
	}
	
	virtual int OnUserPreJoin(User *user, Channel *chan, const char *cname, std::string &privs, const std::string &keygiven)
	{
		// if the user is the first user into the channel, mark them as the founder, but only if
		// the config option for it is set

		if (FirstInGetsFounder && !chan)
			privs = QPrefix + "@";
		
		return 0;
	}
	
	virtual void OnPostJoin(User *user, Channel *channel)
	{
		// This *must* be in PostJoin, not UserJoin - the former will make it appear to happen
		// before the client is in the channel
		
		// This notice was here originally because it was all done prior to the creation of
		// privs in OnUserPreJoin. I've left it because it might still be wanted, but i'm
		// not sure it really should be here - ops don't get shown, obviously, and the prefix
		// will appear in the names list for the user.. remove if desired -Special

		if (FirstInGetsFounder && channel->GetUserCounter() == 1)
			user->WriteServ("MODE %s +q %s", channel->name.c_str(), user->nick.c_str());
	}
	
	virtual int OnAccessCheck(User* source,User* dest,Channel* channel,int access_type)
	{
		// here we perform access checks, this is the important bit that actually stops kicking/deopping
		// etc of protected users. There are many types of access check, we're going to handle
		// a relatively small number of them relevent to our module using a switch statement.
		// don't allow action if:
		// (A) Theyre founder (no matter what)
		// (B) Theyre protected, and you're not
		// always allow the action if:
		// (A) The source is ulined
		
		
		// firstly, if a ulined nick, or a server, is setting the mode, then allow them to set the mode
		// without any access checks, we're not worthy :p
		if ((ServerInstance->ULine(source->nick.c_str())) || (ServerInstance->ULine(source->server)) || (!*source->server))
			return ACR_ALLOW;

		std::string founder("cm_founder_"+channel->name);
		std::string protect("cm_protect_"+channel->name);

		switch (access_type)
		{
			// a user has been deopped. Do we let them? hmmm...
			case AC_DEOP:
				if (dest->GetExt(founder))
				{
					source->WriteNumeric(484, source->nick+" "+channel->name+" :Can't deop "+dest->nick+" as they're a channel founder");
					return ACR_DENY;
				}
				if ((dest->GetExt(protect)) && (!source->GetExt(protect)))
				{
					source->WriteNumeric(484, source->nick+" "+channel->name+" :Can't deop "+dest->nick+" as they're protected (+a)");
					return ACR_DENY;
				}
			break;

			// a user is being kicked. do we chop off the end of the army boot?
			case AC_KICK:
				if (dest->GetExt(founder))
				{
					source->WriteNumeric(484, source->nick+" "+channel->name+" :Can't kick "+dest->nick+" as they're a channel founder");
					return ACR_DENY;
				}
				if ((dest->GetExt(protect)) && (!source->GetExt(protect)))
				{
					source->WriteNumeric(484, source->nick+" "+channel->name+" :Can't kick "+dest->nick+" as they're protected (+a)");
					return ACR_DENY;
				}
			break;

			// a user is being dehalfopped. Yes, we do disallow -h of a +ha user
			case AC_DEHALFOP:
				if (dest->GetExt(founder))
				{
					source->WriteNumeric(484, source->nick+" "+channel->name+" :Can't de-halfop "+dest->nick+" as they're a channel founder");
					return ACR_DENY;
				}
				if ((dest->GetExt(protect)) && (!source->GetExt(protect)))
				{
					source->WriteNumeric(484, source->nick+" "+channel->name+" :Can't de-halfop "+dest->nick+" as they're protected (+a)");
					return ACR_DENY;
				}
			break;

			// same with devoice.
			case AC_DEVOICE:
				if (dest->GetExt(founder))
				{
					source->WriteNumeric(484, source->nick+" "+channel->name+" :Can't devoice "+dest->nick+" as they're a channel founder");
					return ACR_DENY;
				}
				if ((dest->GetExt(protect)) && (!source->GetExt(protect)))
				{
					source->WriteNumeric(484, source->nick+" "+channel->name+" :Can't devoice "+dest->nick+" as they're protected (+a)");
					return ACR_DENY;
				}
			break;
		}
		
		// we dont know what this access check is, or dont care. just carry on, nothing to see here.
		return ACR_DEFAULT;
	}
	
	virtual ~ModuleChanProtect()
	{
		ServerInstance->Modes->DelMode(cp);
		ServerInstance->Modes->DelMode(cf);
		delete cp;
		delete cf;
	}
	
	virtual Version GetVersion()
	{
		return Version(1, 2, 0, 0, VF_COMMON | VF_VENDOR, API_VERSION);
	}
};

MODULE_INIT(ModuleChanProtect)
