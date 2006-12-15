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

/* $ModDesc: Provides support for RFC 1413 ident lookups */

// Version 1.5.0.0 - Updated to use InspSocket, faster and neater.

/** Handles RFC1413 ident connections to users
 */
class RFC1413 : public InspSocket
{
 protected:
			 // Server* class used for core communications
	insp_sockaddr sock_us;	 // our port number
	insp_sockaddr sock_them; // their port number
	socklen_t uslen;	 // length of our port number
	socklen_t themlen;	 // length of their port number
	char ident_request[128]; // buffer used to make up the request string
 public:

	userrec* u;		 // user record that the lookup is associated with
	int ufd;

	RFC1413(InspIRCd* SI, userrec* user, int maxtime) : InspSocket(SI, user->GetIPString(), 113, false, maxtime), u(user)
	{
		ufd = user->GetFd();
	}

	virtual void OnTimeout()
	{
		// When we timeout, the connection failed within the allowed timeframe,
		// so we just display a notice, and tidy off the ident_data.
		if (u && (Instance->SE->GetRef(ufd) == u))
		{
			char newident[MAXBUF];
			u->Shrink("ident_data");
			u->WriteServ("NOTICE "+std::string(u->nick)+" :*** Could not find your ident, using ~"+std::string(u->ident)+" instead.");
			strcpy(newident,"~");
			strlcat(newident,u->ident,IDENTMAX);
			strlcpy(u->ident,newident,IDENTMAX);
		}
	}

	virtual bool OnDataReady()
	{
		char* ibuf = this->Read();
		if (ibuf)
		{
			char* savept;
			char* section = strtok_r(ibuf,":",&savept);
			while (section)
			{
				if (strstr(section,"USERID"))
				{
					section = strtok_r(NULL,":",&savept);
					if (section)
					{
						// ID type, usually UNIX or OTHER... we dont want it, so read the next token
						section = strtok_r(NULL,":",&savept);
						if (section)
						{
							while (*section == ' ') section++; // strip leading spaces
							for (char* j = section; *j; j++)
							if ((*j < 33) || (*j > 126))
								*j = '\0'; // truncate at invalid chars
							if (*section)
							{
								if (u && (Instance->SE->GetRef(ufd) == u))
								{
									if (this->Instance->IsIdent(section))
									{
										strlcpy(u->ident,section,IDENTMAX);
										Instance->Log(DEBUG,"IDENT SET: "+std::string(u->ident));
										u->WriteServ("NOTICE "+std::string(u->nick)+" :*** Found your ident: "+std::string(u->ident));
									}
								}
							}
							return false;
						}
					}
				}
				section = strtok_r(NULL,":",&savept);
			}
		}
		return false;
	}

	virtual void OnClose()
	{
		// tidy up after ourselves when the connection is done.
		// We receive this event straight after a timeout, too.
		//
		//
		// OK, now listen up. The weird looking check here is
		// REQUIRED. Don't try and optimize it away.
		//
		// When a socket is closed, it is not immediately removed
		// from the socket list, there can be a short delay
		// before it is culled from the list. This means that
		// without this check, there is a chance that a user
		// may not exist when we come to ::Shrink them, which
		// results in a segfault. The value of "u" may not
		// always be NULL at this point, so, what we do is
		// check against the fd_ref_table, to see if (1) the user
		// exists, and (2) its the SAME user, on the same file
		// descriptor that they were when the lookup began.
		//
		// Fixes issue reported by webs, 7 Jun 2006
		if (u && (Instance->SE->GetRef(ufd) == u))
		{
			u->Shrink("ident_data");
		}
	}

	virtual void OnError(InspSocketError e)
	{
		if (u && (Instance->SE->GetRef(ufd) == u))
		{
			u->Shrink("ident_data");
		}
	}

	virtual bool OnConnected()
	{
		Instance->Log(DEBUG,"Ident: connected");
		if (u && (Instance->SE->GetRef(ufd) == u))
		{
			uslen = sizeof(sock_us);
			themlen = sizeof(sock_them);
			if ((getsockname(this->u->GetFd(),(sockaddr*)&sock_us,&uslen) || getpeername(this->u->GetFd(), (sockaddr*)&sock_them, &themlen)))
			{
				Instance->Log(DEBUG,"Ident: failed to get socket names, bailing");
				return false;
			}
			else
			{
				// send the request in the following format: theirsocket,oursocket
#ifdef IPV6
				snprintf(ident_request,127,"%d,%d\r\n",ntohs(sock_them.sin6_port),ntohs(sock_us.sin6_port));
#else
				snprintf(ident_request,127,"%d,%d\r\n",ntohs(sock_them.sin_port),ntohs(sock_us.sin_port));
#endif
				this->Write(ident_request);
				Instance->Log(DEBUG,"Sent ident request, waiting for reply");
				return true;
			}
		}
		else
		{
			return true;
		}
	}
};

class ModuleIdent : public Module
{

	ConfigReader* Conf;
	
	int IdentTimeout;

 public:
	void ReadSettings()
	{
		Conf = new ConfigReader(ServerInstance);
		IdentTimeout = Conf->ReadInteger("ident","timeout",0,true);
		if (!IdentTimeout)
			IdentTimeout = 1;
		DELETE(Conf);
	}

	ModuleIdent(InspIRCd* Me)
		: Module::Module(Me)
	{
		
		ReadSettings();
	}

	void Implements(char* List)
	{
		List[I_OnCleanup] = List[I_OnRehash] = List[I_OnUserRegister] = List[I_OnCheckReady] = List[I_OnUserDisconnect] = 1;
	}

	virtual void OnRehash(const std::string &parameter)
	{
		ReadSettings();
	}

	virtual int OnUserRegister(userrec* user)
	{
		/*
		 * when the new user connects, before they authenticate with USER/NICK/PASS, we do
		 * their ident lookup. We do this by instantiating an object of type RFC1413, which
		 * is derived from InspSocket, and inserting it into the socket engine using the
		 * Server::AddSocket() call.
		 */
		user->WriteServ("NOTICE "+std::string(user->nick)+" :*** Looking up your ident...");
		RFC1413* ident = new RFC1413(ServerInstance, user, IdentTimeout);
		if ((ident->GetState() == I_CONNECTING) || (ident->GetState() == I_CONNECTED))
		{
			user->Extend("ident_data", (char*)ident);
		}
		else
		{
			char newident[MAXBUF];
			user->WriteServ("NOTICE "+std::string(user->nick)+" :*** Could not find your ident, using ~"+std::string(user->ident)+" instead.");
			strcpy(newident,"~");
			strlcat(newident,user->ident,IDENTMAX);
			strlcpy(user->ident,newident,IDENTMAX);
			delete ident;
		}
		return 0;
	}

	virtual bool OnCheckReady(userrec* user)
	{
		/*
		 * The socket engine will clean up their ident request for us when it completes,
		 * either due to timeout or due to closing, so, we just hold them until they dont
		 * have an ident field any more.
		 */
		RFC1413* ident;
		return (!user->GetExt("ident_data", ident));
	}

	virtual void OnCleanup(int target_type, void* item)
	{
		if (target_type == TYPE_USER)
		{
			userrec* user = (userrec*)item;
			RFC1413* ident;
			if (user->GetExt("ident_data", ident))
			{
				// FIX: If the user record is deleted, the socket wont be removed
				// immediately so there is chance of the socket trying to write to
				// a user which has now vanished! To prevent this, set ident::u
				// to NULL and check it so that we dont write users who have gone away.
				ident->u = NULL;
				ServerInstance->SE->DelFd(ident);
				delete ident;
			}
		}
	}

	virtual void OnUserDisconnect(userrec* user)
	{
		/*
		 * when the user quits tidy up any ident lookup they have pending to keep things tidy.
		 * When we call RemoveSocket, the abstractions tied into the system evnetually work their
		 * way to RFC1459::OnClose(), which shrinks off the ident_data for us, so we dont need
		 * to do it here. If we don't tidy this up, there may still be lingering idents for users
		 * who have quit, as class RFC1459 is only loosely bound to userrec* via a pair of pointers
		 * and this would leave at least one of the invalid ;)
		 */
		RFC1413* ident;
		if (user->GetExt("ident_data", ident))
		{
			ident->u = NULL;
			ServerInstance->SE->DelFd(ident);
			delete ident;
		}
	}
	
	virtual ~ModuleIdent()
	{
	}
	
	virtual Version GetVersion()
	{
		return Version(1,1,0,0,VF_VENDOR,API_VERSION);
	}
	
};

class ModuleIdentFactory : public ModuleFactory
{
 public:
	ModuleIdentFactory()
	{
	}
	
	~ModuleIdentFactory()
	{
	}
	
	virtual Module * CreateModule(InspIRCd* Me)
	{
		return new ModuleIdent(Me);
	}
	
};


extern "C" void * init_module( void )
{
	return new ModuleIdentFactory;
}

