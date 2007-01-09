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
#include "configreader.h"
#include "inspircd.h"
#include "wildcard.h"

/** Holds a users m_safelist state
 */
class ListData : public classbase
{
 public:
	long list_start;
	long list_position;
	bool list_ended;
	const std::string glob;

	ListData() : list_start(0), list_position(0), list_ended(false) {};
	ListData(long pos, time_t t, const std::string &pattern) : list_start(t), list_position(pos), list_ended(false), glob(pattern) {};
};

/* $ModDesc: A module overriding /list, and making it safe - stop those sendq problems. */
 
typedef std::vector<userrec *> UserList;
UserList listusers;    /* vector of people doing a /list */
class ListTimer *timer;

/** To create a timer which recurs every second, we inherit from InspTimer.
 */
class ListTimer : public InspTimer
{
 private:

	char buffer[MAXBUF];
	chanrec *chan;
	InspIRCd* ServerInstance;
	const std::string glob;
	size_t ServerNameSize;

 public:

	ListTimer(InspIRCd* Instance, long interval) : InspTimer(interval,Instance->Time(), true), ServerInstance(Instance)
	{
		ServerNameSize = 4 + strlen(ServerInstance->Config->ServerName);
	}

	virtual void Tick(time_t TIME)
	{
		bool go_again = true;

		while (go_again)
		{
			go_again = false;
			for (UserList::iterator iter = listusers.begin(); iter != listusers.end(); iter++)
			{
				/*
				 * What we do here:
				 *  - Get where they are up to
				 *  - If it's more than total number of channels, erase
				 *    them from the iterator, set go_again to true
				 *  - If not, spool more channels
				 */
				userrec* u = (userrec*)(*iter);
				ListData* ld;
				u->GetExt("safelist_cache", ld);
				if ((size_t)ld->list_position > ServerInstance->chanlist->size())
				{
					u->Shrink("safelist_cache");
					DELETE(ld);
					listusers.erase(iter);
					go_again = true;
					break;
				}

				ServerInstance->Log(DEBUG, "m_safelist.so: resuming spool of list to client %s at channel %ld", u->nick, ld->list_position);
				chan = NULL;
				/* Attempt to fill up to 25% the user's sendq with /LIST output */
				long amount_sent = 0;
				do
				{
					ServerInstance->Log(DEBUG,"Channel %ld",ld->list_position);
					if (!ld->list_position)
						u->WriteServ("321 %s Channel :Users Name",u->nick);
					chan = ServerInstance->GetChannelIndex(ld->list_position);
					/* spool details */
					bool has_user = (chan && chan->HasUser(u));
					if ((chan) && (chan->modes[CM_PRIVATE]))
					{
						bool display = match(chan->name, ld->glob.c_str());
						long users = chan->GetUserCounter();
						if ((users) && (display))
						{
							int counter = snprintf(buffer, MAXBUF, "322 %s *", u->nick);
							amount_sent += counter + ServerNameSize;
							ServerInstance->Log(DEBUG, "m_safelist.so: Sent %ld of safe %ld / 4", amount_sent, u->sendqmax);
							u->WriteServ(std::string(buffer));
						}
					}
					else if ((chan) && (((!(chan->modes[CM_PRIVATE])) && (!(chan->modes[CM_SECRET]))) || (has_user)))
					{
						bool display = match(chan->name, ld->glob.c_str());
						long users = chan->GetUserCounter();

						if ((users) && (display))
						{
							int counter = snprintf(buffer, MAXBUF, "322 %s %s %ld :[+%s] %s",u->nick, chan->name, users, chan->ChanModes(has_user), chan->topic);
							/* Increment total plus linefeed */
							amount_sent += counter + ServerNameSize;
							ServerInstance->Log(DEBUG, "m_safelist.so: Sent %ld of safe %ld / 4", amount_sent, u->sendqmax);
							u->WriteServ(std::string(buffer));
						}
					}
					else
					{
						if (!chan)
						{
							if (!ld->list_ended)
							{
								ld->list_ended = true;
								u->WriteServ("323 %s :End of channel list.",u->nick);
							}
						}
					}

					ld->list_position++;
				}
				while ((chan != NULL) && (amount_sent < (u->sendqmax / 4)));
			}
		}

		if (!listusers.size())
		{
			timer = NULL;
		}
	}
};

class ModuleSafeList : public Module
{
 public:
	ModuleSafeList(InspIRCd* Me) : Module::Module(Me)
	{
		timer = NULL;
	}
 
	virtual ~ModuleSafeList()
	{
		if (timer)
			ServerInstance->Timers->DelTimer(timer);
	}
 
	virtual Version GetVersion()
	{
		return Version(1,1,0,0,VF_VENDOR,API_VERSION);
	}
 
	void Implements(char* List)
	{
		List[I_OnPreCommand] = List[I_OnCleanup] = List[I_OnUserQuit] = List[I_On005Numeric] = 1;
	}

	/*
	 * OnPreCommand()
	 *   Intercept the LIST command.
	 */ 
	virtual int OnPreCommand(const std::string &command, const char** parameters, int pcnt, userrec *user, bool validated, const std::string &original_line)
	{
		/* If the command doesnt appear to be valid, we dont want to mess with it. */
		if (!validated)
			return 0;
 
		if (command == "LIST")
		{
			return this->HandleList(parameters, pcnt, user);
		}
		return 0;
	}
	
	/*
	 * HandleList()
	 *   Handle (override) the LIST command.
	 */
	int HandleList(const char** parameters, int pcnt, userrec* user)
	{
		/* First, let's check if the user is currently /list'ing */
		ListData *ld;
		user->GetExt("safelist_cache", ld);
 
		if (ld)
		{
			/* user is already /list'ing, we don't want to do shit. */
			return 1;
		}

		/* Work around mIRC suckyness. YOU SUCK, KHALED! */
		if ((pcnt == 1) && (*parameters[0] == '<'))
			pcnt = 0;

		time_t* last_list_time;
		user->GetExt("safelist_last", last_list_time);
		if (last_list_time)
		{
			if (ServerInstance->Time() < (*last_list_time)+60)
			{
				user->WriteServ("NOTICE %s :*** Woah there, slow down a little, you can't /LIST so often!",user->nick);
				user->WriteServ("321 %s Channel :Users Name",user->nick);
				user->WriteServ("323 %s :End of channel list.",user->nick);
				return 1;
			}

			DELETE(last_list_time);
			user->Shrink("safelist_last");
		}
 
		/*
		 * start at channel 0! ;)
		 */
		ld = new ListData(0,ServerInstance->Time(), pcnt ? parameters[0] : "*");
		user->Extend("safelist_cache", ld);
		listusers.push_back(user);

		time_t* llt = new time_t;
		*llt = ServerInstance->Time();
		user->Extend("safelist_last", llt);

		if (!timer)
		{
			timer = new ListTimer(ServerInstance,1);
			ServerInstance->Timers->AddTimer(timer);
		}

		return 1;
	}

	virtual void OnCleanup(int target_type, void* item)
	{
		if(target_type == TYPE_USER)
		{
			userrec* u = (userrec*)item;
			ListData* ld;
			u->GetExt("safelist_cache", ld);
			if (ld)
			{
				u->Shrink("safelist_cache");
				DELETE(ld);
			}
			for (UserList::iterator iter = listusers.begin(); iter != listusers.end(); iter++)
			{
				userrec* u2 = (userrec*)(*iter);
				if (u2 == u)
				{
					listusers.erase(iter);
					break;
				}
			}
			time_t* last_list_time;
			u->GetExt("safelist_last", last_list_time);
			if (last_list_time)
			{
				DELETE(last_list_time);
				u->Shrink("safelist_last");
			}
		}
	}

	virtual void On005Numeric(std::string &output)
	{
		output.append(" SAFELIST");
	}

	virtual void OnUserQuit(userrec* user, const std::string &message)
	{
		this->OnCleanup(TYPE_USER,user);
	}

};


class ModuleSafeListFactory : public ModuleFactory
{
 public:
	ModuleSafeListFactory()
	{
	}
 
	~ModuleSafeListFactory()
	{
	}
 
	virtual Module * CreateModule(InspIRCd* Me)
	{
		return new ModuleSafeList(Me);
	}
 
};
 
extern "C" void * init_module( void )
{
	return new ModuleSafeListFactory;
}
