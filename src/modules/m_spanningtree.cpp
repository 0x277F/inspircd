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

/* $ModDesc: Povides a spanning tree server link protocol */

#include "configreader.h"
#include "users.h"
#include "channels.h"
#include "modules.h"
#include "commands/cmd_whois.h"
#include "commands/cmd_stats.h"
#include "socket.h"
#include "inspircd.h"
#include "wildcard.h"
#include "xline.h"
#include "transport.h"

/** If you make a change which breaks the protocol, increment this.
 * If you  completely change the protocol, completely change the number.
 *
 * IMPORTANT: If you make changes, document your changes here, without fail:
 * http://www.inspircd.org/wiki/List_of_protocol_changes_between_versions
 *
 * Failure to document your protocol changes will result in a painfully
 * painful death by pain. You have been warned.
 */
const long ProtocolVersion = 1103;

/*
 * The server list in InspIRCd is maintained as two structures
 * which hold the data in different ways. Most of the time, we
 * want to very quicky obtain three pieces of information:
 *
 * (1) The information on a server
 * (2) The information on the server we must send data through
 *     to actually REACH the server we're after
 * (3) Potentially, the child/parent objects of this server
 *
 * The InspIRCd spanning protocol provides easy access to these
 * by storing the data firstly in a recursive structure, where
 * each item references its parent item, and a dynamic list
 * of child items, and another structure which stores the items
 * hashed, linearly. This means that if we want to find a server
 * by name quickly, we can look it up in the hash, avoiding
 * any O(n) lookups. If however, during a split or sync, we want
 * to apply an operation to a server, and any of its child objects
 * we can resort to recursion to walk the tree structure.
 * Any socket can have one of five states at any one time.
 * The LISTENER state indicates a socket which is listening
 * for connections. It cannot receive data itself, only incoming
 * sockets.
 * The CONNECTING state indicates an outbound socket which is
 * waiting to be writeable.
 * The WAIT_AUTH_1 state indicates the socket is outbound and
 * has successfully connected, but has not yet sent and received
 * SERVER strings.
 * The WAIT_AUTH_2 state indicates that the socket is inbound
 * (allocated by a LISTENER) but has not yet sent and received
 * SERVER strings.
 * The CONNECTED state represents a fully authorized, fully
 * connected server.
 */
enum ServerState { LISTENER, CONNECTING, WAIT_AUTH_1, WAIT_AUTH_2, CONNECTED };

/* Foward declarations */
class TreeServer;
class TreeSocket;
class Link;
class ModuleSpanningTree;
class SpanningTreeUtilities;

/* This hash_map holds the hash equivalent of the server
 * tree, used for rapid linear lookups.
 */
typedef nspace::hash_map<std::string, TreeServer*, nspace::hash<string>, irc::StrHashComp> server_hash;

typedef std::map<TreeServer*,TreeServer*> TreeServerList;

/** A group of modules that implement InspSocketHook
 * that we can use to hook our server to server connections.
 */
typedef std::map<irc::string, Module*> hookmodules;

/** The Link class might as well be a struct,
 * but this is C++ and we don't believe in structs (!).
 * It holds the entire information of one <link>
 * tag from the main config file. We maintain a list
 * of them, and populate the list on rehash/load.
 */
class Link : public classbase
{
 public:
	irc::string Name;
	std::string IPAddr;
	int Port;
	std::string SendPass;
	std::string RecvPass;
	unsigned long AutoConnect;
	time_t NextConnectTime;
	bool HiddenFromStats;
	std::string FailOver;
	std::string Hook;
	int Timeout;
};

class HandshakeTimer : public InspTimer
{
 private:
	InspIRCd* Instance;
	TreeSocket* sock;
	Link* lnk;
	SpanningTreeUtilities* Utils;
	int thefd;
 public:
	HandshakeTimer(InspIRCd* Inst, TreeSocket* s, Link* l, SpanningTreeUtilities* u);
	virtual void Tick(time_t TIME);
};


/** Contains helper functions and variables for this module,
 * and keeps them out of the global namespace
 */
class SpanningTreeUtilities
{
 private:
	/** Creator server
	 */
	InspIRCd* ServerInstance;
 public:
	/** Creator module
	 */
	ModuleSpanningTree* Creator;
	/** Flatten links and /MAP for non-opers
	 */
	bool FlatLinks;
	/** Hide U-Lined servers in /MAP and /LINKS
	 */
	bool HideULines;
	/** Announce TS changes to channels on merge
	 */
	bool AnnounceTSChange;
	/** Synchronize timestamps between servers
	 */
	bool EnableTimeSync;
	/** Socket bindings for listening sockets
	 */
	std::vector<TreeSocket*> Bindings;
	/** This variable represents the root of the server tree
	 */
	TreeServer *TreeRoot;
	/** IPs allowed to link to us
	 */
	std::vector<std::string> ValidIPs;
	/** Hash of currently connected servers by name
	 */
	server_hash serverlist;
	/** Holds the data from the <link> tags in the conf
	 */
	std::vector<Link> LinkBlocks;

	hookmodules hooks;
	std::vector<std::string> hooknames;

	/** Initialise utility class
	 */
	SpanningTreeUtilities(InspIRCd* Instance, ModuleSpanningTree* Creator);
	/** Destroy class and free listeners etc
	 */
	~SpanningTreeUtilities();
	/** Send a message from this server to one other local or remote
	 */
	bool DoOneToOne(const std::string &prefix, const std::string &command, std::deque<std::string> &params, std::string target);
	/** Send a message from this server to all but one other, local or remote
	 */
	bool DoOneToAllButSender(const std::string &prefix, const std::string &command, std::deque<std::string> &params, std::string omit);
	/** Send a message from this server to all but one other, local or remote
	 */
	bool DoOneToAllButSender(const char* prefix, const char* command, std::deque<std::string> &params, std::string omit);
	/** Send a message from this server to all others
	 */
	bool DoOneToMany(const std::string &prefix, const std::string &command, std::deque<std::string> &params);
	/** Send a message from this server to all others
	 */
	bool DoOneToMany(const char* prefix, const char* command, std::deque<std::string> &params);
	/** Send a message from this server to all others, without doing any processing on the command (e.g. send it as-is with colons and all)
	 */
	bool DoOneToAllButSenderRaw(const std::string &data, const std::string &omit, const std::string &prefix, const irc::string &command, std::deque<std::string> &params);
	/** Read the spanningtree module's tags from the config file
	 */
	void ReadConfiguration(bool rebind);
	/** Add a server to the server list for GetListOfServersForChannel
	 */
	void AddThisServer(TreeServer* server, TreeServerList &list);
	/** Compile a list of servers which contain members of channel c
	 */
	void GetListOfServersForChannel(chanrec* c, TreeServerList &list, char status, const CUList &exempt_list);
	/** Find a server by name
	 */
	TreeServer* FindServer(const std::string &ServerName);
	/** Find a route to a server by name
	 */
	TreeServer* BestRouteTo(const std::string &ServerName);
	/** Find a server by glob mask
	 */
	TreeServer* FindServerMask(const std::string &ServerName);
	/** Returns true if this is a server name we recognise
	 */
	bool IsServer(const std::string &ServerName);
	/** Attempt to connect to the failover link of link x
	 */
	void DoFailOver(Link* x);
	/** Find a link tag from a server name
	 */
	Link* FindLink(const std::string& name);
};


/** Each server in the tree is represented by one class of
 * type TreeServer. A locally connected TreeServer can
 * have a class of type TreeSocket associated with it, for
 * remote servers, the TreeSocket entry will be NULL.
 * Each server also maintains a pointer to its parent
 * (NULL if this server is ours, at the top of the tree)
 * and a pointer to its "Route" (see the comments in the
 * constructors below), and also a dynamic list of pointers
 * to its children which can be iterated recursively
 * if required. Creating or deleting objects of type
 i* TreeServer automatically maintains the hash_map of
 * TreeServer items, deleting and inserting them as they
 * are created and destroyed.
 */
class TreeServer : public classbase
{
	InspIRCd* ServerInstance;		/* Creator */
	TreeServer* Parent;			/* Parent entry */
	TreeServer* Route;			/* Route entry */
	std::vector<TreeServer*> Children;	/* List of child objects */
	irc::string ServerName;			/* Server's name */
	std::string ServerDesc;			/* Server's description */
	std::string VersionString;		/* Version string or empty string */
	int UserCount;				/* Not used in this version */
	int OperCount;				/* Not used in this version */
	TreeSocket* Socket;			/* For directly connected servers this points at the socket object */
	time_t NextPing;			/* After this time, the server should be PINGed*/
	bool LastPingWasGood;			/* True if the server responded to the last PING with a PONG */
	SpanningTreeUtilities* Utils;		/* Utility class */
	
 public:

	/** We don't use this constructor. Its a dummy, and won't cause any insertion
	 * of the TreeServer into the hash_map. See below for the two we DO use.
	 */
	TreeServer(SpanningTreeUtilities* Util, InspIRCd* Instance) : ServerInstance(Instance), Utils(Util)
	{
		Parent = NULL;
		ServerName = "";
		ServerDesc = "";
		VersionString = "";
		UserCount = OperCount = 0;
		VersionString = ServerInstance->GetVersionString();
	}

	/** We use this constructor only to create the 'root' item, Utils->TreeRoot, which
	 * represents our own server. Therefore, it has no route, no parent, and
	 * no socket associated with it. Its version string is our own local version.
	 */
	TreeServer(SpanningTreeUtilities* Util, InspIRCd* Instance, std::string Name, std::string Desc) : ServerInstance(Instance), ServerName(Name.c_str()), ServerDesc(Desc), Utils(Util)
	{
		Parent = NULL;
		VersionString = "";
		UserCount = ServerInstance->UserCount();
		OperCount = ServerInstance->OperCount();
		VersionString = ServerInstance->GetVersionString();
		Route = NULL;
		Socket = NULL; /* Fix by brain */
		AddHashEntry();
	}

	/** When we create a new server, we call this constructor to initialize it.
	 * This constructor initializes the server's Route and Parent, and sets up
	 * its ping counters so that it will be pinged one minute from now.
	 */
	TreeServer(SpanningTreeUtilities* Util, InspIRCd* Instance, std::string Name, std::string Desc, TreeServer* Above, TreeSocket* Sock)
		: ServerInstance(Instance), Parent(Above), ServerName(Name.c_str()), ServerDesc(Desc), Socket(Sock), Utils(Util)
	{
		VersionString = "";
		UserCount = OperCount = 0;
		this->SetNextPingTime(time(NULL) + 60);
		this->SetPingFlag();

		/* find the 'route' for this server (e.g. the one directly connected
		 * to the local server, which we can use to reach it)
		 *
		 * In the following example, consider we have just added a TreeServer
		 * class for server G on our network, of which we are server A.
		 * To route traffic to G (marked with a *) we must send the data to
		 * B (marked with a +) so this algorithm initializes the 'Route'
		 * value to point at whichever server traffic must be routed through
		 * to get here. If we were to try this algorithm with server B,
		 * the Route pointer would point at its own object ('this').
		 *
		 *              A
		 *             / \
		 *          + B   C
		 *           / \   \
		 *          D   E   F
		 *         /         \
		 *      * G           H
		 *
		 * We only run this algorithm when a server is created, as
		 * the routes remain constant while ever the server exists, and
		 * do not need to be re-calculated.
		 */

		Route = Above;
		if (Route == Utils->TreeRoot)
		{
			Route = this;
		}
		else
		{
			while (this->Route->GetParent() != Utils->TreeRoot)
			{
				this->Route = Route->GetParent();
			}
		}

		/* Because recursive code is slow and takes a lot of resources,
		 * we store two representations of the server tree. The first
		 * is a recursive structure where each server references its
		 * children and its parent, which is used for netbursts and
		 * netsplits to dump the whole dataset to the other server,
		 * and the second is used for very fast lookups when routing
		 * messages and is instead a hash_map, where each item can
		 * be referenced by its server name. The AddHashEntry()
		 * call below automatically inserts each TreeServer class
		 * into the hash_map as it is created. There is a similar
		 * maintainance call in the destructor to tidy up deleted
		 * servers.
		 */

		this->AddHashEntry();
	}

	int QuitUsers(const std::string &reason)
	{
		ServerInstance->Log(DEBUG,"Removing all users from server %s",this->ServerName.c_str());
		const char* reason_s = reason.c_str();
		std::vector<userrec*> time_to_die;
		for (user_hash::iterator n = ServerInstance->clientlist->begin(); n != ServerInstance->clientlist->end(); n++)
		{
			if (!strcmp(n->second->server, this->ServerName.c_str()))
			{
				time_to_die.push_back(n->second);
			}
		}
		for (std::vector<userrec*>::iterator n = time_to_die.begin(); n != time_to_die.end(); n++)
		{
			userrec* a = (userrec*)*n;
			ServerInstance->Log(DEBUG,"Kill %s fd=%d",a->nick,a->GetFd());
			if (!IS_LOCAL(a))
				userrec::QuitUser(ServerInstance,a,reason_s);
		}
		return time_to_die.size();
	}

	/** This method is used to add the structure to the
	 * hash_map for linear searches. It is only called
	 * by the constructors.
	 */
	void AddHashEntry()
	{
		server_hash::iterator iter = Utils->serverlist.find(this->ServerName.c_str());
		if (iter == Utils->serverlist.end())
			Utils->serverlist[this->ServerName.c_str()] = this;
	}

	/** This method removes the reference to this object
	 * from the hash_map which is used for linear searches.
	 * It is only called by the default destructor.
	 */
	void DelHashEntry()
	{
		server_hash::iterator iter = Utils->serverlist.find(this->ServerName.c_str());
		if (iter != Utils->serverlist.end())
			Utils->serverlist.erase(iter);
	}

	/** These accessors etc should be pretty self-
	 * explanitory.
	 */
	TreeServer* GetRoute()
	{
		return Route;
	}

	std::string GetName()
	{
		return ServerName.c_str();
	}

	std::string GetDesc()
	{
		return ServerDesc;
	}

	std::string GetVersion()
	{
		return VersionString;
	}

	void SetNextPingTime(time_t t)
	{
		this->NextPing = t;
		LastPingWasGood = false;
	}

	time_t NextPingTime()
	{
		return NextPing;
	}

	bool AnsweredLastPing()
	{
		return LastPingWasGood;
	}

	void SetPingFlag()
	{
		LastPingWasGood = true;
	}

	int GetUserCount()
	{
		return UserCount;
	}

	void AddUserCount()
	{
		UserCount++;
	}

	void DelUserCount()
	{
		UserCount--;
	}

	int GetOperCount()
	{
		return OperCount;
	}

	TreeSocket* GetSocket()
	{
		return Socket;
	}

	TreeServer* GetParent()
	{
		return Parent;
	}

	void SetVersion(const std::string &Version)
	{
		VersionString = Version;
	}

	unsigned int ChildCount()
	{
		return Children.size();
	}

	TreeServer* GetChild(unsigned int n)
	{
		if (n < Children.size())
		{
			/* Make sure they  cant request
			 * an out-of-range object. After
			 * all we know what these programmer
			 * types are like *grin*.
			 */
			return Children[n];
		}
		else
		{
			return NULL;
		}
	}

	void AddChild(TreeServer* Child)
	{
		Children.push_back(Child);
	}

	bool DelChild(TreeServer* Child)
	{
		for (std::vector<TreeServer*>::iterator a = Children.begin(); a < Children.end(); a++)
		{
			if (*a == Child)
			{
				Children.erase(a);
				return true;
			}
		}
		return false;
	}

	/** Removes child nodes of this node, and of that node, etc etc.
	 * This is used during netsplits to automatically tidy up the
	 * server tree. It is slow, we don't use it for much else.
	 */
	bool Tidy()
	{
		bool stillchildren = true;
		while (stillchildren)
		{
			stillchildren = false;
			for (std::vector<TreeServer*>::iterator a = Children.begin(); a < Children.end(); a++)
			{
				TreeServer* s = (TreeServer*)*a;
				s->Tidy();
				Children.erase(a);
				DELETE(s);
				stillchildren = true;
				break;
			}
		}
		return true;
	}

	~TreeServer()
	{
		/* We'd better tidy up after ourselves, eh? */
		this->DelHashEntry();
	}
};

/** Yay for fast searches!
 * This is hundreds of times faster than recursion
 * or even scanning a linked list, especially when
 * there are more than a few servers to deal with.
 * (read as: lots).
 */
TreeServer* SpanningTreeUtilities::FindServer(const std::string &ServerName)
{
	server_hash::iterator iter;
	iter = serverlist.find(ServerName.c_str());
	if (iter != serverlist.end())
	{
		return iter->second;
	}
	else
	{
		return NULL;
	}
}

/** Returns the locally connected server we must route a
 * message through to reach server 'ServerName'. This
 * only applies to one-to-one and not one-to-many routing.
 * See the comments for the constructor of TreeServer
 * for more details.
 */
TreeServer* SpanningTreeUtilities::BestRouteTo(const std::string &ServerName)
{
	if (ServerName.c_str() == TreeRoot->GetName())
		return NULL;
	TreeServer* Found = FindServer(ServerName);
	if (Found)
	{
		return Found->GetRoute();
	}
	else
	{
		return NULL;
	}
}

/** Find the first server matching a given glob mask.
 * Theres no find-using-glob method of hash_map [awwww :-(]
 * so instead, we iterate over the list using an iterator
 * and match each one until we get a hit. Yes its slow,
 * deal with it.
 */
TreeServer* SpanningTreeUtilities::FindServerMask(const std::string &ServerName)
{
	for (server_hash::iterator i = serverlist.begin(); i != serverlist.end(); i++)
	{
		if (match(i->first.c_str(),ServerName.c_str()))
			return i->second;
	}
	return NULL;
}

/* A convenient wrapper that returns true if a server exists */
bool SpanningTreeUtilities::IsServer(const std::string &ServerName)
{
	return (FindServer(ServerName) != NULL);
}


/** Handle /RCONNECT
 */
class cmd_rconnect : public command_t
{
	Module* Creator;
	SpanningTreeUtilities* Utils;
 public:
	cmd_rconnect (InspIRCd* Instance, Module* Callback, SpanningTreeUtilities* Util) : command_t(Instance, "RCONNECT", 'o', 2), Creator(Callback), Utils(Util)
	{
		this->source = "m_spanningtree.so";
		syntax = "<remote-server-mask> <target-server-mask>";
	}

	CmdResult Handle (const char** parameters, int pcnt, userrec *user)
	{
		if (IS_LOCAL(user))
		{
			if (!Utils->FindServer(parameters[0]))
			{
				user->WriteServ("NOTICE %s :*** RCONNECT: Server \002%s\002 isn't connected to the network!", user->nick, parameters[0]);
				return CMD_FAILURE;
			}
			
			user->WriteServ("NOTICE %s :*** RCONNECT: Sending remote connect to \002%s\002 to connect server \002%s\002.",user->nick,parameters[0],parameters[1]);
		}
		
		/* Is this aimed at our server? */
		if (ServerInstance->MatchText(ServerInstance->Config->ServerName,parameters[0]))
		{
			/* Yes, initiate the given connect */
			ServerInstance->SNO->WriteToSnoMask('l',"Remote CONNECT from %s matching \002%s\002, connecting server \002%s\002",user->nick,parameters[0],parameters[1]);
			const char* para[1];
			para[0] = parameters[1];
			std::string original_command = std::string("CONNECT ") + parameters[1];
			Creator->OnPreCommand("CONNECT", para, 1, user, true, original_command);
		}
		
		return CMD_SUCCESS;
	}
};
 


/** Every SERVER connection inbound or outbound is represented by
 * an object of type TreeSocket.
 * TreeSockets, being inherited from InspSocket, can be tied into
 * the core socket engine, and we cn therefore receive activity events
 * for them, just like activex objects on speed. (yes really, that
 * is a technical term!) Each of these which relates to a locally
 * connected server is assocated with it, by hooking it onto a
 * TreeSocket class using its constructor. In this way, we can
 * maintain a list of servers, some of which are directly connected,
 * some of which are not.
 */
class TreeSocket : public InspSocket
{
	SpanningTreeUtilities* Utils;
	std::string myhost;
	std::string in_buffer;
	ServerState LinkState;
	std::string InboundServerName;
	std::string InboundDescription;
	int num_lost_users;
	int num_lost_servers;
	time_t NextPing;
	bool LastPingWasGood;
	bool bursting;
	unsigned int keylength;
	std::string ModuleList;
	std::map<std::string,std::string> CapKeys;
	Module* Hook;

 public:

	/** Because most of the I/O gubbins are encapsulated within
	 * InspSocket, we just call the superclass constructor for
	 * most of the action, and append a few of our own values
	 * to it.
	 */
	TreeSocket(SpanningTreeUtilities* Util, InspIRCd* SI, std::string host, int port, bool listening, unsigned long maxtime, Module* HookMod = NULL)
		: InspSocket(SI, host, port, listening, maxtime), Utils(Util), Hook(HookMod)
	{
		myhost = host;
		this->LinkState = LISTENER;

		Instance->Log(DEBUG, "HOOK = %08x", Hook);

		if (listening && Hook)
			InspSocketHookRequest(this, (Module*)Utils->Creator, Hook).Send();
	}

	TreeSocket(SpanningTreeUtilities* Util, InspIRCd* SI, std::string host, int port, bool listening, unsigned long maxtime, std::string ServerName, Module* HookMod = NULL)
		: InspSocket(SI, host, port, listening, maxtime), Utils(Util), Hook(HookMod)
	{
		myhost = ServerName;
		this->LinkState = CONNECTING;

		if (Hook)
			InspSocketHookRequest(this, (Module*)Utils->Creator, Hook).Send();
	}

	/** When a listening socket gives us a new file descriptor,
	 * we must associate it with a socket without creating a new
	 * connection. This constructor is used for this purpose.
	 */
	TreeSocket(SpanningTreeUtilities* Util, InspIRCd* SI, int newfd, char* ip, Module* HookMod = NULL)
		: InspSocket(SI, newfd, ip), Utils(Util), Hook(HookMod)
	{
		this->LinkState = WAIT_AUTH_1;

		/* If we have a transport module hooked to the parent, hook the same module to this
		 * socket, and set a timer waiting for handshake before we send CAPAB etc.
		 */
		if (Hook)
		{
			InspSocketHookRequest(this, (Module*)Utils->Creator, Hook).Send();
			Instance->Timers->AddTimer(new HandshakeTimer(Instance, this, &(Utils->LinkBlocks[0]), this->Utils));
		}
		else
		{
			/* Otherwise, theres no lower layer transport in plain TCP/IP,
			 * so just send the capabilities right now.
			 */
			this->SendCapabilities();
		}
	}

	ServerState GetLinkState()
	{
		return this->LinkState;
	}

	Module* GetHook()
	{
		return this->Hook;
	}

	~TreeSocket()
	{
		if (Hook)
			InspSocketUnhookRequest(this, (Module*)Utils->Creator, Hook).Send();
	}
	
	/** When an outbound connection finishes connecting, we receive
	 * this event, and must send our SERVER string to the other
	 * side. If the other side is happy, as outlined in the server
	 * to server docs on the inspircd.org site, the other side
	 * will then send back its own server string.
	 */
	virtual bool OnConnected()
	{
		if (this->LinkState == CONNECTING)
		{
			/* we do not need to change state here. */
			for (std::vector<Link>::iterator x = Utils->LinkBlocks.begin(); x < Utils->LinkBlocks.end(); x++)
			{
				if (x->Name == this->myhost)
				{
					this->Instance->SNO->WriteToSnoMask('l',"Connection to \2"+myhost+"\2["+(x->HiddenFromStats ? "<hidden>" : this->GetIP())+"] started.");

					if (Hook)
					{
						InspSocketHookRequest(this, (Module*)Utils->Creator, Hook).Send();
						this->Instance->SNO->WriteToSnoMask('l',"Connection to \2"+myhost+"\2["+(x->HiddenFromStats ? "<hidden>" : this->GetIP())+"] using transport \2"+x->Hook+"\2");
					}
					else
						this->SendCapabilities();

					/* found who we're supposed to be connecting to, send the neccessary gubbins. */
					if (Hook)
						Instance->Timers->AddTimer(new HandshakeTimer(Instance, this, &(*x), this->Utils));
					else
						this->WriteLine(std::string("SERVER ")+this->Instance->Config->ServerName+" "+x->SendPass+" 0 :"+this->Instance->Config->ServerDesc);

					return true;
				}
			}
		}
		/* There is a (remote) chance that between the /CONNECT and the connection
		 * being accepted, some muppet has removed the <link> block and rehashed.
		 * If that happens the connection hangs here until it's closed. Unlikely
		 * and rather harmless.
		 */
		this->Instance->SNO->WriteToSnoMask('l',"Connection to \2"+myhost+"\2 lost link tag(!)");
		return true;
	}
	
	virtual void OnError(InspSocketError e)
	{
		/* We don't handle this method, because all our
		 * dirty work is done in OnClose() (see below)
		 * which is still called on error conditions too.
		 */
		if (e == I_ERR_CONNECT)
		{
			this->Instance->SNO->WriteToSnoMask('l',"Connection failed: Connection to \002"+myhost+"\002 refused");
			Link* MyLink = Utils->FindLink(myhost);
			if (MyLink)
				Utils->DoFailOver(MyLink);
		}
	}

	virtual int OnDisconnect()
	{
		/* For the same reason as above, we don't
		 * handle OnDisconnect()
		 */
		return true;
	}

	/** Recursively send the server tree with distances as hops.
	 * This is used during network burst to inform the other server
	 * (and any of ITS servers too) of what servers we know about.
	 * If at any point any of these servers already exist on the other
	 * end, our connection may be terminated. The hopcounts given
	 * by this function are relative, this doesn't matter so long as
	 * they are all >1, as all the remote servers re-calculate them
	 * to be relative too, with themselves as hop 0.
	 */
	void SendServers(TreeServer* Current, TreeServer* s, int hops)
	{
		char command[1024];
		for (unsigned int q = 0; q < Current->ChildCount(); q++)
		{
			TreeServer* recursive_server = Current->GetChild(q);
			if (recursive_server != s)
			{
				snprintf(command,1024,":%s SERVER %s * %d :%s",Current->GetName().c_str(),recursive_server->GetName().c_str(),hops,recursive_server->GetDesc().c_str());
				this->WriteLine(command);
				this->WriteLine(":"+recursive_server->GetName()+" VERSION :"+recursive_server->GetVersion());
				/* down to next level */
				this->SendServers(recursive_server, s, hops+1);
			}
		}
	}

	std::string MyCapabilities()
	{
		std::vector<std::string> modlist;
		std::string capabilities = "";

		for (int i = 0; i <= this->Instance->GetModuleCount(); i++)
		{
			if (this->Instance->modules[i]->GetVersion().Flags & VF_COMMON)
				modlist.push_back(this->Instance->Config->module_names[i]);
		}
		sort(modlist.begin(),modlist.end());
		for (unsigned int i = 0; i < modlist.size(); i++)
		{
			if (i)
				capabilities = capabilities + ",";
			capabilities = capabilities + modlist[i];
		}
		return capabilities;
	}
	
	void SendCapabilities()
	{
		irc::commasepstream modulelist(MyCapabilities());

		this->WriteLine("CAPAB START");

		/* Send module names, split at 509 length */
		std::string item = "*";
		std::string line = "CAPAB MODULES ";
		while ((item = modulelist.GetToken()) != "")
		{
			if (line.length() + item.length() + 1 > 509)
			{
				this->WriteLine(line);
				line = "CAPAB MODULES ";
			}

			if (line != "CAPAB MODULES ")
				line.append(",");

			line.append(item);
		}
		if (line != "CAPAB MODULES ")
			this->WriteLine(line);

		int ip6 = 0;
		int ip6support = 0;
#ifdef IPV6
		ip6 = 1;
#endif
#ifdef SUPPORT_IP6LINKS
		ip6support = 1;
#endif
		this->WriteLine("CAPAB CAPABILITIES :NICKMAX="+ConvToStr(NICKMAX)+" HALFOP="+ConvToStr(this->Instance->Config->AllowHalfop)+" CHANMAX="+ConvToStr(CHANMAX)+" MAXMODES="+ConvToStr(MAXMODES)+" IDENTMAX="+ConvToStr(IDENTMAX)+" MAXQUIT="+ConvToStr(MAXQUIT)+" MAXTOPIC="+ConvToStr(MAXTOPIC)+" MAXKICK="+ConvToStr(MAXKICK)+" MAXGECOS="+ConvToStr(MAXGECOS)+" MAXAWAY="+ConvToStr(MAXAWAY)+" IP6NATIVE="+ConvToStr(ip6)+" IP6SUPPORT="+ConvToStr(ip6support)+" PROTOCOL="+ConvToStr(ProtocolVersion));

		this->WriteLine("CAPAB END");
	}

	/* Check a comma seperated list for an item */
	bool HasItem(const std::string &list, const std::string &item)
	{
		irc::commasepstream seplist(list);

		std::string item2 = "*";
		while ((item2 = seplist.GetToken()) != "")
		{
			if (item2 == item)
				return true;
		}

		return false;
	}

	/* Isolate and return the elements that are different between two comma seperated lists */
	std::string ListDifference(const std::string &one, const std::string &two)
	{
		irc::commasepstream list_one(one);
		std::string item = "*";
		std::string result = "";
		while ((item = list_one.GetToken()) != "")
		{
			if (!HasItem(two, item))
			{
				result.append(" ");
				result.append(item);
			}
		}
		return result;
	}

	bool Capab(const std::deque<std::string> &params)
	{
		if (params.size() < 1)
		{
			this->WriteLine("ERROR :Invalid number of parameters for CAPAB - Mismatched version");
			return false;
		}

		if (params[0] == "START")
		{
			this->ModuleList = "";
			this->CapKeys.clear();
		}
		else if (params[0] == "END")
		{
			std::string reason = "";
			int ip6support = 0;
#ifdef SUPPORT_IP6LINKS
			ip6support = 1;
#endif
			/* Compare ModuleList and check CapKeys...
			 * Maybe this could be tidier? -- Brain
			 */
			if ((this->ModuleList != this->MyCapabilities()) && (this->ModuleList.length()))
			{
				std::string diff = ListDifference(this->ModuleList, this->MyCapabilities());
				if (!diff.length())
				{
					diff = "your server:" + ListDifference(this->MyCapabilities(), this->ModuleList);
				}
				else
				{
					diff = "this server:" + diff;
				}
				if (diff.length() == 12)
					reason = "Module list in CAPAB is not alphabetically ordered, cannot compare lists.";
				else
					reason = "Modules loaded on these servers are not correctly matched, these modules are not loaded on " + diff;
			}

			if (((this->CapKeys.find("IP6SUPPORT") == this->CapKeys.end()) && (ip6support)) || ((this->CapKeys.find("IP6SUPPORT") != this->CapKeys.end()) && (this->CapKeys.find("IP6SUPPORT")->second != ConvToStr(ip6support))))
				reason = "We don't both support linking to IPV6 servers";

			if (((this->CapKeys.find("IP6NATIVE") != this->CapKeys.end()) && (this->CapKeys.find("IP6NATIVE")->second == "1")) && (!ip6support))
				reason = "The remote server is IPV6 native, and we don't support linking to IPV6 servers";

			if (((this->CapKeys.find("NICKMAX") == this->CapKeys.end()) || ((this->CapKeys.find("NICKMAX") != this->CapKeys.end()) && (this->CapKeys.find("NICKMAX")->second != ConvToStr(NICKMAX)))))
				reason = "Maximum nickname lengths differ or remote nickname length not specified";

			if (((this->CapKeys.find("PROTOCOL") == this->CapKeys.end()) || ((this->CapKeys.find("PROTOCOL") != this->CapKeys.end()) && (this->CapKeys.find("PROTOCOL")->second != ConvToStr(ProtocolVersion)))))
			{
				if (this->CapKeys.find("PROTOCOL") != this->CapKeys.end())
				{
					reason = "Mismatched protocol versions "+this->CapKeys.find("PROTOCOL")->second+" and "+ConvToStr(ProtocolVersion);
				}
				else
				{
					reason = "Protocol version not specified";
				}
			}

			if (((this->CapKeys.find("HALFOP") == this->CapKeys.end()) && (Instance->Config->AllowHalfop)) || ((this->CapKeys.find("HALFOP") != this->CapKeys.end()) && (this->CapKeys.find("HALFOP")->second != ConvToStr(Instance->Config->AllowHalfop))))
				reason = "We don't both have halfop support enabled/disabled identically";

			if (((this->CapKeys.find("IDENTMAX") == this->CapKeys.end()) || ((this->CapKeys.find("IDENTMAX") != this->CapKeys.end()) && (this->CapKeys.find("IDENTMAX")->second != ConvToStr(IDENTMAX)))))
				reason = "Maximum ident lengths differ or remote ident length not specified";

			if (((this->CapKeys.find("CHANMAX") == this->CapKeys.end()) || ((this->CapKeys.find("CHANMAX") != this->CapKeys.end()) && (this->CapKeys.find("CHANMAX")->second != ConvToStr(CHANMAX)))))
				reason = "Maximum channel lengths differ or remote channel length not specified";

			if (((this->CapKeys.find("MAXMODES") == this->CapKeys.end()) || ((this->CapKeys.find("MAXMODES") != this->CapKeys.end()) && (this->CapKeys.find("MAXMODES")->second != ConvToStr(MAXMODES)))))
				reason = "Maximum modes per line differ or remote modes per line not specified";

			if (((this->CapKeys.find("MAXQUIT") == this->CapKeys.end()) || ((this->CapKeys.find("MAXQUIT") != this->CapKeys.end()) && (this->CapKeys.find("MAXQUIT")->second != ConvToStr(MAXQUIT)))))
				reason = "Maximum quit lengths differ or remote quit length not specified";

			if (((this->CapKeys.find("MAXTOPIC") == this->CapKeys.end()) || ((this->CapKeys.find("MAXTOPIC") != this->CapKeys.end()) && (this->CapKeys.find("MAXTOPIC")->second != ConvToStr(MAXTOPIC)))))
				reason = "Maximum topic lengths differ or remote topic length not specified";

			if (((this->CapKeys.find("MAXKICK") == this->CapKeys.end()) || ((this->CapKeys.find("MAXKICK") != this->CapKeys.end()) && (this->CapKeys.find("MAXKICK")->second != ConvToStr(MAXKICK)))))
				reason = "Maximum kick lengths differ or remote kick length not specified";

			if (((this->CapKeys.find("MAXGECOS") == this->CapKeys.end()) || ((this->CapKeys.find("MAXGECOS") != this->CapKeys.end()) && (this->CapKeys.find("MAXGECOS")->second != ConvToStr(MAXGECOS)))))
				reason = "Maximum GECOS (fullname) lengths differ or remote GECOS length not specified";

			if (((this->CapKeys.find("MAXAWAY") == this->CapKeys.end()) || ((this->CapKeys.find("MAXAWAY") != this->CapKeys.end()) && (this->CapKeys.find("MAXAWAY")->second != ConvToStr(MAXAWAY)))))
				reason = "Maximum awaymessage lengths differ or remote awaymessage length not specified";

			if (reason.length())
			{
				this->WriteLine("ERROR :CAPAB negotiation failed: "+reason);
				return false;
			}
		}
		else if ((params[0] == "MODULES") && (params.size() == 2))
		{
			if (!this->ModuleList.length())
			{
				this->ModuleList.append(params[1]);
			}
			else
			{
				this->ModuleList.append(",");
				this->ModuleList.append(params[1]);
			}
		}
		else if ((params[0] == "CAPABILITIES") && (params.size() == 2))
		{
			irc::tokenstream capabs(params[1]);
			std::string item = "*";
			while ((item = capabs.GetToken()) != "")
			{
				/* Process each key/value pair */
				std::string::size_type equals = item.rfind('=');
				if (equals != std::string::npos)
				{
					std::string var = item.substr(0, equals);
					std::string value = item.substr(equals+1, item.length());
					this->Instance->Log(DEBUG,"Key='%s' Value='%s'",var.c_str(),value.c_str());
					CapKeys[var] = value;
				}
			}
		}

		return true;
	}

	/** This function forces this server to quit, removing this server
	 * and any users on it (and servers and users below that, etc etc).
	 * It's very slow and pretty clunky, but luckily unless your network
	 * is having a REAL bad hair day, this function shouldnt be called
	 * too many times a month ;-)
	 */
	void SquitServer(std::string &from, TreeServer* Current)
	{
		/* recursively squit the servers attached to 'Current'.
		 * We're going backwards so we don't remove users
		 * while we still need them ;)
		 */
		for (unsigned int q = 0; q < Current->ChildCount(); q++)
		{
			TreeServer* recursive_server = Current->GetChild(q);
			this->SquitServer(from,recursive_server);
		}
		/* Now we've whacked the kids, whack self */
		num_lost_servers++;
		num_lost_users += Current->QuitUsers(from);
	}

	/** This is a wrapper function for SquitServer above, which
	 * does some validation first and passes on the SQUIT to all
	 * other remaining servers.
	 */
	void Squit(TreeServer* Current, const std::string &reason)
	{
		if ((Current) && (Current != Utils->TreeRoot))
		{
			Event rmode((char*)Current->GetName().c_str(), (Module*)Utils->Creator, "lost_server");
			rmode.Send(Instance);

			std::deque<std::string> params;
			params.push_back(Current->GetName());
			params.push_back(":"+reason);
			Utils->DoOneToAllButSender(Current->GetParent()->GetName(),"SQUIT",params,Current->GetName());
			if (Current->GetParent() == Utils->TreeRoot)
			{
				this->Instance->WriteOpers("Server \002"+Current->GetName()+"\002 split: "+reason);
			}
			else
			{
				this->Instance->WriteOpers("Server \002"+Current->GetName()+"\002 split from server \002"+Current->GetParent()->GetName()+"\002 with reason: "+reason);
			}
			num_lost_servers = 0;
			num_lost_users = 0;
			std::string from = Current->GetParent()->GetName()+" "+Current->GetName();
			SquitServer(from, Current);
			Current->Tidy();
			Current->GetParent()->DelChild(Current);
			DELETE(Current);
			this->Instance->WriteOpers("Netsplit complete, lost \002%d\002 users on \002%d\002 servers.", num_lost_users, num_lost_servers);
		}
		else
		{
			Instance->Log(DEFAULT,"Squit from unknown server");
		}
	}

	/** FMODE command - server mode with timestamp checks */
	bool ForceMode(const std::string &source, std::deque<std::string> &params)
	{
		/* Chances are this is a 1.0 FMODE without TS */
		if (params.size() < 3)
		{
			/* No modes were in the command, probably a channel with no modes set on it */
			return true;
		}
		
		bool smode = false;
		std::string sourceserv;

		/* Are we dealing with an FMODE from a user, or from a server? */
		userrec* who = this->Instance->FindNick(source);
		if (who)
		{
			/* FMODE from a user, set sourceserv to the users server name */
			sourceserv = who->server;
		}
		else
		{
			/* FMODE from a server, create a fake user to receive mode feedback */
			who = new userrec(this->Instance);
			who->SetFd(FD_MAGIC_NUMBER);
			smode = true;		/* Setting this flag tells us we should free the userrec later */
			sourceserv = source;	/* Set sourceserv to the actual source string */
		}
		const char* modelist[64];
		time_t TS = 0;
		int n = 0;
		memset(&modelist,0,sizeof(modelist));
		for (unsigned int q = 0; (q < params.size()) && (q < 64); q++)
		{
			if (q == 1)
			{
				/* The timestamp is in this position.
				 * We don't want to pass that up to the
				 * server->client protocol!
				 */
				TS = atoi(params[q].c_str());
			}
			else
			{
				/* Everything else is fine to append to the modelist */
				modelist[n++] = params[q].c_str();
			}
				
		}
                /* Extract the TS value of the object, either userrec or chanrec */
		userrec* dst = this->Instance->FindNick(params[0]);
		chanrec* chan = NULL;
		time_t ourTS = 0;
		if (dst)
		{
			ourTS = dst->age;
		}
		else
		{
			chan = this->Instance->FindChan(params[0]);
			if (chan)
			{
				ourTS = chan->age;
			}
			else
				/* Oops, channel doesnt exist! */
				return true;
		}

		/* TS is equal: Merge the mode changes, use voooodoooooo on modes
		 * with parameters.
		 */
		if (TS == ourTS)
		{
			Instance->Log(DEBUG,"Entering TS equality check");
			ModeHandler* mh = NULL;
			unsigned long paramptr = 3;
			std::string to_bounce = "";
			std::string to_keep = "";
			std::vector<std::string> params_to_keep;
			std::string params_to_bounce = "";
			bool adding = true;
			char cur_change = 1;
			char old_change = 0;
			char old_bounce_change = 0;
			/* Merge modes, basically do special stuff to mode with params */
			for (std::string::iterator x = params[2].begin(); x != params[2].end(); x++)
			{
				switch (*x)
				{
					case '-':
						adding = false;
					break;
					case '+':
						adding = true;
					break;
					default:
						if (adding)
						{
							/* We only care about whats being set,
							 * not whats being unset
							 */
							mh = this->Instance->Modes->FindMode(*x, chan ? MODETYPE_CHANNEL : MODETYPE_USER);

							if ((mh) && (mh->GetNumParams(adding) > 0) && (!mh->IsListMode()))
							{
								/* We only want to do special things to
								 * modes with parameters, we are going to rewrite
								 * those parameters
								 */
								ModePair ret;
								adding ? cur_change = '+' : cur_change = '-';

								ret = mh->ModeSet(smode ? NULL : who, dst, chan, params[paramptr]);

								/* The mode is set here, check which we should keep */
								if (ret.first)
								{
									bool which_to_keep = mh->CheckTimeStamp(TS, ourTS, params[paramptr], ret.second, chan);

									if (which_to_keep == true)
									{
										/* Keep ours, bounce theirs:
										 * Send back ours to them and
										 * drop their mode changs
										 */
										adding ? cur_change = '+' : cur_change = '-';
										if (cur_change != old_bounce_change)
											to_bounce += cur_change;
										to_bounce += *x;
										old_bounce_change = cur_change;

										if ((mh->GetNumParams(adding) > 0) && (paramptr < params.size()))
											params_to_bounce.append(" ").append(ret.second);
									}
									else
									{
										/* Keep theirs: Accept their mode change,
										 * do nothing else
										 */
										adding ? cur_change = '+' : cur_change = '-';
										if (cur_change != old_change)
											to_keep += cur_change;
										to_keep += *x;
										old_change = cur_change;

										if ((mh->GetNumParams(adding) > 0) && (paramptr < params.size()))
											params_to_keep.push_back(params[paramptr]);
									}
								}
								else
								{
									/* Mode isnt set here, we want it */
									adding ? cur_change = '+' : cur_change = '-';
									if (cur_change != old_change)
										to_keep += cur_change;
									to_keep += *x;
									old_change = cur_change;

									if ((mh->GetNumParams(adding) > 0) && (paramptr < params.size()))
										params_to_keep.push_back(params[paramptr]);
								}

								paramptr++;
							}
							else
							{
								mh = this->Instance->Modes->FindMode(*x, chan ? MODETYPE_CHANNEL : MODETYPE_USER);

								if (mh)
								{
									adding ? cur_change = '+' : cur_change = '-';
	
									/* Just keep this, safe to merge with no checks
									 * it has no parameters
									 */
	
									if (cur_change != old_change)
										to_keep += cur_change;
									to_keep += *x;
									old_change = cur_change;
	
									if ((mh->GetNumParams(adding) > 0) && (paramptr < params.size()))
									{
										Instance->Log(DEBUG,"Mode removal %d %d",adding, mh->GetNumParams(adding));
										params_to_keep.push_back(params[paramptr++]);
									}
								}
							}
						}
						else
						{
							mh = this->Instance->Modes->FindMode(*x, chan ? MODETYPE_CHANNEL : MODETYPE_USER);

							if (mh)
							{
								/* Taking a mode away */
								adding ? cur_change = '+' : cur_change = '-';

								if (cur_change != old_change)
									to_keep += cur_change;
								to_keep += *x;
								old_change = cur_change;

								if ((mh->GetNumParams(adding) > 0) && (paramptr < params.size()))
									params_to_keep.push_back(params[paramptr++]);
							}
						}
					break;
				}
			}

			if (to_bounce.length())
			{
				std::deque<std::string> newparams;
				newparams.push_back(params[0]);
				newparams.push_back(ConvToStr(ourTS));
				newparams.push_back(to_bounce+params_to_bounce);
				Instance->Log(DEBUG,"BOUNCE BACK: %s",(to_bounce+params_to_bounce).c_str());
				Utils->DoOneToOne(this->Instance->Config->ServerName,"FMODE",newparams,sourceserv);
			}

			if (to_keep.length())
			{
				unsigned int n = 2;
				unsigned int q = 0;
				modelist[0] = params[0].c_str();
				modelist[1] = to_keep.c_str();

				if (params_to_keep.size() > 0)
				{
					for (q = 0; (q < params_to_keep.size()) && (q < 64); q++)
					{
						Instance->Log(DEBUG,"KEEP Item %d of %d: %s", q, params_to_keep.size(), params_to_keep[q].c_str());
						modelist[n++] = params_to_keep[q].c_str();
					}
				}

                	        if (smode)
				{
					Instance->Log(DEBUG,"Send mode");
					this->Instance->SendMode(modelist, n, who);
				}
				else
				{
					Instance->Log(DEBUG,"Send mode client");
					this->Instance->CallCommandHandler("MODE", modelist, n, who);
				}

				/* HOT POTATO! PASS IT ON! */
				Utils->DoOneToAllButSender(source,"FMODE",params,sourceserv);
			}
		}
		else
		/* U-lined servers always win regardless of their TS */
		if ((TS > ourTS) && (!this->Instance->ULine(source.c_str())))
		{
			/* Bounce the mode back to its sender.* We use our lower TS, so the other end
			 * SHOULD accept it, if its clock is right.
			 *
			 * NOTE: We should check that we arent bouncing anything thats already set at this end.
			 * If we are, bounce +ourmode to 'reinforce' it. This prevents desyncs.
			 * e.g. They send +l 50, we have +l 10 set. rather than bounce -l 50, we bounce +l 10.
			 *
			 * Thanks to jilles for pointing out this one-hell-of-an-issue before i even finished
			 * writing the code. It took me a while to come up with this solution.
			 *
			 * XXX: BE SURE YOU UNDERSTAND THIS CODE FULLY BEFORE YOU MESS WITH IT.
			 */

			std::deque<std::string> newparams;	/* New parameter list we send back */
			newparams.push_back(params[0]);		/* Target, user or channel */
			newparams.push_back(ConvToStr(ourTS));	/* Timestamp value of the target */
			newparams.push_back("");		/* This contains the mode string. For now
								 * it's empty, we fill it below.
								 */

			/* Intelligent mode bouncing. Don't just invert, reinforce any modes which are already
			 * set to avoid a desync here.
			 */
			std::string modebounce = "";
			bool adding = true;
			unsigned int t = 3;
			ModeHandler* mh = NULL;
			char cur_change = 1;
			char old_change = 0;
			for (std::string::iterator x = params[2].begin(); x != params[2].end(); x++)
			{
				/* Iterate over all mode chars in the sent set */
				switch (*x)
				{
					/* Adding or subtracting modes? */
					case '-':
						adding = false;
					break;
					case '+':
						adding = true;
					break;
					default:
						/* Find the mode handler for this mode */
						mh = this->Instance->Modes->FindMode(*x, chan ? MODETYPE_CHANNEL : MODETYPE_USER);

						/* Got a mode handler?
						 * This also prevents us bouncing modes we have no handler for.
						 */
						if (mh)
						{
							ModePair ret;
							std::string p = "";

							/* Does the mode require a parameter right now?
							 * If it does, fetch it if we can
							 */
							if ((mh->GetNumParams(adding) > 0) && (t < params.size()))
								p = params[t++];

							/* Call the ModeSet method to determine if its set with the
							 * given parameter here or not.
							 */
							ret = mh->ModeSet(smode ? NULL : who, dst, chan, p);

							/* XXX: Really. Dont ask.
							 * Determine from if its set combined with what the current
							 * 'state' is (adding or not) as to wether we should 'invert'
							 * or 'reinforce' the mode change
							 */
							(!ret.first ? (adding ? cur_change = '-' : cur_change = '+') : (!adding ? cur_change = '-' : cur_change = '+'));

							/* Quickly determine if we have 'flipped' from + to -,
							 * or - to +, to prevent unneccessary +/- chars in the
							 * output string that waste bandwidth
							 */
							if (cur_change != old_change)
								modebounce += cur_change;
							old_change = cur_change;

							/* Add the mode character to the output string */
							modebounce += mh->GetModeChar();

							/* We got a parameter back from ModeHandler::ModeSet,
							 * are we supposed to be sending one out right now?
							 */
							if (ret.second.length())
							{
								if (mh->GetNumParams(cur_change == '+') > 0)
									/* Yes we're supposed to be sending out
									 * the parameter. Make sure it goes
									 */
									newparams.push_back(ret.second);
							}

						}
					break;
				}
			}
			
			/* Update the parameters for FMODE with the new 'bounced' string */
			newparams[2] = modebounce;
			/* Only send it back the way it came, no need to send it anywhere else */
			Utils->DoOneToOne(this->Instance->Config->ServerName,"FMODE",newparams,sourceserv);
			Instance->Log(DEBUG,"FMODE bounced intelligently, our TS less than theirs and the other server is NOT a uline.");
		}
		else
		{
			Instance->Log(DEBUG,"Allow modes, TS lower for sender");
			/* The server was ulined, but something iffy is up with the TS.
			 * Sound the alarm bells!
			 */
			if ((this->Instance->ULine(sourceserv.c_str())) && (TS > ourTS))
			{
				this->Instance->WriteOpers("\2WARNING!\2 U-Lined server '%s' has bad TS for '%s' (accepted change): \2SYNC YOUR CLOCKS\2 to avoid this notice",sourceserv.c_str(),params[0].c_str());
			}
			/* Allow the mode, route it to either server or user command handling */
			if (smode)
				this->Instance->SendMode(modelist,n,who);
			else
				this->Instance->CallCommandHandler("MODE", modelist, n, who);

			/* HOT POTATO! PASS IT ON! */
			Utils->DoOneToAllButSender(source,"FMODE",params,sourceserv);
		}
		/* Are we supposed to free the userrec? */
		if (smode)
			DELETE(who);

		return true;
	}

	/** FTOPIC command */
	bool ForceTopic(const std::string &source, std::deque<std::string> &params)
	{
		if (params.size() != 4)
			return true;
		time_t ts = atoi(params[1].c_str());
		std::string nsource = source;

		chanrec* c = this->Instance->FindChan(params[0]);
		if (c)
		{
			if ((ts >= c->topicset) || (!*c->topic))
			{
				std::string oldtopic = c->topic;
				strlcpy(c->topic,params[3].c_str(),MAXTOPIC);
				strlcpy(c->setby,params[2].c_str(),NICKMAX-1);
				c->topicset = ts;
				/* if the topic text is the same as the current topic,
				 * dont bother to send the TOPIC command out, just silently
				 * update the set time and set nick.
				 */
				if (oldtopic != params[3])
				{
					userrec* user = this->Instance->FindNick(source);
					if (!user)
					{
						c->WriteChannelWithServ(source.c_str(), "TOPIC %s :%s", c->name, c->topic);
					}
					else
					{
						c->WriteChannel(user, "TOPIC %s :%s", c->name, c->topic);
						nsource = user->server;
					}
					/* all done, send it on its way */
					params[3] = ":" + params[3];
					Utils->DoOneToAllButSender(source,"FTOPIC",params,nsource);
				}
			}
			
		}

		return true;
	}

	/** FJOIN, similar to TS6 SJOIN, but not quite. */
	bool ForceJoin(const std::string &source, std::deque<std::string> &params)
	{
		/* 1.1 FJOIN works as follows:
		 *
		 * Each FJOIN is sent along with a timestamp, and the side with the lowest
		 * timestamp 'wins'. From this point on we will refer to this side as the
		 * winner. The side with the higher timestamp loses, from this point on we
		 * will call this side the loser or losing side. This should be familiar to
		 * anyone who's dealt with dreamforge or TS6 before.
		 *
		 * When two sides of a split heal and this occurs, the following things
		 * will happen:
		 *
		 * If the timestamps are exactly equal, both sides merge their privilages
		 * and users, as in InspIRCd 1.0 and ircd2.8. The channels have not been
		 * re-created during a split, this is safe to do.
		 *
		 *
		 * If the timestamps are NOT equal, the losing side removes all privilage
		 * modes from all of its users that currently exist in the channel, before
		 * introducing new users into the channel which are listed in the FJOIN
		 * command's parameters. This means, all modes +ohv, and privilages added
		 * by modules, such as +qa. The losing side then LOWERS its timestamp value
		 * of the channel to match that of the winning side, and the modes of the
		 * users of the winning side are merged in with the losing side. The loser
		 * then sends out a set of FMODE commands which 'confirm' that it just
		 * removed all privilage modes from its existing users, which allows for
		 * services packages to still work correctly without needing to know the
		 * timestamping rules which InspIRCd follows. In TS6 servers this is always
		 * a problem, and services packages must contain code which explicitly
		 * behaves as TS6 does, removing ops from the losing side of a split where
		 * neccessary within its internal records, as this state information is
		 * not explicitly echoed out in that protocol.
		 *
		 * The winning side on the other hand will ignore all user modes from the
		 * losing side, so only its own modes get applied. Life is simple for those
		 * who succeed at internets. :-)
		 *
		 * NOTE: Unlike TS6 and dreamforge and other protocols which have SJOIN,
		 * FJOIN does not contain the simple-modes such as +iklmnsp. Why not,
		 * you ask? Well, quite simply because we don't need to. They'll be sent
		 * after the FJOIN by FMODE, and FMODE is timestamped, so in the event
		 * the losing side sends any modes for the channel which shouldnt win,
		 * they wont as their timestamp will be too high :-)
		 */

		if (params.size() < 3)
			return true;

		char first[MAXBUF];		/* The first parameter of the mode command */
		char modestring[MAXBUF];	/* The mode sequence (2nd parameter) of the mode command */
		char* mode_users[127];		/* The values used by the mode command */
		memset(&mode_users,0,sizeof(mode_users));	/* Initialize mode parameters */
		mode_users[0] = first;		/* Set this up to be our on-stack value */
		mode_users[1] = modestring;	/* Same here as above */
		strcpy(modestring,"+");		/* Initialize the mode sequence to just '+' */
		unsigned int modectr = 2;	/* Pointer to the third mode parameter (e.g. the one after the +-sequence) */
		
		userrec* who = NULL;			/* User we are currently checking */
		std::string channel = params[0];	/* Channel name, as a string */
		time_t TS = atoi(params[1].c_str());	/* Timestamp given to us for remote side */
		bool created = false;
		
		/* Try and find the channel */
		chanrec* chan = this->Instance->FindChan(channel);

		/* Initialize channel name in the mode parameters */
		strlcpy(mode_users[0],channel.c_str(),MAXBUF);

		/* default TS is a high value, which if we dont have this
		 * channel will let the other side apply their modes.
		 */
		time_t ourTS = Instance->Time(true)+600;

		/* Does this channel exist? if it does, get its REAL timestamp */
		if (chan)
			ourTS = chan->age;
		else
			created = true; /* don't perform deops, and set TS to correct time after processing. */

		/* In 1.1, if they have the newer channel, we immediately clear
		 * all status modes from our users. We then accept their modes.
		 * If WE have the newer channel its the other side's job to do this.
		 * Note that this causes the losing server to send out confirming
		 * FMODE lines.
		 */
		if (ourTS > TS)
		{
			Instance->Log(DEBUG,"************ WE LOST TS COMPARE, DEOPPING EVERYONE ********************* ");
			std::deque<std::string> param_list;

			/* Lower the TS here */
			if (Utils->AnnounceTSChange && chan)
				chan->WriteChannelWithServ(Instance->Config->ServerName,
				"NOTICE %s :TS for %s changed from %lu to %lu", chan->name, chan->name, ourTS, TS);
			ourTS = TS;

			/* Zap all the privilage modes on our side, if the channel exists here */
			if (!created)
			{
				param_list.push_back(channel);
				/* Do this first! */
				chan->age = TS;
				this->RemoveStatus(Instance->Config->ServerName, param_list);
			}
		}

		/* Put the final parameter of the FJOIN into a tokenstream ready to split it */
		irc::tokenstream users(params[2]);
		std::string item = "*";

		/* do this first, so our mode reversals are correctly received by other servers
		 * if there is a TS collision.
		 */
		params[2] = ":" + params[2];
		Utils->DoOneToAllButSender(source,"FJOIN",params,source);

		/* Now, process every 'prefixes,nick' pair */
		while (item != "")
		{
			/* Find next user */
			item = users.GetToken();

			const char* usr = item.c_str();

			/* Safety check just to make sure someones not sent us an FJOIN full of spaces
			 * (is this even possible?) */
			if (usr && *usr)
			{
				const char* permissions = usr;
				int ntimes = 0;
				char* nm = new char[MAXBUF];
				char* tnm = nm;

				/* Iterate through all the prefix values, convert them from prefixes
				 * to mode letters, and append them to the mode sequence
				 */
				while ((*permissions) && (*permissions != ',') && (ntimes < MAXBUF))
				{
					ModeHandler* mh = Instance->Modes->FindPrefix(*permissions);
					if (mh)
					{
						/* This is a valid prefix */
						ntimes++;
						*tnm++ = mh->GetModeChar();
					}
					else
					{
						/* Not a valid prefix...
						 * danger bill bobbertson! (that's will robinsons older brother ;-) ...)
						 */
						this->Instance->WriteOpers("ERROR: We received a user with an unknown prefix '%c'. Closed connection to avoid a desync.",*permissions);
						this->WriteLine(std::string("ERROR :Invalid prefix '")+(*permissions)+"' in FJOIN");
						return false;
					}
					usr++;
					permissions++;
				}

				/* Null terminate modes */
				*tnm = 0;
				/* Advance past the comma, to the nick */
				usr++;

				/* Check the user actually exists */
				who = this->Instance->FindNick(usr);
				if (who)
				{
					/* Did they get any modes? How many times? */
					strlcat(modestring, nm, MAXBUF);
					for (int k = 0; k < ntimes; k++)
						mode_users[modectr++] = strdup(usr);

					/* Free temporary buffer used for mode sequence */
					delete[] nm;

					/* Check that the user's 'direction' is correct
					 * based on the server sending the FJOIN. We must
					 * check each nickname in turn, because the origin of
					 * the FJOIN may be different to the origin of the nicks
					 * in the command itself.
					 */
					TreeServer* route_back_again = Utils->BestRouteTo(who->server);
					if ((!route_back_again) || (route_back_again->GetSocket() != this))
					{
						/* Oh dear oh dear. */
						Instance->Log(DEBUG,"Fake direction in FJOIN, user '%s'",who->nick);
						continue;
					}
					/* Finally, we can actually place the user into the channel.
					 * We're sure its right. Final answer, phone a friend.
					 */
					chanrec::JoinUser(this->Instance, who, channel.c_str(), true, "");

					/* Have we already queued up MAXMODES modes with parameters
					 * (+qaohv) ready to be sent to the server?
					 */
					if (modectr >= (MAXMODES-1))
					{
						/* Only actually give the users any status if we lost
						 * the FJOIN or drew (equal timestamps).
						 * It isn't actually possible for ourTS to be > TS here,
						 * only possible to actually have ourTS == TS, or
						 * ourTS < TS, because if we lost, we already lowered
						 * our TS above before we entered this loop. We only
						 * check >= as a safety measure, in case someone stuffed
						 * up. If someone DID stuff up, it was most likely me.
						 * Note: I do not like baseball bats in the face...
						 */
						if (ourTS >= TS)
						{
							Instance->Log(DEBUG,"Our our channel newer than theirs, accepting their modes");
							this->Instance->SendMode((const char**)mode_users,modectr,who);

							/* Something stuffed up, and for some reason, the timestamp is
							 * NOT lowered right now and should be. Lower it. Usually this
							 * code won't be executed, doubtless someone will remove it some
							 * day soon.
							 */
							if (ourTS > TS)
							{
								Instance->Log(DEFAULT,"Channel TS for %s changed from %lu to %lu",chan->name,ourTS,TS);
								chan->age = TS;
								ourTS = TS;
							}
						}

						/* Reset all this back to defaults, and
						 * free any ram we have left allocated.
						 */
						strcpy(mode_users[1],"+");
						for (unsigned int f = 2; f < modectr; f++)
							free(mode_users[f]);
						modectr = 2;
					}
				}
				else
				{
					/* Remember to free this */
					delete[] nm;
					/* If we got here, there's a nick in FJOIN which doesnt exist on this server.
					 * We don't try to process the nickname here (that WOULD cause a segfault because
					 * we'd be playing with null pointers) however, we DO pass the nickname on, just
					 * in case somehow we're desynched, so that other users which might be able to see
					 * the nickname get their fair chance to process it.
					 */
					Instance->Log(SPARSE,"Warning! Invalid user in FJOIN to channel %s IGNORED", channel.c_str());
					continue;
				}
			}
		}

		/* there werent enough modes built up to flush it during FJOIN,
		 * or, there are a number left over. flush them out.
		 */
		if ((modectr > 2) && (who) && (chan))
		{
			if (ourTS >= TS)
			{
				/* Our channel is newer than theirs. Evil deeds must be afoot. */
				this->Instance->SendMode((const char**)mode_users,modectr,who);
				/* Yet again, we can't actually get a true value here, if everything else
				 * is working as it should.
				 */
				if (ourTS > TS)
				{
					Instance->Log(DEFAULT,"Channel TS for %s changed from %lu to %lu",chan->name,ourTS,TS);
					chan->age = TS;
					ourTS = TS;
				}
			}

			/* Free anything we have left to free */
			for (unsigned int f = 2; f < modectr; f++)
				free(mode_users[f]);
		}

		/* if we newly created the channel, set it's TS properly. */
		if (created)
		{
			/* find created channel .. */
			chan = this->Instance->FindChan(channel);
			if (chan)
				/* w00t said this shouldnt be needed but it is.
				 * This isnt strictly true, as chan can be NULL
				 * if a nick collision has occured and therefore
				 * the channel was never created.
				 */
				chan->age = TS;
		}

		/* All done. That wasnt so bad was it, you can wipe
		 * the sweat from your forehead now. :-)
		 */
		return true;
	}

	/** NICK command */
	bool IntroduceClient(const std::string &source, std::deque<std::string> &params)
	{
		if (params.size() < 8)
			return true;
		if (params.size() > 8)
		{
			this->WriteLine(std::string(":")+this->Instance->Config->ServerName+" KILL "+params[1]+" :Invalid client introduction ("+params[1]+"?)");
			return true;
		}
		// NICK age nick host dhost ident +modes ip :gecos
		//       0    1   2     3     4      5   6     7
		time_t age = atoi(params[0].c_str());
		
		const char* tempnick = params[1].c_str();
		Instance->Log(DEBUG,"Introduce client %s!%s@%s",tempnick,params[4].c_str(),params[2].c_str());
		
		user_hash::iterator iter = this->Instance->clientlist->find(tempnick);
		
		if (iter != this->Instance->clientlist->end())
		{
			// nick collision
			Instance->Log(DEBUG,"Nick collision on %s!%s@%s: %lu %lu",tempnick,params[4].c_str(),params[2].c_str(),(unsigned long)age,(unsigned long)iter->second->age);
			this->WriteLine(std::string(":")+this->Instance->Config->ServerName+" KILL "+tempnick+" :Nickname collision");
			userrec::QuitUser(this->Instance, iter->second, "Nickname collision");
			return true;
		}

		userrec* _new = new userrec(this->Instance);
		(*(this->Instance->clientlist))[tempnick] = _new;
		_new->SetFd(FD_MAGIC_NUMBER);
		strlcpy(_new->nick, tempnick,NICKMAX-1);
		strlcpy(_new->host, params[2].c_str(),63);
		strlcpy(_new->dhost, params[3].c_str(),63);
		_new->server = this->Instance->FindServerNamePtr(source.c_str());
		strlcpy(_new->ident, params[4].c_str(),IDENTMAX);
		strlcpy(_new->fullname, params[7].c_str(),MAXGECOS);
		_new->registered = REG_ALL;
		_new->signon = age;
		
		/*
		 * we need to remove the + from the modestring, so we can do our stuff
		 */
		std::string::size_type pos_after_plus = params[5].find_first_not_of('+');
		if (pos_after_plus != std::string::npos)
			params[5] = params[5].substr(pos_after_plus);
		

		for (std::string::iterator v = params[5].begin(); v != params[5].end(); v++)
			_new->modes[(*v)-65] = 1;

		/* now we've done with modes processing, put the + back for remote servers */
		params[5] = "+" + params[5];

#ifdef SUPPORT_IP6LINKS
		if (params[6].find_first_of(":") != std::string::npos)
			_new->SetSockAddr(AF_INET6, params[6].c_str(), 0);
		else
#endif
			_new->SetSockAddr(AF_INET, params[6].c_str(), 0);

		Instance->AddGlobalClone(_new);

		this->Instance->SNO->WriteToSnoMask('C',"Client connecting at %s: %s!%s@%s [%s]",_new->server,_new->nick,_new->ident,_new->host, _new->GetIPString());

		params[7] = ":" + params[7];
		Utils->DoOneToAllButSender(source,"NICK", params, source);

		// Increment the Source Servers User Count..
		TreeServer* SourceServer = Utils->FindServer(source);
		if (SourceServer)
		{
			Instance->Log(DEBUG,"Found source server of %s",_new->nick);
			SourceServer->AddUserCount();
		}

		FOREACH_MOD_I(Instance,I_OnPostConnect,OnPostConnect(_new));

		return true;
	}

	/** Send one or more FJOINs for a channel of users.
	 * If the length of a single line is more than 480-NICKMAX
	 * in length, it is split over multiple lines.
	 */
	void SendFJoins(TreeServer* Current, chanrec* c)
	{
		std::string buffer;

		Instance->Log(DEBUG,"Sending FJOINs to other server for %s",c->name);
		char list[MAXBUF];
		std::string individual_halfops = std::string(":")+this->Instance->Config->ServerName+" FMODE "+c->name+" "+ConvToStr(c->age);
		
		size_t dlen, curlen;
		dlen = curlen = snprintf(list,MAXBUF,":%s FJOIN %s %lu",this->Instance->Config->ServerName,c->name,(unsigned long)c->age);
		int numusers = 0;
		char* ptr = list + dlen;

		CUList *ulist = c->GetUsers();
		std::string modes = "";
		std::string params = "";

		for (CUList::iterator i = ulist->begin(); i != ulist->end(); i++)
		{
			// The first parameter gets a : before it
			size_t ptrlen = snprintf(ptr, MAXBUF, " %s%s,%s", !numusers ? ":" : "", c->GetAllPrefixChars(i->second), i->second->nick);

			curlen += ptrlen;
			ptr += ptrlen;

			numusers++;

			if (curlen > (480-NICKMAX))
			{
				buffer.append(list).append("\r\n");

				dlen = curlen = snprintf(list,MAXBUF,":%s FJOIN %s %lu",this->Instance->Config->ServerName,c->name,(unsigned long)c->age);
				ptr = list + dlen;
				ptrlen = 0;
				numusers = 0;
			}
		}

		if (numusers)
			buffer.append(list).append("\r\n");

		/* Sorry for the hax. Because newly created channels assume +nt,
		 * if this channel doesnt have +nt, explicitly send -n and -t for the missing modes.
		 */
		bool inverted = false;
		if (!c->IsModeSet('n'))
		{
			modes.append("-n");
			inverted = true;
		}
		if (!c->IsModeSet('t'))
		{
			modes.append("-t");
			inverted = true;
		}
		if (inverted)
		{
			modes.append("+");
		}

		for (BanList::iterator b = c->bans.begin(); b != c->bans.end(); b++)
		{
			modes.append("b");
			params.append(" ").append(b->data);

			if (params.length() >= MAXMODES)
			{
				/* Wrap at MAXMODES */
				buffer.append(":").append(this->Instance->Config->ServerName).append(" FMODE ").append(c->name).append(" ").append(ConvToStr(c->age)).append(" +").append(modes).append(params).append("\r\n");
				modes = "";
				params = "";
			}
		}

		buffer.append(":").append(this->Instance->Config->ServerName).append(" FMODE ").append(c->name).append(" ").append(ConvToStr(c->age)).append(" +").append(c->ChanModes(true));

		/* Only send these if there are any */
		if (!modes.empty())
			buffer.append("\r\n").append(":").append(this->Instance->Config->ServerName).append(" FMODE ").append(c->name).append(" ").append(ConvToStr(c->age)).append(" +").append(modes).append(params);

		this->WriteLine(buffer);
	}

	/** Send G, Q, Z and E lines */
	void SendXLines(TreeServer* Current)
	{
		char data[MAXBUF];
		std::string buffer;
		std::string n = this->Instance->Config->ServerName;
		const char* sn = n.c_str();

		/* Yes, these arent too nice looking, but they get the job done */
		for (std::vector<ZLine*>::iterator i = Instance->XLines->zlines.begin(); i != Instance->XLines->zlines.end(); i++)
		{
			snprintf(data,MAXBUF,":%s ADDLINE Z %s %s %lu %lu :%s\r\n",sn,(*i)->ipaddr,(*i)->source,(unsigned long)(*i)->set_time,(unsigned long)(*i)->duration,(*i)->reason);
			buffer.append(data);
		}
		for (std::vector<QLine*>::iterator i = Instance->XLines->qlines.begin(); i != Instance->XLines->qlines.end(); i++)
		{
			snprintf(data,MAXBUF,":%s ADDLINE Q %s %s %lu %lu :%s\r\n",sn,(*i)->nick,(*i)->source,(unsigned long)(*i)->set_time,(unsigned long)(*i)->duration,(*i)->reason);
			buffer.append(data);
		}
		for (std::vector<GLine*>::iterator i = Instance->XLines->glines.begin(); i != Instance->XLines->glines.end(); i++)
		{
			snprintf(data,MAXBUF,":%s ADDLINE G %s@%s %s %lu %lu :%s\r\n",sn,(*i)->identmask,(*i)->hostmask,(*i)->source,(unsigned long)(*i)->set_time,(unsigned long)(*i)->duration,(*i)->reason);
			buffer.append(data);
		}
		for (std::vector<ELine*>::iterator i = Instance->XLines->elines.begin(); i != Instance->XLines->elines.end(); i++)
		{
			snprintf(data,MAXBUF,":%s ADDLINE E %s@%s %s %lu %lu :%s\r\n",sn,(*i)->identmask,(*i)->hostmask,(*i)->source,(unsigned long)(*i)->set_time,(unsigned long)(*i)->duration,(*i)->reason);
			buffer.append(data);
		}
		for (std::vector<ZLine*>::iterator i = Instance->XLines->pzlines.begin(); i != Instance->XLines->pzlines.end(); i++)
		{
			snprintf(data,MAXBUF,":%s ADDLINE Z %s %s %lu %lu :%s\r\n",sn,(*i)->ipaddr,(*i)->source,(unsigned long)(*i)->set_time,(unsigned long)(*i)->duration,(*i)->reason);
			buffer.append(data);
		}
		for (std::vector<QLine*>::iterator i = Instance->XLines->pqlines.begin(); i != Instance->XLines->pqlines.end(); i++)
		{
			snprintf(data,MAXBUF,":%s ADDLINE Q %s %s %lu %lu :%s\r\n",sn,(*i)->nick,(*i)->source,(unsigned long)(*i)->set_time,(unsigned long)(*i)->duration,(*i)->reason);
			buffer.append(data);
		}
		for (std::vector<GLine*>::iterator i = Instance->XLines->pglines.begin(); i != Instance->XLines->pglines.end(); i++)
		{
			snprintf(data,MAXBUF,":%s ADDLINE G %s@%s %s %lu %lu :%s\r\n",sn,(*i)->identmask,(*i)->hostmask,(*i)->source,(unsigned long)(*i)->set_time,(unsigned long)(*i)->duration,(*i)->reason);
			buffer.append(data);
		}
		for (std::vector<ELine*>::iterator i = Instance->XLines->pelines.begin(); i != Instance->XLines->pelines.end(); i++)
		{
			snprintf(data,MAXBUF,":%s ADDLINE E %s@%s %s %lu %lu :%s\r\n",sn,(*i)->identmask,(*i)->hostmask,(*i)->source,(unsigned long)(*i)->set_time,(unsigned long)(*i)->duration,(*i)->reason);
			buffer.append(data);
		}

		if (!buffer.empty())
			this->WriteLine(buffer);
	}

	/** Send channel modes and topics */
	void SendChannelModes(TreeServer* Current)
	{
		char data[MAXBUF];
		std::deque<std::string> list;
		std::string n = this->Instance->Config->ServerName;
		const char* sn = n.c_str();
		for (chan_hash::iterator c = this->Instance->chanlist->begin(); c != this->Instance->chanlist->end(); c++)
		{
			SendFJoins(Current, c->second);
			if (*c->second->topic)
			{
				snprintf(data,MAXBUF,":%s FTOPIC %s %lu %s :%s",sn,c->second->name,(unsigned long)c->second->topicset,c->second->setby,c->second->topic);
				this->WriteLine(data);
			}
			FOREACH_MOD_I(this->Instance,I_OnSyncChannel,OnSyncChannel(c->second,(Module*)Utils->Creator,(void*)this));
			list.clear();
			c->second->GetExtList(list);
			for (unsigned int j = 0; j < list.size(); j++)
			{
				FOREACH_MOD_I(this->Instance,I_OnSyncChannelMetaData,OnSyncChannelMetaData(c->second,(Module*)Utils->Creator,(void*)this,list[j]));
			}
		}
	}

	/** send all users and their oper state/modes */
	void SendUsers(TreeServer* Current)
	{
		char data[MAXBUF];
		std::deque<std::string> list;
		std::string dataline;
		for (user_hash::iterator u = this->Instance->clientlist->begin(); u != this->Instance->clientlist->end(); u++)
		{
			if (u->second->registered == REG_ALL)
			{
				snprintf(data,MAXBUF,":%s NICK %lu %s %s %s %s +%s %s :%s",u->second->server,(unsigned long)u->second->age,u->second->nick,u->second->host,u->second->dhost,u->second->ident,u->second->FormatModes(),u->second->GetIPString(),u->second->fullname);
				this->WriteLine(data);
				if (*u->second->oper)
				{
					snprintf(data,MAXBUF,":%s OPERTYPE %s", u->second->nick, u->second->oper);
					this->WriteLine(data);
				}
				if (*u->second->awaymsg)
				{
					snprintf(data,MAXBUF,":%s AWAY :%s", u->second->nick, u->second->awaymsg);
					this->WriteLine(data);
				}
				FOREACH_MOD_I(this->Instance,I_OnSyncUser,OnSyncUser(u->second,(Module*)Utils->Creator,(void*)this));
				list.clear();
				u->second->GetExtList(list);
				for (unsigned int j = 0; j < list.size(); j++)
				{
					FOREACH_MOD_I(this->Instance,I_OnSyncUserMetaData,OnSyncUserMetaData(u->second,(Module*)Utils->Creator,(void*)this,list[j]));
				}
			}
		}
	}

	/** This function is called when we want to send a netburst to a local
	 * server. There is a set order we must do this, because for example
	 * users require their servers to exist, and channels require their
	 * users to exist. You get the idea.
	 */
	void DoBurst(TreeServer* s)
	{
		std::string burst = "BURST "+ConvToStr(Instance->Time(true));
		std::string endburst = "ENDBURST";
		// Because by the end of the netburst, it  could be gone!
		std::string name = s->GetName();
		this->Instance->SNO->WriteToSnoMask('l',"Bursting to \2"+name+"\2.");
		this->WriteLine(burst);
		/* send our version string */
		this->WriteLine(std::string(":")+this->Instance->Config->ServerName+" VERSION :"+this->Instance->GetVersionString());
		/* Send server tree */
		this->SendServers(Utils->TreeRoot,s,1);
		/* Send users and their oper status */
		this->SendUsers(s);
		/* Send everything else (channel modes, xlines etc) */
		this->SendChannelModes(s);
		this->SendXLines(s);		
		FOREACH_MOD_I(this->Instance,I_OnSyncOtherMetaData,OnSyncOtherMetaData((Module*)Utils->Creator,(void*)this));
		this->WriteLine(endburst);
		this->Instance->SNO->WriteToSnoMask('l',"Finished bursting to \2"+name+"\2.");
	}

	/** This function is called when we receive data from a remote
	 * server. We buffer the data in a std::string (it doesnt stay
	 * there for long), reading using InspSocket::Read() which can
	 * read up to 16 kilobytes in one operation.
	 *
	 * IF THIS FUNCTION RETURNS FALSE, THE CORE CLOSES AND DELETES
	 * THE SOCKET OBJECT FOR US.
	 */
	virtual bool OnDataReady()
	{
		char* data = this->Read();
		/* Check that the data read is a valid pointer and it has some content */
		if (data && *data)
		{
			this->in_buffer.append(data);
			/* While there is at least one new line in the buffer,
			 * do something useful (we hope!) with it.
			 */
			while (in_buffer.find("\n") != std::string::npos)
			{
				std::string ret = in_buffer.substr(0,in_buffer.find("\n")-1);
				in_buffer = in_buffer.substr(in_buffer.find("\n")+1,in_buffer.length()-in_buffer.find("\n"));
				/* Use rfind here not find, as theres more
				 * chance of the \r being near the end of the
				 * string, not the start.
				 */
				if (ret.find("\r") != std::string::npos)
					ret = in_buffer.substr(0,in_buffer.find("\r")-1);
				/* Process this one, abort if it
				 * didnt return true.
				 */
				if (!this->ProcessLine(ret))
				{
					return false;
				}
			}
			return true;
		}
		/* EAGAIN returns an empty but non-NULL string, so this
		 * evaluates to TRUE for EAGAIN but to FALSE for EOF.
		 */
		return (data && !*data);
	}

	int WriteLine(std::string line)
	{
		Instance->Log(DEBUG,"OUT: %s",line.c_str());
		line.append("\r\n");
		return this->Write(line);
	}

	/* Handle ERROR command */
	bool Error(std::deque<std::string> &params)
	{
		if (params.size() < 1)
			return false;
		this->Instance->SNO->WriteToSnoMask('l',"ERROR from %s: %s",(InboundServerName != "" ? InboundServerName.c_str() : myhost.c_str()),params[0].c_str());
		/* we will return false to cause the socket to close. */
		return false;
	}

	/** remote MOTD. leet, huh? */
	bool Motd(const std::string &prefix, std::deque<std::string> &params)
	{
		if (params.size() > 0)
		{
			if (this->Instance->MatchText(this->Instance->Config->ServerName, params[0]))
			{
				/* It's for our server */
				string_list results;
				userrec* source = this->Instance->FindNick(prefix);

				if (source)
				{
					std::deque<std::string> par;
					par.push_back(prefix);
					par.push_back("");

					if (!Instance->Config->MOTD.size())
					{
						par[1] = std::string("::")+Instance->Config->ServerName+" 422 "+source->nick+" :Message of the day file is missing.";
						Utils->DoOneToOne(this->Instance->Config->ServerName, "PUSH",par, source->server);
						return true;
					}
   
					par[1] = std::string("::")+Instance->Config->ServerName+" 375 "+source->nick+" :"+Instance->Config->ServerName+" message of the day";
					Utils->DoOneToOne(this->Instance->Config->ServerName, "PUSH",par, source->server);
   
					for (unsigned int i = 0; i < Instance->Config->MOTD.size(); i++)
					{
						par[1] = std::string("::")+Instance->Config->ServerName+" 372 "+source->nick+" :- "+Instance->Config->MOTD[i];
						Utils->DoOneToOne(this->Instance->Config->ServerName, "PUSH",par, source->server);
					}
     
					par[1] = std::string("::")+Instance->Config->ServerName+" 376 "+source->nick+" End of message of the day.";
					Utils->DoOneToOne(this->Instance->Config->ServerName, "PUSH",par, source->server);
				}
			}
			else
			{
				/* Pass it on */
				userrec* source = this->Instance->FindNick(prefix);
				if (source)
					Utils->DoOneToOne(prefix, "MOTD", params, params[0]);
			}
		}
		return true;
	}

	/** remote ADMIN. leet, huh? */
	bool Admin(const std::string &prefix, std::deque<std::string> &params)
	{
		if (params.size() > 0)
		{
			if (this->Instance->MatchText(this->Instance->Config->ServerName, params[0]))
			{
				/* It's for our server */
				string_list results;
				userrec* source = this->Instance->FindNick(prefix);

				if (source)
				{
					std::deque<std::string> par;
					par.push_back(prefix);
					par.push_back("");

					par[1] = std::string("::")+Instance->Config->ServerName+" 256 "+source->nick+" :Administrative info for "+Instance->Config->ServerName;
					Utils->DoOneToOne(this->Instance->Config->ServerName, "PUSH",par, source->server);

					par[1] = std::string("::")+Instance->Config->ServerName+" 257 "+source->nick+" :Name     - "+Instance->Config->AdminName;
					Utils->DoOneToOne(this->Instance->Config->ServerName, "PUSH",par, source->server);

					par[1] = std::string("::")+Instance->Config->ServerName+" 258 "+source->nick+" :Nickname - "+Instance->Config->AdminNick;
					Utils->DoOneToOne(this->Instance->Config->ServerName, "PUSH",par, source->server);

					par[1] = std::string("::")+Instance->Config->ServerName+" 258 "+source->nick+" :E-Mail   - "+Instance->Config->AdminEmail;
					Utils->DoOneToOne(this->Instance->Config->ServerName, "PUSH",par, source->server);
				}
			}
			else
			{
				/* Pass it on */
				userrec* source = this->Instance->FindNick(prefix);
				if (source)
					Utils->DoOneToOne(prefix, "ADMIN", params, params[0]);
			}
		}
		return true;
	}

	bool Stats(const std::string &prefix, std::deque<std::string> &params)
	{
		/* Get the reply to a STATS query if it matches this servername,
		 * and send it back as a load of PUSH queries
		 */
		if (params.size() > 1)
		{
			if (this->Instance->MatchText(this->Instance->Config->ServerName, params[1]))
			{
				/* It's for our server */
				string_list results;
				userrec* source = this->Instance->FindNick(prefix);
				if (source)
				{
					std::deque<std::string> par;
					par.push_back(prefix);
					par.push_back("");
					DoStats(this->Instance, *(params[0].c_str()), source, results);
					for (size_t i = 0; i < results.size(); i++)
					{
						par[1] = "::" + results[i];
						Utils->DoOneToOne(this->Instance->Config->ServerName, "PUSH",par, source->server);
					}
				}
			}
			else
			{
				/* Pass it on */
				userrec* source = this->Instance->FindNick(prefix);
				if (source)
					Utils->DoOneToOne(prefix, "STATS", params, params[1]);
			}
		}
		return true;
	}


	/** Because the core won't let users or even SERVERS set +o,
	 * we use the OPERTYPE command to do this.
	 */
	bool OperType(const std::string &prefix, std::deque<std::string> &params)
	{
		if (params.size() != 1)
		{
			Instance->Log(DEBUG,"Received invalid oper type from %s",prefix.c_str());
			return true;
		}
		std::string opertype = params[0];
		userrec* u = this->Instance->FindNick(prefix);
		if (u)
		{
			u->modes[UM_OPERATOR] = 1;
			strlcpy(u->oper,opertype.c_str(),NICKMAX-1);
			Utils->DoOneToAllButSender(u->nick,"OPERTYPE",params,u->server);
			this->Instance->SNO->WriteToSnoMask('o',"From %s: User %s (%s@%s) is now an IRC operator of type %s",u->server, u->nick,u->ident,u->host,irc::Spacify(opertype.c_str()));

		}
		return true;
	}

	/** Because Andy insists that services-compatible servers must
	 * implement SVSNICK and SVSJOIN, that's exactly what we do :p
	 */
	bool ForceNick(const std::string &prefix, std::deque<std::string> &params)
	{
		if (params.size() < 3)
			return true;

		userrec* u = this->Instance->FindNick(params[0]);

		if (u)
		{
			Utils->DoOneToAllButSender(prefix,"SVSNICK",params,prefix);
			if (IS_LOCAL(u))
			{
				std::deque<std::string> par;
				par.push_back(params[1]);
				/* This is not required as one is sent in OnUserPostNick below
				 */
				//Utils->DoOneToMany(u->nick,"NICK",par);
				if (!u->ForceNickChange(params[1].c_str()))
				{
					userrec::QuitUser(this->Instance, u, "Nickname collision");
					return true;
				}
				u->age = atoi(params[2].c_str());
			}
		}
		return true;
	}

	/*
	 * Remote SQUIT (RSQUIT). Routing works similar to SVSNICK: Route it to the server that the target is connected to locally,
	 * then let that server do the dirty work (squit it!). Example:
	 * A -> B -> C -> D: oper on A squits D, A routes to B, B routes to C, C notices D connected locally, kills it. -- w00t
	 */
	bool RemoteSquit(const std::string &prefix, std::deque<std::string> &params)
	{
		/* ok.. :w00t RSQUIT jupe.barafranca.com :reason here */
		if (params.size() < 2)
			return true;

		TreeServer* s = Utils->FindServerMask(params[0]);

		if (s)
		{
			if (s == Utils->TreeRoot)
			{
				this->Instance->SNO->WriteToSnoMask('l',"What the fuck, I recieved a remote SQUIT for myself? :< (from %s", prefix.c_str());
				return true;
			}

			TreeSocket* sock = s->GetSocket();

			if (sock)
			{
				/* it's locally connected, KILL IT! */
				Instance->Log(DEBUG,"Splitting server %s",s->GetName().c_str());
				Instance->SNO->WriteToSnoMask('l',"RSQUIT: Server \002%s\002 removed from network by %s: %s", params[0].c_str(), prefix.c_str(), params[1].c_str());
				sock->Squit(s,"Server quit by " + prefix + ": " + params[1]);
				Instance->SE->DelFd(sock);
				sock->Close();
				delete sock;
			}
			else
			{
				/* route the rsquit */
				params[1] = ":" + params[1];
				Utils->DoOneToOne(prefix, "RSQUIT", params, params[0]);
			}
		}
		else
		{
			/* mother fucker! it doesn't exist */
		}

		return true;
	}

	bool ServiceJoin(const std::string &prefix, std::deque<std::string> &params)
	{
		if (params.size() < 2)
			return true;

		userrec* u = this->Instance->FindNick(params[0]);

		if (u)
		{
			chanrec::JoinUser(this->Instance, u, params[1].c_str(), false);
			Utils->DoOneToAllButSender(prefix,"SVSJOIN",params,prefix);
		}
		return true;
	}

	bool RemoteRehash(const std::string &prefix, std::deque<std::string> &params)
	{
		if (params.size() < 1)
			return false;

		std::string servermask = params[0];

		if (this->Instance->MatchText(this->Instance->Config->ServerName,servermask))
		{
			this->Instance->SNO->WriteToSnoMask('l',"Remote rehash initiated by \002"+prefix+"\002.");
			this->Instance->RehashServer();
			Utils->ReadConfiguration(false);
			InitializeDisabledCommands(Instance->Config->DisabledCommands, Instance);
		}
		Utils->DoOneToAllButSender(prefix,"REHASH",params,prefix);
		return true;
	}

	bool RemoteKill(const std::string &prefix, std::deque<std::string> &params)
	{
		if (params.size() != 2)
			return true;

		std::string nick = params[0];
		userrec* u = this->Instance->FindNick(prefix);
		userrec* who = this->Instance->FindNick(nick);

		if (who)
		{
			/* Prepend kill source, if we don't have one */
			std::string sourceserv = prefix;
			if (u)
			{
				sourceserv = u->server;
			}
			if (*(params[1].c_str()) != '[')
			{
				params[1] = "[" + sourceserv + "] Killed (" + params[1] +")";
			}
			std::string reason = params[1];
			params[1] = ":" + params[1];
			Utils->DoOneToAllButSender(prefix,"KILL",params,sourceserv);
			who->Write(":%s KILL %s :%s (%s)", sourceserv.c_str(), who->nick, sourceserv.c_str(), reason.c_str());
			userrec::QuitUser(this->Instance,who,reason);
		}
		return true;
	}

	bool LocalPong(const std::string &prefix, std::deque<std::string> &params)
	{
		if (params.size() < 1)
			return true;

		if (params.size() == 1)
		{
			TreeServer* ServerSource = Utils->FindServer(prefix);
			if (ServerSource)
			{
				ServerSource->SetPingFlag();
			}
		}
		else
		{
			std::string forwardto = params[1];
			if (forwardto == this->Instance->Config->ServerName)
			{
				/*
				 * this is a PONG for us
				 * if the prefix is a user, check theyre local, and if they are,
				 * dump the PONG reply back to their fd. If its a server, do nowt.
				 * Services might want to send these s->s, but we dont need to yet.
				 */
				userrec* u = this->Instance->FindNick(prefix);

				if (u)
				{
					u->WriteServ("PONG %s %s",params[0].c_str(),params[1].c_str());
				}
			}
			else
			{
				// not for us, pass it on :)
				Utils->DoOneToOne(prefix,"PONG",params,forwardto);
			}
		}

		return true;
	}
	
	bool MetaData(const std::string &prefix, std::deque<std::string> &params)
	{
		if (params.size() < 3)
			return true;

		TreeServer* ServerSource = Utils->FindServer(prefix);

		if (ServerSource)
		{
			if (params[0] == "*")
			{
				FOREACH_MOD_I(this->Instance,I_OnDecodeMetaData,OnDecodeMetaData(TYPE_OTHER,NULL,params[1],params[2]));
			}
			else if (*(params[0].c_str()) == '#')
			{
				chanrec* c = this->Instance->FindChan(params[0]);
				if (c)
				{
					FOREACH_MOD_I(this->Instance,I_OnDecodeMetaData,OnDecodeMetaData(TYPE_CHANNEL,c,params[1],params[2]));
				}
			}
			else if (*(params[0].c_str()) != '#')
			{
				userrec* u = this->Instance->FindNick(params[0]);
				if (u)
				{
					FOREACH_MOD_I(this->Instance,I_OnDecodeMetaData,OnDecodeMetaData(TYPE_USER,u,params[1],params[2]));
				}
			}
		}

		params[2] = ":" + params[2];
		Utils->DoOneToAllButSender(prefix,"METADATA",params,prefix);
		return true;
	}

	bool ServerVersion(const std::string &prefix, std::deque<std::string> &params)
	{
		if (params.size() < 1)
			return true;

		TreeServer* ServerSource = Utils->FindServer(prefix);

		if (ServerSource)
		{
			ServerSource->SetVersion(params[0]);
		}
		params[0] = ":" + params[0];
		Utils->DoOneToAllButSender(prefix,"VERSION",params,prefix);
		return true;
	}

	bool ChangeHost(const std::string &prefix, std::deque<std::string> &params)
	{
		if (params.size() < 1)
			return true;

		userrec* u = this->Instance->FindNick(prefix);

		if (u)
		{
			u->ChangeDisplayedHost(params[0].c_str());
			Utils->DoOneToAllButSender(prefix,"FHOST",params,u->server);
		}
		return true;
	}

	bool AddLine(const std::string &prefix, std::deque<std::string> &params)
	{
		if (params.size() < 6)
			return true;

		bool propogate = false;

		switch (*(params[0].c_str()))
		{
			case 'Z':
				propogate = Instance->XLines->add_zline(atoi(params[4].c_str()), params[2].c_str(), params[5].c_str(), params[1].c_str());
				Instance->XLines->zline_set_creation_time(params[1].c_str(), atoi(params[3].c_str()));
			break;
			case 'Q':
				propogate = Instance->XLines->add_qline(atoi(params[4].c_str()), params[2].c_str(), params[5].c_str(), params[1].c_str());
				Instance->XLines->qline_set_creation_time(params[1].c_str(), atoi(params[3].c_str()));
			break;
			case 'E':
				propogate = Instance->XLines->add_eline(atoi(params[4].c_str()), params[2].c_str(), params[5].c_str(), params[1].c_str());
				Instance->XLines->eline_set_creation_time(params[1].c_str(), atoi(params[3].c_str()));
			break;
			case 'G':
				propogate = Instance->XLines->add_gline(atoi(params[4].c_str()), params[2].c_str(), params[5].c_str(), params[1].c_str());
				Instance->XLines->gline_set_creation_time(params[1].c_str(), atoi(params[3].c_str()));
			break;
			case 'K':
				propogate = Instance->XLines->add_kline(atoi(params[4].c_str()), params[2].c_str(), params[5].c_str(), params[1].c_str());
			break;
			default:
				/* Just in case... */
				this->Instance->SNO->WriteToSnoMask('x',"\2WARNING\2: Invalid xline type '"+params[0]+"' sent by server "+prefix+", ignored!");
				propogate = false;
			break;
		}

		/* Send it on its way */
		if (propogate)
		{
			if (atoi(params[4].c_str()))
			{
				this->Instance->SNO->WriteToSnoMask('x',"%s Added %cLINE on %s to expire in %lu seconds (%s).",prefix.c_str(),*(params[0].c_str()),params[1].c_str(),atoi(params[4].c_str()),params[5].c_str());
			}
			else
			{
				this->Instance->SNO->WriteToSnoMask('x',"%s Added permenant %cLINE on %s (%s).",prefix.c_str(),*(params[0].c_str()),params[1].c_str(),params[5].c_str());
			}
			params[5] = ":" + params[5];
			Utils->DoOneToAllButSender(prefix,"ADDLINE",params,prefix);
		}
		if (!this->bursting)
		{
			Instance->Log(DEBUG,"Applying lines...");
			Instance->XLines->apply_lines(APPLY_ZLINES|APPLY_GLINES|APPLY_QLINES);
		}
		return true;
	}

	bool ChangeName(const std::string &prefix, std::deque<std::string> &params)
	{
		if (params.size() < 1)
			return true;

		userrec* u = this->Instance->FindNick(prefix);

		if (u)
		{
			u->ChangeName(params[0].c_str());
			params[0] = ":" + params[0];
			Utils->DoOneToAllButSender(prefix,"FNAME",params,u->server);
		}
		return true;
	}

	bool Whois(const std::string &prefix, std::deque<std::string> &params)
	{
		if (params.size() < 1)
			return true;

		Instance->Log(DEBUG,"In IDLE command");
		userrec* u = this->Instance->FindNick(prefix);

		if (u)
		{
			Instance->Log(DEBUG,"USER EXISTS: %s",u->nick);
			// an incoming request
			if (params.size() == 1)
			{
				userrec* x = this->Instance->FindNick(params[0]);
				if ((x) && (IS_LOCAL(x)))
				{
					userrec* x = this->Instance->FindNick(params[0]);
					char signon[MAXBUF];
					char idle[MAXBUF];

					snprintf(signon,MAXBUF,"%lu",(unsigned long)x->signon);
					snprintf(idle,MAXBUF,"%lu",(unsigned long)abs((x->idle_lastmsg)-Instance->Time(true)));
					std::deque<std::string> par;
					par.push_back(prefix);
					par.push_back(signon);
					par.push_back(idle);
					// ours, we're done, pass it BACK
					Utils->DoOneToOne(params[0],"IDLE",par,u->server);
				}
				else
				{
					// not ours pass it on
					Utils->DoOneToOne(prefix,"IDLE",params,x->server);
				}
			}
			else if (params.size() == 3)
			{
				std::string who_did_the_whois = params[0];
				userrec* who_to_send_to = this->Instance->FindNick(who_did_the_whois);
				if ((who_to_send_to) && (IS_LOCAL(who_to_send_to)))
				{
					// an incoming reply to a whois we sent out
					std::string nick_whoised = prefix;
					unsigned long signon = atoi(params[1].c_str());
					unsigned long idle = atoi(params[2].c_str());
					if ((who_to_send_to) && (IS_LOCAL(who_to_send_to)))
						do_whois(this->Instance,who_to_send_to,u,signon,idle,nick_whoised.c_str());
				}
				else
				{
					// not ours, pass it on
					Utils->DoOneToOne(prefix,"IDLE",params,who_to_send_to->server);
				}
			}
		}
		return true;
	}

	bool Push(const std::string &prefix, std::deque<std::string> &params)
	{
		if (params.size() < 2)
			return true;

		userrec* u = this->Instance->FindNick(params[0]);

		if (!u)
			return true;

		if (IS_LOCAL(u))
		{
			u->Write(params[1]);
		}
		else
		{
			// continue the raw onwards
			params[1] = ":" + params[1];
			Utils->DoOneToOne(prefix,"PUSH",params,u->server);
		}
		return true;
	}

	bool HandleSetTime(const std::string &prefix, std::deque<std::string> &params)
	{
		if (!params.size() || !Utils->EnableTimeSync)
			return true;
		
		bool force = false;
		
		if ((params.size() == 2) && (params[1] == "FORCE"))
			force = true;
		
		time_t rts = atoi(params[0].c_str());
		time_t us = Instance->Time(true);
		
		if (rts == us)
		{
			Instance->Log(DEBUG, "Timestamp from %s is equal", prefix.c_str());
			
			Utils->DoOneToAllButSender(prefix, "TIMESET", params, prefix);
		}
		else if (force || (rts < us))
		{
			int old = Instance->SetTimeDelta(rts - us);
			Instance->Log(DEBUG, "%s TS (diff %d) from %s applied (old delta was %d)", (force) ? "Forced" : "Lower", rts - us, prefix.c_str(), old);
			
			Utils->DoOneToAllButSender(prefix, "TIMESET", params, prefix);
		}
		else
		{
			Instance->Log(DEBUG, "Higher TS (diff %d) from %s overridden", us - rts, prefix.c_str());
			
			std::deque<std::string> oparams;
			oparams.push_back(ConvToStr(us));
			
			Utils->DoOneToMany(prefix, "TIMESET", oparams);
		}
		
		return true;
	}

	bool Time(const std::string &prefix, std::deque<std::string> &params)
	{
		// :source.server TIME remote.server sendernick
		// :remote.server TIME source.server sendernick TS
		if (params.size() == 2)
		{
			// someone querying our time?
			if (this->Instance->Config->ServerName == params[0])
			{
				userrec* u = this->Instance->FindNick(params[1]);
				if (u)
				{
					params.push_back(ConvToStr(Instance->Time(false)));
					params[0] = prefix;
					Utils->DoOneToOne(this->Instance->Config->ServerName,"TIME",params,params[0]);
				}
			}
			else
			{
				// not us, pass it on
				userrec* u = this->Instance->FindNick(params[1]);
				if (u)
					Utils->DoOneToOne(prefix,"TIME",params,params[0]);
			}
		}
		else if (params.size() == 3)
		{
			// a response to a previous TIME
			userrec* u = this->Instance->FindNick(params[1]);
			if ((u) && (IS_LOCAL(u)))
			{
			time_t rawtime = atol(params[2].c_str());
			struct tm * timeinfo;
			timeinfo = localtime(&rawtime);
				char tms[26];
				snprintf(tms,26,"%s",asctime(timeinfo));
				tms[24] = 0;
			u->WriteServ("391 %s %s :%s",u->nick,prefix.c_str(),tms);
			}
			else
			{
				if (u)
					Utils->DoOneToOne(prefix,"TIME",params,u->server);
			}
		}
		return true;
	}
	
	bool LocalPing(const std::string &prefix, std::deque<std::string> &params)
	{
		if (params.size() < 1)
			return true;

		if (params.size() == 1)
		{
			std::string stufftobounce = params[0];
			this->WriteLine(std::string(":")+this->Instance->Config->ServerName+" PONG "+stufftobounce);
			return true;
		}
		else
		{
			std::string forwardto = params[1];
			if (forwardto == this->Instance->Config->ServerName)
			{
				// this is a ping for us, send back PONG to the requesting server
				params[1] = params[0];
				params[0] = forwardto;
				Utils->DoOneToOne(forwardto,"PONG",params,params[1]);
			}
			else
			{
				// not for us, pass it on :)
				Utils->DoOneToOne(prefix,"PING",params,forwardto);
			}
			return true;
		}
	}

	bool RemoveStatus(const std::string &prefix, std::deque<std::string> &params)
	{
		if (params.size() < 1)
			return true;

		chanrec* c = Instance->FindChan(params[0]);

		if (c)
		{
			irc::modestacker modestack(false);
			CUList *ulist = c->GetUsers();
			const char* y[127];
			std::deque<std::string> stackresult;
			std::string x;

			for (CUList::iterator i = ulist->begin(); i != ulist->end(); i++)
			{
				std::string modesequence = Instance->Modes->ModeString(i->second, c);
				if (modesequence.length())
				{
					Instance->Log(DEBUG,"Mode sequence = '%s'",modesequence.c_str());
					irc::spacesepstream sep(modesequence);
					std::string modeletters = sep.GetToken();
					Instance->Log(DEBUG,"Mode letters = '%s'",modeletters.c_str());
					
					while (!modeletters.empty())
					{
						char mletter = *(modeletters.begin());
						modestack.Push(mletter,sep.GetToken());
						Instance->Log(DEBUG,"Push letter = '%c'",mletter);
						modeletters.erase(modeletters.begin());
						Instance->Log(DEBUG,"Mode letters = '%s'",modeletters.c_str());
					}
				}
			}

			while (modestack.GetStackedLine(stackresult))
			{
				Instance->Log(DEBUG,"Stacked line size %d",stackresult.size());
				stackresult.push_front(ConvToStr(c->age));
				stackresult.push_front(c->name);
				Utils->DoOneToMany(Instance->Config->ServerName, "FMODE", stackresult);
				stackresult.erase(stackresult.begin() + 1);
				Instance->Log(DEBUG,"Stacked items:");
				for (size_t z = 0; z < stackresult.size(); z++)
				{
					y[z] = stackresult[z].c_str();
					Instance->Log(DEBUG,"\tstackresult[%d]='%s'",z,stackresult[z].c_str());
				}
				userrec* n = new userrec(Instance);
				n->SetFd(FD_MAGIC_NUMBER);
				Instance->SendMode(y, stackresult.size(), n);
				delete n;
			}
		}
		return true;
	}

	bool RemoteServer(const std::string &prefix, std::deque<std::string> &params)
	{
		if (params.size() < 4)
			return false;

		std::string servername = params[0];
		std::string password = params[1];
		// hopcount is not used for a remote server, we calculate this ourselves
		std::string description = params[3];
		TreeServer* ParentOfThis = Utils->FindServer(prefix);

		if (!ParentOfThis)
		{
			this->WriteLine("ERROR :Protocol error - Introduced remote server from unknown server "+prefix);
			return false;
		}
		TreeServer* CheckDupe = Utils->FindServer(servername);
		if (CheckDupe)
		{
			this->WriteLine("ERROR :Server "+servername+" already exists!");
			this->Instance->SNO->WriteToSnoMask('l',"Server connection from \2"+servername+"\2 denied, already exists");
			return false;
		}
		TreeServer* Node = new TreeServer(this->Utils,this->Instance,servername,description,ParentOfThis,NULL);
		ParentOfThis->AddChild(Node);
		params[3] = ":" + params[3];
		Utils->DoOneToAllButSender(prefix,"SERVER",params,prefix);
		this->Instance->SNO->WriteToSnoMask('l',"Server \002"+prefix+"\002 introduced server \002"+servername+"\002 ("+description+")");
		return true;
	}

	bool Outbound_Reply_Server(std::deque<std::string> &params)
	{
		if (params.size() < 4)
			return false;

		irc::string servername = params[0].c_str();
		std::string sname = params[0];
		std::string password = params[1];
		int hops = atoi(params[2].c_str());

		if (hops)
		{
			this->WriteLine("ERROR :Server too far away for authentication");
			this->Instance->SNO->WriteToSnoMask('l',"Server connection from \2"+sname+"\2 denied, server is too far away for authentication");
			return false;
		}
		std::string description = params[3];
		for (std::vector<Link>::iterator x = Utils->LinkBlocks.begin(); x < Utils->LinkBlocks.end(); x++)
		{
			if ((x->Name == servername) && (x->RecvPass == password))
			{
				TreeServer* CheckDupe = Utils->FindServer(sname);
				if (CheckDupe)
				{
					this->WriteLine("ERROR :Server "+sname+" already exists on server "+CheckDupe->GetParent()->GetName()+"!");
					this->Instance->SNO->WriteToSnoMask('l',"Server connection from \2"+sname+"\2 denied, already exists on server "+CheckDupe->GetParent()->GetName());
					return false;
				}
				// Begin the sync here. this kickstarts the
				// other side, waiting in WAIT_AUTH_2 state,
				// into starting their burst, as it shows
				// that we're happy.
				this->LinkState = CONNECTED;
				// we should add the details of this server now
				// to the servers tree, as a child of the root
				// node.
				TreeServer* Node = new TreeServer(this->Utils,this->Instance,sname,description,Utils->TreeRoot,this);
				Utils->TreeRoot->AddChild(Node);
				params[3] = ":" + params[3];
				Utils->DoOneToAllButSender(Utils->TreeRoot->GetName(),"SERVER",params,sname);
				this->bursting = true;
				this->DoBurst(Node);
				return true;
			}
		}
		this->WriteLine("ERROR :Invalid credentials");
		this->Instance->SNO->WriteToSnoMask('l',"Server connection from \2"+sname+"\2 denied, invalid link credentials");
		return false;
	}

	bool Inbound_Server(std::deque<std::string> &params)
	{
		if (params.size() < 4)
			return false;

		irc::string servername = params[0].c_str();
		std::string sname = params[0];
		std::string password = params[1];
		int hops = atoi(params[2].c_str());

		if (hops)
		{
			this->WriteLine("ERROR :Server too far away for authentication");
			this->Instance->SNO->WriteToSnoMask('l',"Server connection from \2"+sname+"\2 denied, server is too far away for authentication");
			return false;
		}
		std::string description = params[3];
		for (std::vector<Link>::iterator x = Utils->LinkBlocks.begin(); x < Utils->LinkBlocks.end(); x++)
		{
			if ((x->Name == servername) && (x->RecvPass == password))
			{
				TreeServer* CheckDupe = Utils->FindServer(sname);
				if (CheckDupe)
				{
					this->WriteLine("ERROR :Server "+sname+" already exists on server "+CheckDupe->GetParent()->GetName()+"!");
					this->Instance->SNO->WriteToSnoMask('l',"Server connection from \2"+sname+"\2 denied, already exists on server "+CheckDupe->GetParent()->GetName());
					return false;
				}
				this->Instance->SNO->WriteToSnoMask('l',"Verified incoming server connection from \002"+sname+"\002["+(x->HiddenFromStats ? "<hidden>" : this->GetIP())+"] ("+description+")");

				if (this->Hook)
				{
					std::string name = InspSocketNameRequest((Module*)Utils->Creator, this->Hook).Send();
					this->Instance->SNO->WriteToSnoMask('l',"Connection from \2"+sname+"\2["+(x->HiddenFromStats ? "<hidden>" : this->GetIP())+"] using transport \2"+name+"\2");
				}

				this->InboundServerName = sname;
				this->InboundDescription = description;
				// this is good. Send our details: Our server name and description and hopcount of 0,
				// along with the sendpass from this block.
				this->WriteLine(std::string("SERVER ")+this->Instance->Config->ServerName+" "+x->SendPass+" 0 :"+this->Instance->Config->ServerDesc);
				// move to the next state, we are now waiting for THEM.
				this->LinkState = WAIT_AUTH_2;
				return true;
			}
		}
		this->WriteLine("ERROR :Invalid credentials");
		this->Instance->SNO->WriteToSnoMask('l',"Server connection from \2"+sname+"\2 denied, invalid link credentials");
		return false;
	}

	void Split(const std::string &line, std::deque<std::string> &n)
	{
		n.clear();
		irc::tokenstream tokens(line);
		std::string param;
		while ((param = tokens.GetToken()) != "")
			n.push_back(param);
		return;
	}

	bool ProcessLine(std::string &line)
	{
		std::deque<std::string> params;
		irc::string command;
		std::string prefix;
		
		line = line.substr(0, line.find_first_of("\r\n"));
		
		if (line.empty())
			return true;
		
		Instance->Log(DEBUG,"IN: %s", line.c_str());
		
		this->Split(line.c_str(),params);
			
		if ((params[0][0] == ':') && (params.size() > 1))
		{
			prefix = params[0].substr(1);
			params.pop_front();
		}

		command = params[0].c_str();
		params.pop_front();

		switch (this->LinkState)
		{
			TreeServer* Node;
			
			case WAIT_AUTH_1:
				// Waiting for SERVER command from remote server. Server initiating
				// the connection sends the first SERVER command, listening server
				// replies with theirs if its happy, then if the initiator is happy,
				// it starts to send its net sync, which starts the merge, otherwise
				// it sends an ERROR.
				if (command == "PASS")
				{
					/* Silently ignored */
				}
				else if (command == "SERVER")
				{
					return this->Inbound_Server(params);
				}
				else if (command == "ERROR")
				{
					return this->Error(params);
				}
				else if (command == "USER")
				{
					this->WriteLine("ERROR :Client connections to this port are prohibited.");
					return false;
				}
				else if (command == "CAPAB")
				{
					return this->Capab(params);
				}
				else if ((command == "U") || (command == "S"))
				{
					this->WriteLine("ERROR :Cannot use the old-style mesh linking protocol with m_spanningtree.so!");
					return false;
				}
				else
				{
					std::string error("ERROR :Invalid command in negotiation phase: ");
					error.append(command.c_str());
					this->WriteLine(error);
					return false;
				}
			break;
			case WAIT_AUTH_2:
				// Waiting for start of other side's netmerge to say they liked our
				// password.
				if (command == "SERVER")
				{
					// cant do this, they sent it to us in the WAIT_AUTH_1 state!
					// silently ignore.
					return true;
				}
				else if ((command == "U") || (command == "S"))
				{
					this->WriteLine("ERROR :Cannot use the old-style mesh linking protocol with m_spanningtree.so!");
					return false;
				}
				else if (command == "BURST")
				{
					if (params.size() && Utils->EnableTimeSync)
					{
						/* If a time stamp is provided, apply synchronization */
						bool force = false;
						time_t them = atoi(params[0].c_str());
						time_t us = Instance->Time(true);
						int delta = them - us;

						if ((params.size() == 2) && (params[1] == "FORCE"))
							force = true;

						if ((delta < -600) || (delta > 600))
						{
							this->Instance->SNO->WriteToSnoMask('l',"\2ERROR\2: Your clocks are out by %d seconds (this is more than ten minutes). Link aborted, \2PLEASE SYNC YOUR CLOCKS!\2",abs(delta));
							this->WriteLine("ERROR :Your clocks are out by "+ConvToStr(abs(delta))+" seconds (this is more than ten minutes). Link aborted, PLEASE SYNC YOUR CLOCKS!");
							return false;
						}
						
						if (us == them)
						{
							this->Instance->Log(DEBUG, "Timestamps are equal; pat yourself on the back");
						}
						else if (force || (us > them))
						{
							this->Instance->Log(DEBUG, "Remote server has lower TS (%d seconds)", them - us);
							this->Instance->SetTimeDelta(them - us);
							// Send this new timestamp to any other servers
							Utils->DoOneToMany(Utils->TreeRoot->GetName(), "TIMESET", params);
						}
						else
						{
							// Override the timestamp
							this->Instance->Log(DEBUG, "We have a higher timestamp (by %d seconds), not updating delta", us - them);
							this->WriteLine(":" + Utils->TreeRoot->GetName() + " TIMESET " + ConvToStr(us));
						}
					}
					this->LinkState = CONNECTED;
					Node = new TreeServer(this->Utils,this->Instance,InboundServerName,InboundDescription,Utils->TreeRoot,this);
					Utils->TreeRoot->AddChild(Node);
					params.clear();
					params.push_back(InboundServerName);
					params.push_back("*");
					params.push_back("1");
					params.push_back(":"+InboundDescription);
					Utils->DoOneToAllButSender(Utils->TreeRoot->GetName(),"SERVER",params,InboundServerName);
					this->bursting = true;
					this->DoBurst(Node);
				}
				else if (command == "ERROR")
				{
					return this->Error(params);
				}
				else if (command == "CAPAB")
				{
					return this->Capab(params);
				}
				
			break;
			case LISTENER:
				this->WriteLine("ERROR :Internal error -- listening socket accepted its own descriptor!!!");
				return false;
			break;
			case CONNECTING:
				if (command == "SERVER")
				{
					// another server we connected to, which was in WAIT_AUTH_1 state,
					// has just sent us their credentials. If we get this far, theyre
					// happy with OUR credentials, and they are now in WAIT_AUTH_2 state.
					// if we're happy with this, we should send our netburst which
					// kickstarts the merge.
					return this->Outbound_Reply_Server(params);
				}
				else if (command == "ERROR")
				{
					return this->Error(params);
				}
			break;
			case CONNECTED:
				// This is the 'authenticated' state, when all passwords
				// have been exchanged and anything past this point is taken
				// as gospel.
				
				if (prefix != "")
				{
					std::string direction = prefix;
					userrec* t = this->Instance->FindNick(prefix);
					if (t)
					{
						direction = t->server;
					}
					TreeServer* route_back_again = Utils->BestRouteTo(direction);
					if ((!route_back_again) || (route_back_again->GetSocket() != this))
					{
						if (route_back_again)
							Instance->Log(DEBUG,"Protocol violation: Fake direction in command '%s' from connection '%s'",line.c_str(),this->GetName().c_str());
						return true;
					}

					/* Fix by brain:
					 * When there is activity on the socket, reset the ping counter so
					 * that we're not wasting bandwidth pinging an active server.
					 */ 
					route_back_again->SetNextPingTime(time(NULL) + 60);
					route_back_again->SetPingFlag();
				}
				
				if (command == "SVSMODE")
				{
					/* Services expects us to implement
					 * SVSMODE. In inspircd its the same as
					 * MODE anyway.
					 */
					command = "MODE";
				}
				std::string target = "";
				/* Yes, know, this is a mess. Its reasonably fast though as we're
				 * working with std::string here.
				 */
				if ((command == "NICK") && (params.size() > 1))
				{
					return this->IntroduceClient(prefix,params);
				}
				else if (command == "FJOIN")
				{
					return this->ForceJoin(prefix,params);
				}
				else if (command == "STATS")
				{
					return this->Stats(prefix, params);
				}
				else if (command == "MOTD")
				{
					return this->Motd(prefix, params);
				}
				else if (command == "ADMIN")
				{
					return this->Admin(prefix, params);
				}
				else if (command == "SERVER")
				{
					return this->RemoteServer(prefix,params);
				}
				else if (command == "ERROR")
				{
					return this->Error(params);
				}
				else if (command == "OPERTYPE")
				{
					return this->OperType(prefix,params);
				}
				else if (command == "FMODE")
				{
					return this->ForceMode(prefix,params);
				}
				else if (command == "KILL")
				{
					return this->RemoteKill(prefix,params);
				}
				else if (command == "FTOPIC")
				{
					return this->ForceTopic(prefix,params);
				}
				else if (command == "REHASH")
				{
					return this->RemoteRehash(prefix,params);
				}
				else if (command == "METADATA")
				{
					return this->MetaData(prefix,params);
				}
				else if (command == "REMSTATUS")
				{
					return this->RemoveStatus(prefix,params);
				}
				else if (command == "PING")
				{
					/*
					 * We just got a ping from a server that's bursting.
					 * This can't be right, so set them to not bursting, and
					 * apply their lines.
					 */
					if (this->bursting)
					{
						this->bursting = false;
						Instance->XLines->apply_lines(APPLY_ZLINES|APPLY_GLINES|APPLY_QLINES);
					}
					if (prefix == "")
					{
						prefix = this->GetName();
					}
					return this->LocalPing(prefix,params);
				}
				else if (command == "PONG")
				{
					/*
					 * We just got a pong from a server that's bursting.
					 * This can't be right, so set them to not bursting, and
					 * apply their lines.
					 */
					if (this->bursting)
					{
						this->bursting = false;
						Instance->XLines->apply_lines(APPLY_ZLINES|APPLY_GLINES|APPLY_QLINES);
					}
					if (prefix == "")
					{
						prefix = this->GetName();
					}
					return this->LocalPong(prefix,params);
				}
				else if (command == "VERSION")
				{
					return this->ServerVersion(prefix,params);
				}
				else if (command == "FHOST")
				{
					return this->ChangeHost(prefix,params);
				}
				else if (command == "FNAME")
				{
					return this->ChangeName(prefix,params);
				}
				else if (command == "ADDLINE")
				{
					return this->AddLine(prefix,params);
				}
				else if (command == "SVSNICK")
				{
					if (prefix == "")
					{
						prefix = this->GetName();
					}
					return this->ForceNick(prefix,params);
				}
				else if (command == "RSQUIT")
				{
					return this->RemoteSquit(prefix, params);
				}
				else if (command == "IDLE")
				{
					return this->Whois(prefix,params);
				}
				else if (command == "PUSH")
				{
					return this->Push(prefix,params);
				}
				else if (command == "TIMESET")
				{
					return this->HandleSetTime(prefix, params);
				}
				else if (command == "TIME")
				{
					return this->Time(prefix,params);
				}
				else if ((command == "KICK") && (Utils->IsServer(prefix)))
				{
					std::string sourceserv = this->myhost;
					if (params.size() == 3)
					{
						userrec* user = this->Instance->FindNick(params[1]);
						chanrec* chan = this->Instance->FindChan(params[0]);
						if (user && chan)
						{
							if (!chan->ServerKickUser(user, params[2].c_str(), false))
								/* Yikes, the channels gone! */
								delete chan;
						}
					}
					if (this->InboundServerName != "")
					{
						sourceserv = this->InboundServerName;
					}
					return Utils->DoOneToAllButSenderRaw(line,sourceserv,prefix,command,params);
				}
				else if (command == "SVSJOIN")
				{
					if (prefix == "")
					{
						prefix = this->GetName();
					}
					return this->ServiceJoin(prefix,params);
				}
				else if (command == "SQUIT")
				{
					if (params.size() == 2)
					{
						this->Squit(Utils->FindServer(params[0]),params[1]);
					}
					return true;
				}
				else if (command == "OPERNOTICE")
				{
					std::string sourceserv = this->myhost;

					if (this->InboundServerName != "")
						sourceserv = this->InboundServerName;

					if (params.size() >= 1)
						Instance->WriteOpers("*** From " + sourceserv + ": " + params[0]);

					return Utils->DoOneToAllButSenderRaw(line, sourceserv, prefix, command, params);
				}
				else if (command == "MODENOTICE")
				{
					std::string sourceserv = this->myhost;
					if (this->InboundServerName != "")
						sourceserv = this->InboundServerName;
					if (params.size() >= 2)
					{
						Instance->WriteMode(params[0].c_str(), WM_AND, "*** From %s: %s", sourceserv.c_str(), params[1].c_str());
					}

					return Utils->DoOneToAllButSenderRaw(line, sourceserv, prefix, command, params);
				}
				else if (command == "SNONOTICE")
				{
					std::string sourceserv = this->myhost;
					if (this->InboundServerName != "")
						sourceserv = this->InboundServerName;
					if (params.size() >= 2)
					{
						Instance->SNO->WriteToSnoMask(*(params[0].c_str()), "From " + sourceserv + ": "+ params[1]);
					}

					return Utils->DoOneToAllButSenderRaw(line, sourceserv, prefix, command, params);
				}
				else if (command == "ENDBURST")
				{
					this->bursting = false;
					Instance->XLines->apply_lines(APPLY_ZLINES|APPLY_GLINES|APPLY_QLINES);
					std::string sourceserv = this->myhost;
					if (this->InboundServerName != "")
					{
						sourceserv = this->InboundServerName;
					}
					this->Instance->SNO->WriteToSnoMask('l',"Received end of netburst from \2%s\2",sourceserv.c_str());

					Event rmode((char*)sourceserv.c_str(), (Module*)Utils->Creator, "new_server");
					rmode.Send(Instance);

					return true;
				}
				else
				{
					// not a special inter-server command.
					// Emulate the actual user doing the command,
					// this saves us having a huge ugly parser.
					userrec* who = this->Instance->FindNick(prefix);
					std::string sourceserv = this->myhost;
					if (this->InboundServerName != "")
					{
						sourceserv = this->InboundServerName;
					}
					if ((!who) && (command == "MODE"))
					{
						if (Utils->IsServer(prefix))
						{
							const char* modelist[127];
							for (size_t i = 0; i < params.size(); i++)
								modelist[i] = params[i].c_str();

							userrec* fake = new userrec(Instance);
							fake->SetFd(FD_MAGIC_NUMBER);

							this->Instance->SendMode(modelist, params.size(), fake);
	
							delete fake;

							/* Hot potato! pass it on! */
							return Utils->DoOneToAllButSenderRaw(line,sourceserv,prefix,command,params);
						}
					}
					if (who)
					{
						if ((command == "NICK") && (params.size() > 0))
						{
							/* On nick messages, check that the nick doesnt
							 * already exist here. If it does, kill their copy,
							 * and our copy.
							 */
							userrec* x = this->Instance->FindNick(params[0]);
							if ((x) && (x != who))
							{
								std::deque<std::string> p;
								p.push_back(params[0]);
								p.push_back("Nickname collision ("+prefix+" -> "+params[0]+")");
								Utils->DoOneToMany(this->Instance->Config->ServerName,"KILL",p);
								p.clear();
								p.push_back(prefix);
								p.push_back("Nickname collision");
								Utils->DoOneToMany(this->Instance->Config->ServerName,"KILL",p);
								userrec::QuitUser(this->Instance,x,"Nickname collision ("+prefix+" -> "+params[0]+")");
								userrec* y = this->Instance->FindNick(prefix);
								if (y)
								{
									userrec::QuitUser(this->Instance,y,"Nickname collision");
								}
								return Utils->DoOneToAllButSenderRaw(line,sourceserv,prefix,command,params);
							}
						}
						// its a user
						target = who->server;
						const char* strparams[127];
						for (unsigned int q = 0; q < params.size(); q++)
						{
							strparams[q] = params[q].c_str();
						}
						switch (this->Instance->CallCommandHandler(command.c_str(), strparams, params.size(), who))
						{
							case CMD_INVALID:
								this->WriteLine("ERROR :Unrecognised command '"+std::string(command.c_str())+"' -- possibly loaded mismatched modules");
								return false;
							break;
							case CMD_FAILURE:
								return true;
							break;
							default:
								/* CMD_SUCCESS and CMD_USER_DELETED fall through here */
							break;
						}
					}
					else
					{
						// its not a user. Its either a server, or somethings screwed up.
						if (Utils->IsServer(prefix))
						{
							target = this->Instance->Config->ServerName;
						}
						else
						{
							Instance->Log(DEBUG,"Command with unknown origin '%s'",prefix.c_str());
							return true;
						}
					}
					return Utils->DoOneToAllButSenderRaw(line,sourceserv,prefix,command,params);

				}
				return true;
			break;
		}
		return true;
	}

	virtual std::string GetName()
	{
		std::string sourceserv = this->myhost;
		if (this->InboundServerName != "")
		{
			sourceserv = this->InboundServerName;
		}
		return sourceserv;
	}

	virtual void OnTimeout()
	{
		if (this->LinkState == CONNECTING)
		{
			this->Instance->SNO->WriteToSnoMask('l',"CONNECT: Connection to \002"+myhost+"\002 timed out.");
			Link* MyLink = Utils->FindLink(myhost);
			if (MyLink)
				Utils->DoFailOver(MyLink);
		}
	}

	virtual void OnClose()
	{
		// Connection closed.
		// If the connection is fully up (state CONNECTED)
		// then propogate a netsplit to all peers.
		std::string quitserver = this->myhost;
		if (this->InboundServerName != "")
		{
			quitserver = this->InboundServerName;
		}
		TreeServer* s = Utils->FindServer(quitserver);
		if (s)
		{
			Squit(s,"Remote host closed the connection");
		}

		if (quitserver != "")
			this->Instance->SNO->WriteToSnoMask('l',"Connection to '\2%s\2' failed.",quitserver.c_str());
	}

	virtual int OnIncomingConnection(int newsock, char* ip)
	{
		/* To prevent anyone from attempting to flood opers/DDoS by connecting to the server port,
		 * or discovering if this port is the server port, we don't allow connections from any
		 * IPs for which we don't have a link block.
		 */
		bool found = false;

		found = (std::find(Utils->ValidIPs.begin(), Utils->ValidIPs.end(), ip) != Utils->ValidIPs.end());
		if (!found)
		{
			for (vector<std::string>::iterator i = Utils->ValidIPs.begin(); i != Utils->ValidIPs.end(); i++)
				if (irc::sockets::MatchCIDR(ip, (*i).c_str()))
					found = true;

			if (!found)
			{
				this->Instance->SNO->WriteToSnoMask('l',"Server connection from %s denied (no link blocks with that IP address)", ip);
				close(newsock);
				return false;
			}
		}

		TreeSocket* s = new TreeSocket(this->Utils, this->Instance, newsock, ip, this->Hook);

		s = s; /* Whinge whinge whinge, thats all GCC ever does. */
		return true;
	}
};

/** This class is used to resolve server hostnames during /connect and autoconnect.
 * As of 1.1, the resolver system is seperated out from InspSocket, so we must do this
 * resolver step first ourselves if we need it. This is totally nonblocking, and will
 * callback to OnLookupComplete or OnError when completed. Once it has completed we
 * will have an IP address which we can then use to continue our connection.
 */
class ServernameResolver : public Resolver
{       
 private:
	/** A copy of the Link tag info for what we're connecting to.
	 * We take a copy, rather than using a pointer, just in case the
	 * admin takes the tag away and rehashes while the domain is resolving.
	 */
	Link MyLink;
	SpanningTreeUtilities* Utils;
 public: 
	ServernameResolver(Module* me, SpanningTreeUtilities* Util, InspIRCd* Instance, const std::string &hostname, Link x) : Resolver(Instance, hostname, DNS_QUERY_FORWARD, me), MyLink(x), Utils(Util)
	{
		/* Nothing in here, folks */
	}

	void OnLookupComplete(const std::string &result)
	{
		/* Initiate the connection, now that we have an IP to use.
		 * Passing a hostname directly to InspSocket causes it to
		 * just bail and set its FD to -1.
		 */
		TreeServer* CheckDupe = Utils->FindServer(MyLink.Name.c_str());
		if (!CheckDupe) /* Check that nobody tried to connect it successfully while we were resolving */
		{

			if ((!MyLink.Hook.empty()) && (Utils->hooks.find(MyLink.Hook.c_str()) ==  Utils->hooks.end()))
				return;

			TreeSocket* newsocket = new TreeSocket(this->Utils, ServerInstance, result,MyLink.Port,false,MyLink.Timeout ? MyLink.Timeout : 10,MyLink.Name.c_str(),
					MyLink.Hook.empty() ? NULL : Utils->hooks[MyLink.Hook.c_str()]);
			if (newsocket->GetFd() > -1)
			{
				/* We're all OK */
			}
			else
			{
				/* Something barfed, show the opers */
				ServerInstance->SNO->WriteToSnoMask('l',"CONNECT: Error connecting \002%s\002: %s.",MyLink.Name.c_str(),strerror(errno));
				delete newsocket;
				Utils->DoFailOver(&MyLink);
			}
		}
	}

	void OnError(ResolverError e, const std::string &errormessage)
	{
		/* Ooops! */
		ServerInstance->SNO->WriteToSnoMask('l',"CONNECT: Error connecting \002%s\002: Unable to resolve hostname - %s",MyLink.Name.c_str(),errormessage.c_str());
		Utils->DoFailOver(&MyLink);
	}
};

/** Handle resolving of server IPs for the cache
 */
class SecurityIPResolver : public Resolver
{
 private:
	Link MyLink;
	SpanningTreeUtilities* Utils;
 public:
	SecurityIPResolver(Module* me, SpanningTreeUtilities* U, InspIRCd* Instance, const std::string &hostname, Link x) : Resolver(Instance, hostname, DNS_QUERY_FORWARD, me), MyLink(x), Utils(U)
	{
	}

	void OnLookupComplete(const std::string &result)
	{
		ServerInstance->Log(DEBUG,"Security IP cache: Adding IP address '%s' for Link '%s'",result.c_str(),MyLink.Name.c_str());
		Utils->ValidIPs.push_back(result);
	}

	void OnError(ResolverError e, const std::string &errormessage)
	{
		ServerInstance->Log(DEBUG,"Could not resolve IP associated with Link '%s': %s",MyLink.Name.c_str(),errormessage.c_str());
	}
};

SpanningTreeUtilities::SpanningTreeUtilities(InspIRCd* Instance, ModuleSpanningTree* C) : ServerInstance(Instance), Creator(C)
{
	Bindings.clear();

	this->TreeRoot = new TreeServer(this, ServerInstance, ServerInstance->Config->ServerName, ServerInstance->Config->ServerDesc);

	modulelist* ml = ServerInstance->FindInterface("InspSocketHook");

	/* Did we find any modules? */
	if (ml)
	{
		/* Yes, enumerate them all to find out the hook name */
		for (modulelist::iterator m = ml->begin(); m != ml->end(); m++)
		{
			/* Make a request to it for its name, its implementing
			 * InspSocketHook so we know its safe to do this
			 */
			std::string name = InspSocketNameRequest((Module*)Creator, *m).Send();
			/* Build a map of them */
			hooks[name.c_str()] = *m;
			hooknames.push_back(name);
			ServerInstance->Log(DEBUG, "Found InspSocketHook interface: '%s' -> '%08x'", name.c_str(), *m);
		}
	}

	this->ReadConfiguration(true);
}

SpanningTreeUtilities::~SpanningTreeUtilities()
{
	for (unsigned int i = 0; i < Bindings.size(); i++)
	{
		ServerInstance->Log(DEBUG,"Freeing binding %d of %d",i, Bindings.size());
		ServerInstance->SE->DelFd(Bindings[i]);
		Bindings[i]->Close();
		DELETE(Bindings[i]);
	}
	ServerInstance->Log(DEBUG,"Freeing connected servers...");
	while (TreeRoot->ChildCount())
	{
		TreeServer* child_server = TreeRoot->GetChild(0);
		ServerInstance->Log(DEBUG,"Freeing connected server %s", child_server->GetName().c_str());
		if (child_server)
		{
			TreeSocket* sock = child_server->GetSocket();
			ServerInstance->SE->DelFd(sock);
			sock->Close();
			DELETE(sock);
		}
	}
	delete TreeRoot;
}

void SpanningTreeUtilities::AddThisServer(TreeServer* server, TreeServerList &list)
{
	if (list.find(server) == list.end())
		list[server] = server;
}

/** returns a list of DIRECT servernames for a specific channel */
void SpanningTreeUtilities::GetListOfServersForChannel(chanrec* c, TreeServerList &list, char status, const CUList &exempt_list)
{
	CUList *ulist;
	switch (status)
	{
		case '@':
			ulist = c->GetOppedUsers();
		break;
		case '%':
			ulist = c->GetHalfoppedUsers();
		break;
		case '+':
			ulist = c->GetVoicedUsers();
		break;
		default:
			ulist = c->GetUsers();
		break;
	}
	for (CUList::iterator i = ulist->begin(); i != ulist->end(); i++)
	{
		if ((i->second->GetFd() < 0) && (exempt_list.find(i->second) == exempt_list.end()))
		{
			TreeServer* best = this->BestRouteTo(i->second->server);
			if (best)
				AddThisServer(best,list);
		}
	}
	return;
}

bool SpanningTreeUtilities::DoOneToAllButSenderRaw(const std::string &data, const std::string &omit, const std::string &prefix, const irc::string &command, std::deque<std::string> &params)
{
	char pfx = 0;
	TreeServer* omitroute = this->BestRouteTo(omit);
	if ((command == "NOTICE") || (command == "PRIVMSG"))
	{
		if (params.size() >= 2)
		{
			/* Prefixes */
			if ((*(params[0].c_str()) == '@') || (*(params[0].c_str()) == '%') || (*(params[0].c_str()) == '+'))
			{
				pfx = params[0][0];
				params[0] = params[0].substr(1, params[0].length()-1);
			}
			if ((*(params[0].c_str()) != '#') && (*(params[0].c_str()) != '$'))
			{
				// special routing for private messages/notices
				userrec* d = ServerInstance->FindNick(params[0]);
				if (d)
				{
					std::deque<std::string> par;
					par.push_back(params[0]);
					par.push_back(":"+params[1]);
					this->DoOneToOne(prefix,command.c_str(),par,d->server);
					return true;
				}
			}
			else if (*(params[0].c_str()) == '$')
			{
				std::deque<std::string> par;
				par.push_back(params[0]);
				par.push_back(":"+params[1]);
				this->DoOneToAllButSender(prefix,command.c_str(),par,omitroute->GetName());
				return true;
			}
			else
			{
				chanrec* c = ServerInstance->FindChan(params[0]);
				userrec* u = ServerInstance->FindNick(prefix);
				if (c && u)
				{
					CUList elist;
					TreeServerList list;
					FOREACH_MOD(I_OnBuildExemptList, OnBuildExemptList((command == "PRIVMSG" ? MSG_PRIVMSG : MSG_NOTICE), c, u, pfx, elist));
					GetListOfServersForChannel(c,list,pfx,elist);

					for (TreeServerList::iterator i = list.begin(); i != list.end(); i++)
					{
						TreeSocket* Sock = i->second->GetSocket();
						if ((Sock) && (i->second->GetName() != omit) && (omitroute != i->second))
						{
							Sock->WriteLine(data);
						}
					}
					return true;
				}
			}
		}
	}
	unsigned int items =this->TreeRoot->ChildCount();
	for (unsigned int x = 0; x < items; x++)
	{
		TreeServer* Route = this->TreeRoot->GetChild(x);
		if ((Route) && (Route->GetSocket()) && (Route->GetName() != omit) && (omitroute != Route))
		{
			TreeSocket* Sock = Route->GetSocket();
			if (Sock)
				Sock->WriteLine(data);
		}
	}
	return true;
}

bool SpanningTreeUtilities::DoOneToAllButSender(const std::string &prefix, const std::string &command, std::deque<std::string> &params, std::string omit)
{
	TreeServer* omitroute = this->BestRouteTo(omit);
	std::string FullLine = ":" + prefix + " " + command;
	unsigned int words = params.size();
	for (unsigned int x = 0; x < words; x++)
	{
		FullLine = FullLine + " " + params[x];
	}
	unsigned int items = this->TreeRoot->ChildCount();
	for (unsigned int x = 0; x < items; x++)
	{
		TreeServer* Route = this->TreeRoot->GetChild(x);
		// Send the line IF:
		// The route has a socket (its a direct connection)
		// The route isnt the one to be omitted
		// The route isnt the path to the one to be omitted
		if ((Route) && (Route->GetSocket()) && (Route->GetName() != omit) && (omitroute != Route))
		{
			TreeSocket* Sock = Route->GetSocket();
			if (Sock)
				Sock->WriteLine(FullLine);
		}
	}
	return true;
}

bool SpanningTreeUtilities::DoOneToMany(const std::string &prefix, const std::string &command, std::deque<std::string> &params)
{
	std::string FullLine = ":" + prefix + " " + command;
	unsigned int words = params.size();
	for (unsigned int x = 0; x < words; x++)
	{
		FullLine = FullLine + " " + params[x];
	}
	unsigned int items = this->TreeRoot->ChildCount();
	for (unsigned int x = 0; x < items; x++)
	{
		TreeServer* Route = this->TreeRoot->GetChild(x);
		if (Route && Route->GetSocket())
		{
			TreeSocket* Sock = Route->GetSocket();
			if (Sock)
				Sock->WriteLine(FullLine);
		}
	}
	return true;
}

bool SpanningTreeUtilities::DoOneToMany(const char* prefix, const char* command, std::deque<std::string> &params)
{
	std::string spfx = prefix;
	std::string scmd = command;
	return this->DoOneToMany(spfx, scmd, params);
}

bool SpanningTreeUtilities::DoOneToAllButSender(const char* prefix, const char* command, std::deque<std::string> &params, std::string omit)
{
	std::string spfx = prefix;
	std::string scmd = command;
	return this->DoOneToAllButSender(spfx, scmd, params, omit);
}
	
bool SpanningTreeUtilities::DoOneToOne(const std::string &prefix, const std::string &command, std::deque<std::string> &params, std::string target)
{
	TreeServer* Route = this->BestRouteTo(target);
	if (Route)
	{
		std::string FullLine = ":" + prefix + " " + command;
		unsigned int words = params.size();
		for (unsigned int x = 0; x < words; x++)
		{
			FullLine = FullLine + " " + params[x];
		}
		if (Route && Route->GetSocket())
		{
			TreeSocket* Sock = Route->GetSocket();
			if (Sock)
				Sock->WriteLine(FullLine);
		}
		return true;
	}
	else
	{
		return false;
	}
}

void SpanningTreeUtilities::ReadConfiguration(bool rebind)
{
	ConfigReader* Conf = new ConfigReader(ServerInstance);
	if (rebind)
	{
		for (int j =0; j < Conf->Enumerate("bind"); j++)
		{
			std::string Type = Conf->ReadValue("bind","type",j);
			std::string IP = Conf->ReadValue("bind","address",j);
			std::string Port = Conf->ReadValue("bind","port",j);
			std::string transport = Conf->ReadValue("bind","transport",j);
			if (Type == "servers")
			{
				irc::portparser portrange(Port, false);
				int portno = -1;
				while ((portno = portrange.GetToken()))
				{
					ServerInstance->Log(DEBUG,"m_spanningtree: Binding server port %s:%d", IP.c_str(), portno);
					if (IP == "*")
						IP = "";

					if ((!transport.empty()) && (hooks.find(transport.c_str()) ==  hooks.end()))
					{
						ServerInstance->Log(DEFAULT,"m_spanningtree: WARNING: Can't find transport type '%s' for port %s:%s - maybe you forgot to load it BEFORE m_spanningtree in your config file? - Skipping this port binding",
								transport.c_str(), IP.c_str(), Port.c_str());
						break;
					}

					TreeSocket* listener = new TreeSocket(this, ServerInstance, IP.c_str(), portno, true, 10, transport.empty() ? NULL : hooks[transport.c_str()]);
					if (listener->GetState() == I_LISTENING)
					{
						ServerInstance->Log(DEFAULT,"m_spanningtree: Binding server port %s:%d successful!", IP.c_str(), portno);
						Bindings.push_back(listener);
					}
					else
					{
						ServerInstance->Log(DEFAULT,"m_spanningtree: Warning: Failed to bind server port %s:%d",IP.c_str(), portno);
						listener->Close();
						DELETE(listener);
					}
					ServerInstance->Log(DEBUG,"Done with this binding");
				}
			}
		}
	}
	FlatLinks = Conf->ReadFlag("options","flatlinks",0);
	HideULines = Conf->ReadFlag("options","hideulines",0);
	AnnounceTSChange = Conf->ReadFlag("options","announcets",0);
	EnableTimeSync = !(Conf->ReadFlag("options","notimesync",0));
	LinkBlocks.clear();
	ValidIPs.clear();
	for (int j =0; j < Conf->Enumerate("link"); j++)
	{
		Link L;
		std::string Allow = Conf->ReadValue("link","allowmask",j);
		L.Name = (Conf->ReadValue("link","name",j)).c_str();
		L.IPAddr = Conf->ReadValue("link","ipaddr",j);
		L.FailOver = Conf->ReadValue("link","failover",j).c_str();
		L.Port = Conf->ReadInteger("link","port",j,true);
		L.SendPass = Conf->ReadValue("link","sendpass",j);
		L.RecvPass = Conf->ReadValue("link","recvpass",j);
		L.AutoConnect = Conf->ReadInteger("link","autoconnect",j,true);
		L.HiddenFromStats = Conf->ReadFlag("link","hidden",j);
		L.Timeout = Conf->ReadInteger("link","timeout",j,true);
		L.Hook = Conf->ReadValue("link", "transport", j);

		if ((!L.Hook.empty()) && (hooks.find(L.Hook.c_str()) ==  hooks.end()))
		{
			ServerInstance->Log(DEFAULT,"m_spanningtree: WARNING: Can't find transport type '%s' for link '%s' - maybe you forgot to load it BEFORE m_spanningtree in your config file? Skipping <link> tag completely.",
					L.Hook.c_str(), L.Name.c_str());
			continue;

		}

		L.NextConnectTime = time(NULL) + L.AutoConnect;
		/* Bugfix by brain, do not allow people to enter bad configurations */
		if (L.Name != ServerInstance->Config->ServerName)
		{
			if ((L.IPAddr != "") && (L.RecvPass != "") && (L.SendPass != "") && (L.Name != "") && (L.Port))
			{
				ValidIPs.push_back(L.IPAddr);

				if (Allow.length())
					ValidIPs.push_back(Allow);

				/* Needs resolving */
				insp_inaddr binip;
				if (insp_aton(L.IPAddr.c_str(), &binip) < 1)
				{
					try
					{
						SecurityIPResolver* sr = new SecurityIPResolver((Module*)this->Creator, this, ServerInstance, L.IPAddr, L);
						ServerInstance->AddResolver(sr);
					}
					catch (ModuleException& e)
					{
						ServerInstance->Log(DEBUG,"Error in resolver: %s",e.GetReason());
					}
				}

				LinkBlocks.push_back(L);
				ServerInstance->Log(DEBUG,"m_spanningtree: Read server %s with host %s:%d",L.Name.c_str(),L.IPAddr.c_str(),L.Port);
			}
			else
			{
				if (L.IPAddr == "")
				{
					ServerInstance->Log(DEFAULT,"Invalid configuration for server '%s', IP address not defined!",L.Name.c_str());
				}
				else if (L.RecvPass == "")
				{
					ServerInstance->Log(DEFAULT,"Invalid configuration for server '%s', recvpass not defined!",L.Name.c_str());
				}
				else if (L.SendPass == "")
				{
					ServerInstance->Log(DEFAULT,"Invalid configuration for server '%s', sendpass not defined!",L.Name.c_str());
				}
				else if (L.Name == "")
				{
					ServerInstance->Log(DEFAULT,"Invalid configuration, link tag without a name!");
				}
				else if (!L.Port)
				{
					ServerInstance->Log(DEFAULT,"Invalid configuration for server '%s', no port specified!",L.Name.c_str());
				}
			}
		}
		else
		{
			ServerInstance->Log(DEFAULT,"Invalid configuration for server '%s', link tag has the same server name as the local server!",L.Name.c_str());
		}
	}
	DELETE(Conf);
}

/** To create a timer which recurs every second, we inherit from InspTimer.
 * InspTimer is only one-shot however, so at the end of each Tick() we simply
 * insert another of ourselves into the pending queue :)
 */
class TimeSyncTimer : public InspTimer
{
 private:
	InspIRCd *Instance;
	ModuleSpanningTree *Module;
 public:
	TimeSyncTimer(InspIRCd *Instance, ModuleSpanningTree *Mod);
	virtual void Tick(time_t TIME);
};

HandshakeTimer::HandshakeTimer(InspIRCd* Inst, TreeSocket* s, Link* l, SpanningTreeUtilities* u) : InspTimer(1, time(NULL)), Instance(Inst), sock(s), lnk(l), Utils(u)
{
	thefd = sock->GetFd();
}

void HandshakeTimer::Tick(time_t TIME)
{
	if (Instance->SE->GetRef(thefd) == sock)
	{
		if (sock->GetHook() && InspSocketHSCompleteRequest(sock, (Module*)Utils->Creator, sock->GetHook()).Send())
		{
			Instance->Log(DEBUG,"Handshake timer activated, sending SERVER and/or CAPAB");
			InspSocketAttachCertRequest(sock, (Module*)Utils->Creator, sock->GetHook()).Send();
			sock->SendCapabilities();
			if (sock->GetLinkState() == CONNECTING)
			{
				sock->WriteLine(std::string("SERVER ")+this->Instance->Config->ServerName+" "+lnk->SendPass+" 0 :"+this->Instance->Config->ServerDesc);
			}
		}
		else
		{
			Instance->Timers->AddTimer(new HandshakeTimer(Instance, sock, lnk, Utils));
		}
	}
}

class ModuleSpanningTree : public Module
{
	int line;
	int NumServers;
	unsigned int max_local;
	unsigned int max_global;
	cmd_rconnect* command_rconnect;
	SpanningTreeUtilities* Utils;

 public:
	TimeSyncTimer *SyncTimer;

	ModuleSpanningTree(InspIRCd* Me)
		: Module::Module(Me), max_local(0), max_global(0)
	{
		ServerInstance->UseInterface("InspSocketHook");

		Utils = new SpanningTreeUtilities(Me, this);

		command_rconnect = new cmd_rconnect(ServerInstance, this, Utils);
		ServerInstance->AddCommand(command_rconnect);

		if (Utils->EnableTimeSync)
		{
			SyncTimer = new TimeSyncTimer(ServerInstance, this);
			ServerInstance->Timers->AddTimer(SyncTimer);
		}
		else
			SyncTimer = NULL;
	}

	void ShowLinks(TreeServer* Current, userrec* user, int hops)
	{
		std::string Parent = Utils->TreeRoot->GetName();
		if (Current->GetParent())
		{
			Parent = Current->GetParent()->GetName();
		}
		for (unsigned int q = 0; q < Current->ChildCount(); q++)
		{
			if ((Utils->HideULines) && (ServerInstance->ULine(Current->GetChild(q)->GetName().c_str())))
			{
				if (*user->oper)
				{
					 ShowLinks(Current->GetChild(q),user,hops+1);
				}
			}
			else
			{
				ShowLinks(Current->GetChild(q),user,hops+1);
			}
		}
		/* Don't display the line if its a uline, hide ulines is on, and the user isnt an oper */
		if ((Utils->HideULines) && (ServerInstance->ULine(Current->GetName().c_str())) && (!*user->oper))
			return;
		user->WriteServ("364 %s %s %s :%d %s",user->nick,Current->GetName().c_str(),(Utils->FlatLinks && (!*user->oper)) ? ServerInstance->Config->ServerName : Parent.c_str(),(Utils->FlatLinks && (!*user->oper)) ? 0 : hops,Current->GetDesc().c_str());
	}

	int CountLocalServs()
	{
		return Utils->TreeRoot->ChildCount();
	}

	int CountServs()
	{
		return Utils->serverlist.size();
	}

	void HandleLinks(const char** parameters, int pcnt, userrec* user)
	{
		ShowLinks(Utils->TreeRoot,user,0);
		user->WriteServ("365 %s * :End of /LINKS list.",user->nick);
		return;
	}

	void HandleLusers(const char** parameters, int pcnt, userrec* user)
	{
		unsigned int n_users = ServerInstance->UserCount();

		/* Only update these when someone wants to see them, more efficient */
		if ((unsigned int)ServerInstance->LocalUserCount() > max_local)
			max_local = ServerInstance->LocalUserCount();
		if (n_users > max_global)
			max_global = n_users;

		unsigned int ulined_count = 0;
		unsigned int ulined_local_count = 0;

		/* If ulined are hidden and we're not an oper, count the number of ulined servers hidden,
		 * locally and globally (locally means directly connected to us)
		 */
		if ((Utils->HideULines) && (!*user->oper))
		{
			for (server_hash::iterator q = Utils->serverlist.begin(); q != Utils->serverlist.end(); q++)
			{
				if (ServerInstance->ULine(q->second->GetName().c_str()))
				{
					ulined_count++;
					if (q->second->GetParent() == Utils->TreeRoot)
						ulined_local_count++;
				}
			}
		}

		user->WriteServ("251 %s :There are %d users and %d invisible on %d servers",user->nick,n_users-ServerInstance->InvisibleUserCount(),ServerInstance->InvisibleUserCount(),ulined_count ? this->CountServs() - ulined_count : this->CountServs());
		if (ServerInstance->OperCount())
			user->WriteServ("252 %s %d :operator(s) online",user->nick,ServerInstance->OperCount());
		if (ServerInstance->UnregisteredUserCount())
			user->WriteServ("253 %s %d :unknown connections",user->nick,ServerInstance->UnregisteredUserCount());
		if (ServerInstance->ChannelCount())
			user->WriteServ("254 %s %d :channels formed",user->nick,ServerInstance->ChannelCount());
		user->WriteServ("254 %s :I have %d clients and %d servers",user->nick,ServerInstance->LocalUserCount(),ulined_local_count ? this->CountLocalServs() - ulined_local_count : this->CountLocalServs());
		user->WriteServ("265 %s :Current Local Users: %d  Max: %d",user->nick,ServerInstance->LocalUserCount(),max_local);
		user->WriteServ("266 %s :Current Global Users: %d  Max: %d",user->nick,n_users,max_global);
		return;
	}

	// WARNING: NOT THREAD SAFE - DONT GET ANY SMART IDEAS.

	void ShowMap(TreeServer* Current, userrec* user, int depth, char matrix[128][80], float &totusers, float &totservers)
	{
		if (line < 128)
		{
			for (int t = 0; t < depth; t++)
			{
				matrix[line][t] = ' ';
			}

			// For Aligning, we need to work out exactly how deep this thing is, and produce
			// a 'Spacer' String to compensate.
			char spacer[40];

			memset(spacer,' ',40);
			if ((40 - Current->GetName().length() - depth) > 1) {
				spacer[40 - Current->GetName().length() - depth] = '\0';
			}
			else
			{
				spacer[5] = '\0';
			}

			float percent;
			char text[80];
			if (ServerInstance->clientlist->size() == 0) {
				// If there are no users, WHO THE HELL DID THE /MAP?!?!?!
				percent = 0;
			}
			else
			{
				percent = ((float)Current->GetUserCount() / (float)ServerInstance->clientlist->size()) * 100;
			}
			snprintf(text, 80, "%s %s%5d [%5.2f%%]", Current->GetName().c_str(), spacer, Current->GetUserCount(), percent);
			totusers += Current->GetUserCount();
			totservers++;
			strlcpy(&matrix[line][depth],text,80);
			line++;
			for (unsigned int q = 0; q < Current->ChildCount(); q++)
			{
				if ((Utils->HideULines) && (ServerInstance->ULine(Current->GetChild(q)->GetName().c_str())))
				{
					if (*user->oper)
					{
						ShowMap(Current->GetChild(q),user,(Utils->FlatLinks && (!*user->oper)) ? depth : depth+2,matrix,totusers,totservers);
					}
				}
				else
				{
					ShowMap(Current->GetChild(q),user,(Utils->FlatLinks && (!*user->oper)) ? depth : depth+2,matrix,totusers,totservers);
				}
			}
		}
	}

	int HandleMotd(const char** parameters, int pcnt, userrec* user)
	{
		if (pcnt > 0)
		{
			/* Remote MOTD, the server is within the 1st parameter */
			std::deque<std::string> params;
			params.push_back(parameters[0]);

			/* Send it out remotely, generate no reply yet */
			TreeServer* s = Utils->FindServerMask(parameters[0]);
			if (s)
			{
				Utils->DoOneToOne(user->nick, "MOTD", params, s->GetName());
			}
			else
			{
				user->WriteServ( "402 %s %s :No such server", user->nick, parameters[0]);
			}
			return 1;
		}
		return 0;
	}

	int HandleAdmin(const char** parameters, int pcnt, userrec* user)
	{
		if (pcnt > 0)
		{
			/* Remote ADMIN, the server is within the 1st parameter */
			std::deque<std::string> params;
			params.push_back(parameters[0]);

			/* Send it out remotely, generate no reply yet */
			TreeServer* s = Utils->FindServerMask(parameters[0]);
			if (s)
			{
				Utils->DoOneToOne(user->nick, "ADMIN", params, s->GetName());
			}
			else
			{
				user->WriteServ( "402 %s %s :No such server", user->nick, parameters[0]);
			}
			return 1;
		}
		return 0;
	}

	int HandleStats(const char** parameters, int pcnt, userrec* user)
	{
		if (pcnt > 1)
		{
			/* Remote STATS, the server is within the 2nd parameter */
			std::deque<std::string> params;
			params.push_back(parameters[0]);
			params.push_back(parameters[1]);
			/* Send it out remotely, generate no reply yet */
			TreeServer* s = Utils->FindServerMask(parameters[1]);
			if (s)
			{
				params[1] = s->GetName();
				Utils->DoOneToOne(user->nick, "STATS", params, s->GetName());
			}
			else
			{
				user->WriteServ( "402 %s %s :No such server", user->nick, parameters[0]);
			}
			return 1;
		}
		return 0;
	}

	// Ok, prepare to be confused.
	// After much mulling over how to approach this, it struck me that
	// the 'usual' way of doing a /MAP isnt the best way. Instead of
	// keeping track of a ton of ascii characters, and line by line
	// under recursion working out where to place them using multiplications
	// and divisons, we instead render the map onto a backplane of characters
	// (a character matrix), then draw the branches as a series of "L" shapes
	// from the nodes. This is not only friendlier on CPU it uses less stack.

	void HandleMap(const char** parameters, int pcnt, userrec* user)
	{
		// This array represents a virtual screen which we will
		// "scratch" draw to, as the console device of an irc
		// client does not provide for a proper terminal.
		float totusers = 0;
		float totservers = 0;
		char matrix[128][80];
		for (unsigned int t = 0; t < 128; t++)
		{
			matrix[t][0] = '\0';
		}
		line = 0;
		// The only recursive bit is called here.
		ShowMap(Utils->TreeRoot,user,0,matrix,totusers,totservers);
		// Process each line one by one. The algorithm has a limit of
		// 128 servers (which is far more than a spanning tree should have
		// anyway, so we're ok). This limit can be raised simply by making
		// the character matrix deeper, 128 rows taking 10k of memory.
		for (int l = 1; l < line; l++)
		{
			// scan across the line looking for the start of the
			// servername (the recursive part of the algorithm has placed
			// the servers at indented positions depending on what they
			// are related to)
			int first_nonspace = 0;
			while (matrix[l][first_nonspace] == ' ')
			{
				first_nonspace++;
			}
			first_nonspace--;
			// Draw the `- (corner) section: this may be overwritten by
			// another L shape passing along the same vertical pane, becoming
			// a |- (branch) section instead.
			matrix[l][first_nonspace] = '-';
			matrix[l][first_nonspace-1] = '`';
			int l2 = l - 1;
			// Draw upwards until we hit the parent server, causing possibly
			// other corners (`-) to become branches (|-)
			while ((matrix[l2][first_nonspace-1] == ' ') || (matrix[l2][first_nonspace-1] == '`'))
			{
				matrix[l2][first_nonspace-1] = '|';
				l2--;
			}
		}
		// dump the whole lot to the user. This is the easy bit, honest.
		for (int t = 0; t < line; t++)
		{
			user->WriteServ("006 %s :%s",user->nick,&matrix[t][0]);
		}
		float avg_users = totusers / totservers;
		user->WriteServ("270 %s :%.0f server%s and %.0f user%s, average %.2f users per server",user->nick,totservers,(totservers > 1 ? "s" : ""),totusers,(totusers > 1 ? "s" : ""),avg_users);
	user->WriteServ("007 %s :End of /MAP",user->nick);
		return;
	}

	int HandleSquit(const char** parameters, int pcnt, userrec* user)
	{
		TreeServer* s = Utils->FindServerMask(parameters[0]);
		if (s)
		{
			if (s == Utils->TreeRoot)
			{
				user->WriteServ("NOTICE %s :*** SQUIT: Foolish mortal, you cannot make a server SQUIT itself! (%s matches local server name)",user->nick,parameters[0]);
				return 1;
			}
			TreeSocket* sock = s->GetSocket();
			if (sock)
			{
				ServerInstance->Log(DEBUG,"Splitting server %s",s->GetName().c_str());
				ServerInstance->SNO->WriteToSnoMask('l',"SQUIT: Server \002%s\002 removed from network by %s",parameters[0],user->nick);
				sock->Squit(s,std::string("Server quit by ") + user->GetFullRealHost());
				ServerInstance->SE->DelFd(sock);
				sock->Close();
				delete sock;
			}
			else
			{
				/* route it */
				std::deque<std::string> params;
				params.push_back(parameters[0]);
				params.push_back(std::string(":Server quit by ") + user->GetFullRealHost());
				Utils->DoOneToOne(user->nick, "RSQUIT", params, parameters[0]);
			}
		}
		else
		{
			 user->WriteServ("NOTICE %s :*** SQUIT: The server \002%s\002 does not exist on the network.",user->nick,parameters[0]);
		}
		return 1;
	}

	int HandleTime(const char** parameters, int pcnt, userrec* user)
	{
		if ((IS_LOCAL(user)) && (pcnt))
		{
			TreeServer* found = Utils->FindServerMask(parameters[0]);
			if (found)
			{
				// we dont' override for local server
				if (found == Utils->TreeRoot)
					return 0;
				
				std::deque<std::string> params;
				params.push_back(found->GetName());
				params.push_back(user->nick);
				Utils->DoOneToOne(ServerInstance->Config->ServerName,"TIME",params,found->GetName());
			}
			else
			{
				user->WriteServ("402 %s %s :No such server",user->nick,parameters[0]);
			}
		}
		return 1;
	}

	int HandleRemoteWhois(const char** parameters, int pcnt, userrec* user)
	{
		if ((IS_LOCAL(user)) && (pcnt > 1))
		{
			userrec* remote = ServerInstance->FindNick(parameters[1]);
			if ((remote) && (remote->GetFd() < 0))
			{
				std::deque<std::string> params;
				params.push_back(parameters[1]);
				Utils->DoOneToOne(user->nick,"IDLE",params,remote->server);
				return 1;
			}
			else if (!remote)
			{
				user->WriteServ("401 %s %s :No such nick/channel",user->nick, parameters[1]);
				user->WriteServ("318 %s %s :End of /WHOIS list.",user->nick, parameters[1]);
				return 1;
			}
		}
		return 0;
	}

	void DoPingChecks(time_t curtime)
	{
		for (unsigned int j = 0; j < Utils->TreeRoot->ChildCount(); j++)
		{
			TreeServer* serv = Utils->TreeRoot->GetChild(j);
			TreeSocket* sock = serv->GetSocket();
			if (sock)
			{
				if (curtime >= serv->NextPingTime())
				{
					if (serv->AnsweredLastPing())
					{
						sock->WriteLine(std::string(":")+ServerInstance->Config->ServerName+" PING "+serv->GetName());
						serv->SetNextPingTime(curtime + 60);
					}
					else
					{
						// they didnt answer, boot them
						ServerInstance->SNO->WriteToSnoMask('l',"Server \002%s\002 pinged out",serv->GetName().c_str());
						sock->Squit(serv,"Ping timeout");
						ServerInstance->SE->DelFd(sock);
						sock->Close();
						delete sock;
						return;
					}
				}
			}
		}
	}

	void ConnectServer(Link* x)
	{
		insp_inaddr binip;

		/* Do we already have an IP? If so, no need to resolve it. */
		if (insp_aton(x->IPAddr.c_str(), &binip) > 0)
		{
			/* Gave a hook, but it wasnt one we know */
			if ((!x->Hook.empty()) && (Utils->hooks.find(x->Hook.c_str()) == Utils->hooks.end()))
				return;

			TreeSocket* newsocket = new TreeSocket(Utils, ServerInstance, x->IPAddr,x->Port,false,x->Timeout ? x->Timeout : 10,x->Name.c_str(), x->Hook.empty() ? NULL : Utils->hooks[x->Hook.c_str()]);
			if (newsocket->GetFd() > -1)
			{
				/* Handled automatically on success */
			}
			else
			{
				ServerInstance->SNO->WriteToSnoMask('l',"CONNECT: Error connecting \002%s\002: %s.",x->Name.c_str(),strerror(errno));
				delete newsocket;
				Utils->DoFailOver(x);
			}
		}
		else
		{
			try
			{
				ServernameResolver* snr = new ServernameResolver((Module*)this, Utils, ServerInstance,x->IPAddr, *x);
				ServerInstance->AddResolver(snr);
			}
			catch (ModuleException& e)
			{
				ServerInstance->Log(DEBUG,"Error in resolver: %s",e.GetReason());
				Utils->DoFailOver(x);
			}
		}
	}

	void AutoConnectServers(time_t curtime)
	{
		for (std::vector<Link>::iterator x = Utils->LinkBlocks.begin(); x < Utils->LinkBlocks.end(); x++)
		{
			if ((x->AutoConnect) && (curtime >= x->NextConnectTime))
			{
				ServerInstance->Log(DEBUG,"Auto-Connecting %s",x->Name.c_str());
				x->NextConnectTime = curtime + x->AutoConnect;
				TreeServer* CheckDupe = Utils->FindServer(x->Name.c_str());
				if (x->FailOver.length())
				{
					TreeServer* CheckFailOver = Utils->FindServer(x->FailOver.c_str());
					if (CheckFailOver)
					{
						/* The failover for this server is currently a member of the network.
						 * The failover probably succeeded, where the main link did not.
						 * Don't try the main link until the failover is gone again.
						 */
						continue;
					}
				}
				if (!CheckDupe)
				{
					// an autoconnected server is not connected. Check if its time to connect it
					ServerInstance->SNO->WriteToSnoMask('l',"AUTOCONNECT: Auto-connecting server \002%s\002 (%lu seconds until next attempt)",x->Name.c_str(),x->AutoConnect);
					this->ConnectServer(&(*x));
				}
			}
		}
	}

	int HandleVersion(const char** parameters, int pcnt, userrec* user)
	{
		// we've already checked if pcnt > 0, so this is safe
		TreeServer* found = Utils->FindServerMask(parameters[0]);
		if (found)
		{
			std::string Version = found->GetVersion();
			user->WriteServ("351 %s :%s",user->nick,Version.c_str());
			if (found == Utils->TreeRoot)
			{
				ServerInstance->Config->Send005(user);
			}
		}
		else
		{
			user->WriteServ("402 %s %s :No such server",user->nick,parameters[0]);
		}
		return 1;
	}
	
	int HandleConnect(const char** parameters, int pcnt, userrec* user)
	{
		for (std::vector<Link>::iterator x = Utils->LinkBlocks.begin(); x < Utils->LinkBlocks.end(); x++)
		{
			if (ServerInstance->MatchText(x->Name.c_str(),parameters[0]))
			{
				TreeServer* CheckDupe = Utils->FindServer(x->Name.c_str());
				if (!CheckDupe)
				{
					user->WriteServ("NOTICE %s :*** CONNECT: Connecting to server: \002%s\002 (%s:%d)",user->nick,x->Name.c_str(),(x->HiddenFromStats ? "<hidden>" : x->IPAddr.c_str()),x->Port);
					ConnectServer(&(*x));
					return 1;
				}
				else
				{
					user->WriteServ("NOTICE %s :*** CONNECT: Server \002%s\002 already exists on the network and is connected via \002%s\002",user->nick,x->Name.c_str(),CheckDupe->GetParent()->GetName().c_str());
					return 1;
				}
			}
		}
		user->WriteServ("NOTICE %s :*** CONNECT: No server matching \002%s\002 could be found in the config file.",user->nick,parameters[0]);
		return 1;
	}

	void BroadcastTimeSync()
	{
		std::deque<std::string> params;
		params.push_back(ConvToStr(ServerInstance->Time(true)));
		Utils->DoOneToMany(Utils->TreeRoot->GetName(), "TIMESET", params);
	}

	virtual int OnStats(char statschar, userrec* user, string_list &results)
	{
		if ((statschar == 'c') || (statschar == 'n'))
		{
			for (unsigned int i = 0; i < Utils->LinkBlocks.size(); i++)
			{
				results.push_back(std::string(ServerInstance->Config->ServerName)+" 213 "+user->nick+" "+statschar+" *@"+(Utils->LinkBlocks[i].HiddenFromStats ? "<hidden>" : Utils->LinkBlocks[i].IPAddr)+" * "+Utils->LinkBlocks[i].Name.c_str()+" "+ConvToStr(Utils->LinkBlocks[i].Port)+" "+(Utils->LinkBlocks[i].Hook.empty() ? "plaintext" : Utils->LinkBlocks[i].Hook)+" "+(Utils->LinkBlocks[i].AutoConnect ? 'a' : '-')+'s');
				if (statschar == 'c')
					results.push_back(std::string(ServerInstance->Config->ServerName)+" 244 "+user->nick+" H * * "+Utils->LinkBlocks[i].Name.c_str());
			}
			results.push_back(std::string(ServerInstance->Config->ServerName)+" 219 "+user->nick+" "+statschar+" :End of /STATS report");
			ServerInstance->SNO->WriteToSnoMask('t',"Notice: %s '%c' requested by %s (%s@%s)",(!strcmp(user->server,ServerInstance->Config->ServerName) ? "Stats" : "Remote stats"),statschar,user->nick,user->ident,user->host);
			return 1;
		}
		return 0;
	}

	virtual int OnPreCommand(const std::string &command, const char** parameters, int pcnt, userrec *user, bool validated, const std::string &original_line)
	{
		/* If the command doesnt appear to be valid, we dont want to mess with it. */
		if (!validated)
			return 0;

		if (command == "CONNECT")
		{
			return this->HandleConnect(parameters,pcnt,user);
		}
		else if (command == "STATS")
		{
			return this->HandleStats(parameters,pcnt,user);
		}
		else if (command == "MOTD")
		{
			return this->HandleMotd(parameters,pcnt,user);
		}
		else if (command == "ADMIN")
		{
			return this->HandleAdmin(parameters,pcnt,user);
		}
		else if (command == "SQUIT")
		{
			return this->HandleSquit(parameters,pcnt,user);
		}
		else if (command == "MAP")
		{
			this->HandleMap(parameters,pcnt,user);
			return 1;
		}
		else if ((command == "TIME") && (pcnt > 0))
		{
			return this->HandleTime(parameters,pcnt,user);
		}
		else if (command == "LUSERS")
		{
			this->HandleLusers(parameters,pcnt,user);
			return 1;
		}
		else if (command == "LINKS")
		{
			this->HandleLinks(parameters,pcnt,user);
			return 1;
		}
		else if (command == "WHOIS")
		{
			if (pcnt > 1)
			{
				// remote whois
				return this->HandleRemoteWhois(parameters,pcnt,user);
			}
		}
		else if ((command == "VERSION") && (pcnt > 0))
		{
			this->HandleVersion(parameters,pcnt,user);
			return 1;
		}

		return 0;
	}

	virtual void OnPostCommand(const std::string &command, const char** parameters, int pcnt, userrec *user, CmdResult result, const std::string &original_line)
	{
		if ((result == CMD_SUCCESS) && (ServerInstance->IsValidModuleCommand(command, pcnt, user)))
		{
			// this bit of code cleverly routes all module commands
			// to all remote severs *automatically* so that modules
			// can just handle commands locally, without having
			// to have any special provision in place for remote
			// commands and linking protocols.
			std::deque<std::string> params;
			params.clear();
			for (int j = 0; j < pcnt; j++)
			{
				if (strchr(parameters[j],' '))
				{
					params.push_back(":" + std::string(parameters[j]));
				}
				else
				{
					params.push_back(std::string(parameters[j]));
				}
			}
			ServerInstance->Log(DEBUG,"Globally route '%s'",command.c_str());
			Utils->DoOneToMany(user->nick,command,params);
		}
	}

	virtual void OnGetServerDescription(const std::string &servername,std::string &description)
	{
		TreeServer* s = Utils->FindServer(servername);
		if (s)
		{
			description = s->GetDesc();
		}
	}

	virtual void OnUserInvite(userrec* source,userrec* dest,chanrec* channel)
	{
		if (IS_LOCAL(source))
		{
			std::deque<std::string> params;
			params.push_back(dest->nick);
			params.push_back(channel->name);
			Utils->DoOneToMany(source->nick,"INVITE",params);
		}
	}

	virtual void OnPostLocalTopicChange(userrec* user, chanrec* chan, const std::string &topic)
	{
		std::deque<std::string> params;
		params.push_back(chan->name);
		params.push_back(":"+topic);
		Utils->DoOneToMany(user->nick,"TOPIC",params);
	}

	virtual void OnWallops(userrec* user, const std::string &text)
	{
		if (IS_LOCAL(user))
		{
			std::deque<std::string> params;
			params.push_back(":"+text);
			Utils->DoOneToMany(user->nick,"WALLOPS",params);
		}
	}

	virtual void OnUserNotice(userrec* user, void* dest, int target_type, const std::string &text, char status, const CUList &exempt_list)
	{
		if (target_type == TYPE_USER)
		{
			userrec* d = (userrec*)dest;
			if ((d->GetFd() < 0) && (IS_LOCAL(user)))
			{
				std::deque<std::string> params;
				params.clear();
				params.push_back(d->nick);
				params.push_back(":"+text);
				Utils->DoOneToOne(user->nick,"NOTICE",params,d->server);
			}
		}
		else if (target_type == TYPE_CHANNEL)
		{
			if (IS_LOCAL(user))
			{
				chanrec *c = (chanrec*)dest;
				if (c)
				{
					std::string cname = c->name;
					if (status)
						cname = status + cname;
					TreeServerList list;
					Utils->GetListOfServersForChannel(c,list,status,exempt_list);

					for (TreeServerList::iterator i = list.begin(); i != list.end(); i++)
					{
						TreeSocket* Sock = i->second->GetSocket();
						if (Sock)
							Sock->WriteLine(":"+std::string(user->nick)+" NOTICE "+cname+" :"+text);
					}
				}
			}
		}
                else if (target_type == TYPE_SERVER)
		{
			if (IS_LOCAL(user))
			{
				char* target = (char*)dest;
				std::deque<std::string> par;
				par.push_back(target);
				par.push_back(":"+text);
				Utils->DoOneToMany(user->nick,"NOTICE",par);
			}
		}
	}

	virtual void OnUserMessage(userrec* user, void* dest, int target_type, const std::string &text, char status, const CUList &exempt_list)
	{
		if (target_type == TYPE_USER)
		{
			// route private messages which are targetted at clients only to the server
			// which needs to receive them
			userrec* d = (userrec*)dest;
			if ((d->GetFd() < 0) && (IS_LOCAL(user)))
			{
				std::deque<std::string> params;
				params.clear();
				params.push_back(d->nick);
				params.push_back(":"+text);
				Utils->DoOneToOne(user->nick,"PRIVMSG",params,d->server);
			}
		}
		else if (target_type == TYPE_CHANNEL)
		{
			if (IS_LOCAL(user))
			{
				chanrec *c = (chanrec*)dest;
				if (c)
				{
					std::string cname = c->name;
					if (status)
						cname = status + cname;
					TreeServerList list;
					Utils->GetListOfServersForChannel(c,list,status,exempt_list);

					for (TreeServerList::iterator i = list.begin(); i != list.end(); i++)
					{
						TreeSocket* Sock = i->second->GetSocket();
						if (Sock)
							Sock->WriteLine(":"+std::string(user->nick)+" PRIVMSG "+cname+" :"+text);
					}
				}
			}
		}
		else if (target_type == TYPE_SERVER)
		{
			if (IS_LOCAL(user))
			{
				char* target = (char*)dest;
				std::deque<std::string> par;
				par.push_back(target);
				par.push_back(":"+text);
				Utils->DoOneToMany(user->nick,"PRIVMSG",par);
			}
		}
	}

	virtual void OnBackgroundTimer(time_t curtime)
	{
		AutoConnectServers(curtime);
		DoPingChecks(curtime);
	}

	virtual void OnUserJoin(userrec* user, chanrec* channel)
	{
		// Only do this for local users
		if (IS_LOCAL(user))
		{
			if (channel->GetUserCounter() == 1)
			{
				std::deque<std::string> params;
				// set up their permissions and the channel TS with FJOIN.
				// All users are FJOINed now, because a module may specify
				// new joining permissions for the user.
				params.push_back(channel->name);
				params.push_back(ConvToStr(channel->age));
				params.push_back(std::string(channel->GetAllPrefixChars(user))+","+std::string(user->nick));
				Utils->DoOneToMany(ServerInstance->Config->ServerName,"FJOIN",params);
				/* First user in, sync the modes for the channel */
				params.pop_back();
				/* This is safe, all inspircd servers default to +nt */
				params.push_back("+nt");
				Utils->DoOneToMany(ServerInstance->Config->ServerName,"FMODE",params);
			}
			else
			{
				std::deque<std::string> params;
				params.push_back(channel->name);
				params.push_back(ConvToStr(channel->age));
				Utils->DoOneToMany(user->nick,"JOIN",params);
			}
		}
	}

	virtual void OnChangeHost(userrec* user, const std::string &newhost)
	{
		// only occurs for local clients
		if (user->registered != REG_ALL)
			return;
		std::deque<std::string> params;
		params.push_back(newhost);
		Utils->DoOneToMany(user->nick,"FHOST",params);
	}

	virtual void OnChangeName(userrec* user, const std::string &gecos)
	{
		// only occurs for local clients
		if (user->registered != REG_ALL)
			return;
		std::deque<std::string> params;
		params.push_back(gecos);
		Utils->DoOneToMany(user->nick,"FNAME",params);
	}

	virtual void OnUserPart(userrec* user, chanrec* channel, const std::string &partmessage)
	{
		if (IS_LOCAL(user))
		{
			std::deque<std::string> params;
			params.push_back(channel->name);
			if (partmessage != "")
				params.push_back(":"+partmessage);
			Utils->DoOneToMany(user->nick,"PART",params);
		}
	}

	virtual void OnUserConnect(userrec* user)
	{
		char agestr[MAXBUF];
		if (IS_LOCAL(user))
		{
			std::deque<std::string> params;
			snprintf(agestr,MAXBUF,"%lu",(unsigned long)user->age);
			params.push_back(agestr);
			params.push_back(user->nick);
			params.push_back(user->host);
			params.push_back(user->dhost);
			params.push_back(user->ident);
			params.push_back("+"+std::string(user->FormatModes()));
			params.push_back(user->GetIPString());
			params.push_back(":"+std::string(user->fullname));
			Utils->DoOneToMany(ServerInstance->Config->ServerName,"NICK",params);

			// User is Local, change needs to be reflected!
			TreeServer* SourceServer = Utils->FindServer(user->server);
			if (SourceServer)
			{
				SourceServer->AddUserCount();
			}

		}
	}

	virtual void OnUserQuit(userrec* user, const std::string &reason)
	{
		if ((IS_LOCAL(user)) && (user->registered == REG_ALL))
		{
			std::deque<std::string> params;
			params.push_back(":"+reason);
			Utils->DoOneToMany(user->nick,"QUIT",params);
		}
		// Regardless, We need to modify the user Counts..
		TreeServer* SourceServer = Utils->FindServer(user->server);
		if (SourceServer)
		{
			SourceServer->DelUserCount();
		}

	}

	virtual void OnUserPostNick(userrec* user, const std::string &oldnick)
	{
		if (IS_LOCAL(user))
		{
			std::deque<std::string> params;
			params.push_back(user->nick);
			Utils->DoOneToMany(oldnick,"NICK",params);
		}
	}

	virtual void OnUserKick(userrec* source, userrec* user, chanrec* chan, const std::string &reason)
	{
		if ((source) && (IS_LOCAL(source)))
		{
			std::deque<std::string> params;
			params.push_back(chan->name);
			params.push_back(user->nick);
			params.push_back(":"+reason);
			Utils->DoOneToMany(source->nick,"KICK",params);
		}
		else if (!source)
		{
			std::deque<std::string> params;
			params.push_back(chan->name);
			params.push_back(user->nick);
			params.push_back(":"+reason);
			Utils->DoOneToMany(ServerInstance->Config->ServerName,"KICK",params);
		}
	}

	virtual void OnRemoteKill(userrec* source, userrec* dest, const std::string &reason)
	{
		std::deque<std::string> params;
		params.push_back(dest->nick);
		params.push_back(":"+reason);
		Utils->DoOneToMany(source->nick,"KILL",params);
	}

	virtual void OnRehash(userrec* user, const std::string &parameter)
	{
		if (parameter != "")
		{
			std::deque<std::string> params;
			params.push_back(parameter);
			Utils->DoOneToMany(user ? user->nick : ServerInstance->Config->ServerName, "REHASH", params);
			// check for self
			if (ServerInstance->MatchText(ServerInstance->Config->ServerName,parameter))
			{
				ServerInstance->WriteOpers("*** Remote rehash initiated locally by \002%s\002", user ? user->nick : ServerInstance->Config->ServerName);
				ServerInstance->RehashServer();
			}
		}
		Utils->ReadConfiguration(false);
	}

	// note: the protocol does not allow direct umode +o except
	// via NICK with 8 params. sending OPERTYPE infers +o modechange
	// locally.
	virtual void OnOper(userrec* user, const std::string &opertype)
	{
		if (IS_LOCAL(user))
		{
			std::deque<std::string> params;
			params.push_back(opertype);
			Utils->DoOneToMany(user->nick,"OPERTYPE",params);
		}
	}

	void OnLine(userrec* source, const std::string &host, bool adding, char linetype, long duration, const std::string &reason)
	{
		if (!source)
		{
			/* Server-set lines */
			char data[MAXBUF];
			snprintf(data,MAXBUF,"%c %s %s %lu %lu :%s", linetype, host.c_str(), ServerInstance->Config->ServerName, (unsigned long)ServerInstance->Time(false),
					(unsigned long)duration, reason.c_str());
			std::deque<std::string> params;
			params.push_back(data);
			Utils->DoOneToMany(ServerInstance->Config->ServerName, "ADDLINE", params);
		}
		else
		{
			if (IS_LOCAL(source))
			{
				char type[8];
				snprintf(type,8,"%cLINE",linetype);
				std::string stype = type;
				if (adding)
				{
					char sduration[MAXBUF];
					snprintf(sduration,MAXBUF,"%ld",duration);
					std::deque<std::string> params;
					params.push_back(host);
					params.push_back(sduration);
					params.push_back(":"+reason);
					Utils->DoOneToMany(source->nick,stype,params);
				}
				else
				{
					std::deque<std::string> params;
					params.push_back(host);
					Utils->DoOneToMany(source->nick,stype,params);
				}
			}
		}
	}

	virtual void OnAddGLine(long duration, userrec* source, const std::string &reason, const std::string &hostmask)
	{
		OnLine(source,hostmask,true,'G',duration,reason);
	}
	
	virtual void OnAddZLine(long duration, userrec* source, const std::string &reason, const std::string &ipmask)
	{
		OnLine(source,ipmask,true,'Z',duration,reason);
	}

	virtual void OnAddQLine(long duration, userrec* source, const std::string &reason, const std::string &nickmask)
	{
		OnLine(source,nickmask,true,'Q',duration,reason);
	}

	virtual void OnAddELine(long duration, userrec* source, const std::string &reason, const std::string &hostmask)
	{
		OnLine(source,hostmask,true,'E',duration,reason);
	}

	virtual void OnDelGLine(userrec* source, const std::string &hostmask)
	{
		OnLine(source,hostmask,false,'G',0,"");
	}

	virtual void OnDelZLine(userrec* source, const std::string &ipmask)
	{
		OnLine(source,ipmask,false,'Z',0,"");
	}

	virtual void OnDelQLine(userrec* source, const std::string &nickmask)
	{
		OnLine(source,nickmask,false,'Q',0,"");
	}

	virtual void OnDelELine(userrec* source, const std::string &hostmask)
	{
		OnLine(source,hostmask,false,'E',0,"");
	}

	virtual void OnMode(userrec* user, void* dest, int target_type, const std::string &text)
	{
		if ((IS_LOCAL(user)) && (user->registered == REG_ALL))
		{
			if (target_type == TYPE_USER)
			{
				userrec* u = (userrec*)dest;
				std::deque<std::string> params;
				params.push_back(u->nick);
				params.push_back(text);
				Utils->DoOneToMany(user->nick,"MODE",params);
			}
			else
			{
				chanrec* c = (chanrec*)dest;
				std::deque<std::string> params;
				params.push_back(c->name);
				params.push_back(text);
				Utils->DoOneToMany(user->nick,"MODE",params);
			}
		}
	}

	virtual void OnSetAway(userrec* user)
	{
		if (IS_LOCAL(user))
		{
			std::deque<std::string> params;
			params.push_back(":"+std::string(user->awaymsg));
			Utils->DoOneToMany(user->nick,"AWAY",params);
		}
	}

	virtual void OnCancelAway(userrec* user)
	{
		if (IS_LOCAL(user))
		{
			std::deque<std::string> params;
			params.clear();
			Utils->DoOneToMany(user->nick,"AWAY",params);
		}
	}

	virtual void ProtoSendMode(void* opaque, int target_type, void* target, const std::string &modeline)
	{
		TreeSocket* s = (TreeSocket*)opaque;
		if (target)
		{
			if (target_type == TYPE_USER)
			{
				userrec* u = (userrec*)target;
				s->WriteLine(std::string(":")+ServerInstance->Config->ServerName+" FMODE "+u->nick+" "+ConvToStr(u->age)+" "+modeline);
			}
			else
			{
				chanrec* c = (chanrec*)target;
				s->WriteLine(std::string(":")+ServerInstance->Config->ServerName+" FMODE "+c->name+" "+ConvToStr(c->age)+" "+modeline);
			}
		}
	}

	virtual void ProtoSendMetaData(void* opaque, int target_type, void* target, const std::string &extname, const std::string &extdata)
	{
		TreeSocket* s = (TreeSocket*)opaque;
		if (target)
		{
			if (target_type == TYPE_USER)
			{
				userrec* u = (userrec*)target;
				s->WriteLine(std::string(":")+ServerInstance->Config->ServerName+" METADATA "+u->nick+" "+extname+" :"+extdata);
			}
			else if (target_type == TYPE_CHANNEL)
			{
				chanrec* c = (chanrec*)target;
				s->WriteLine(std::string(":")+ServerInstance->Config->ServerName+" METADATA "+c->name+" "+extname+" :"+extdata);
			}
		}
		if (target_type == TYPE_OTHER)
		{
			s->WriteLine(std::string(":")+ServerInstance->Config->ServerName+" METADATA * "+extname+" :"+extdata);
		}
	}

	virtual void OnEvent(Event* event)
	{
		std::deque<std::string>* params = (std::deque<std::string>*)event->GetData();

		if (event->GetEventID() == "send_metadata")
		{
			if (params->size() < 3)
				return;
			(*params)[2] = ":" + (*params)[2];
			Utils->DoOneToMany(ServerInstance->Config->ServerName,"METADATA",*params);
		}
		else if (event->GetEventID() == "send_topic")
		{
			if (params->size() < 2)
				return;
			(*params)[1] = ":" + (*params)[1];
			params->insert(params->begin() + 1,ServerInstance->Config->ServerName);
			params->insert(params->begin() + 1,ConvToStr(ServerInstance->Time(true)));
			Utils->DoOneToMany(ServerInstance->Config->ServerName,"FTOPIC",*params);
		}
		else if (event->GetEventID() == "send_mode")
		{
			if (params->size() < 2)
				return;
			// Insert the TS value of the object, either userrec or chanrec
			time_t ourTS = 0;
			userrec* a = ServerInstance->FindNick((*params)[0]);
			if (a)
			{
				ourTS = a->age;
			}
			else
			{
				chanrec* a = ServerInstance->FindChan((*params)[0]);
				if (a)
				{
					ourTS = a->age;
				}
			}
			params->insert(params->begin() + 1,ConvToStr(ourTS));
			Utils->DoOneToMany(ServerInstance->Config->ServerName,"FMODE",*params);
		}
		else if (event->GetEventID() == "send_mode_explicit")
		{
			if (params->size() < 2)
				return;
			Utils->DoOneToMany(ServerInstance->Config->ServerName,"MODE",*params);
		}
		else if (event->GetEventID() == "send_opers")
		{
			if (params->size() < 1)
				return;
			(*params)[0] = ":" + (*params)[0];
			Utils->DoOneToMany(ServerInstance->Config->ServerName,"OPERNOTICE",*params);
		}
		else if (event->GetEventID() == "send_modeset")
		{
			if (params->size() < 2)
				return;
			(*params)[1] = ":" + (*params)[1];
			Utils->DoOneToMany(ServerInstance->Config->ServerName,"MODENOTICE",*params);
		}
		else if (event->GetEventID() == "send_snoset")
		{
			if (params->size() < 2)
				return;
			(*params)[1] = ":" + (*params)[1];
			Utils->DoOneToMany(ServerInstance->Config->ServerName,"SNONOTICE",*params);
		}
		else if (event->GetEventID() == "send_push")
		{
			if (params->size() < 2)
				return;
			
			userrec *a = ServerInstance->FindNick((*params)[0]);
			
			if (!a)
				return;
			
			(*params)[1] = ":" + (*params)[1];
			Utils->DoOneToOne(ServerInstance->Config->ServerName, "PUSH", *params, a->server);
		}
	}

	virtual ~ModuleSpanningTree()
	{
		ServerInstance->Log(DEBUG,"Performing unload of spanningtree!");
		/* This will also free the listeners */
		delete Utils;
		if (SyncTimer)
			ServerInstance->Timers->DelTimer(SyncTimer);

		ServerInstance->DoneWithInterface("InspSocketHook");
	}

	virtual Version GetVersion()
	{
		return Version(1,1,0,2,VF_VENDOR,API_VERSION);
	}

	void Implements(char* List)
	{
		List[I_OnPreCommand] = List[I_OnGetServerDescription] = List[I_OnUserInvite] = List[I_OnPostLocalTopicChange] = 1;
		List[I_OnWallops] = List[I_OnUserNotice] = List[I_OnUserMessage] = List[I_OnBackgroundTimer] = 1;
		List[I_OnUserJoin] = List[I_OnChangeHost] = List[I_OnChangeName] = List[I_OnUserPart] = List[I_OnUserConnect] = 1;
		List[I_OnUserQuit] = List[I_OnUserPostNick] = List[I_OnUserKick] = List[I_OnRemoteKill] = List[I_OnRehash] = 1;
		List[I_OnOper] = List[I_OnAddGLine] = List[I_OnAddZLine] = List[I_OnAddQLine] = List[I_OnAddELine] = 1;
		List[I_OnDelGLine] = List[I_OnDelZLine] = List[I_OnDelQLine] = List[I_OnDelELine] = List[I_ProtoSendMode] = List[I_OnMode] = 1;
		List[I_OnStats] = List[I_ProtoSendMetaData] = List[I_OnEvent] = List[I_OnSetAway] = List[I_OnCancelAway] = List[I_OnPostCommand] = 1;
	}

	/* It is IMPORTANT that m_spanningtree is the last module in the chain
	 * so that any activity it sees is FINAL, e.g. we arent going to send out
	 * a NICK message before m_cloaking has finished putting the +x on the user,
	 * etc etc.
	 * Therefore, we return PRIORITY_LAST to make sure we end up at the END of
	 * the module call queue.
	 */
	Priority Prioritize()
	{
		return PRIORITY_LAST;
	}
};

TimeSyncTimer::TimeSyncTimer(InspIRCd *Inst, ModuleSpanningTree *Mod) : InspTimer(43200, Inst->Time()), Instance(Inst), Module(Mod)
{
}

void TimeSyncTimer::Tick(time_t TIME)
{
	Module->BroadcastTimeSync();
	Module->SyncTimer = new TimeSyncTimer(Instance, Module);
	Instance->Timers->AddTimer(Module->SyncTimer);
}

void SpanningTreeUtilities::DoFailOver(Link* x)
{
	if (x->FailOver.length())
	{
		if (x->FailOver == x->Name)
		{
			ServerInstance->SNO->WriteToSnoMask('l',"FAILOVER: Some muppet configured the failover for server \002%s\002 to point at itself. Not following it!", x->Name.c_str());
			return;
		}
		Link* TryThisOne = this->FindLink(x->FailOver.c_str());
		if (TryThisOne)
		{
			ServerInstance->SNO->WriteToSnoMask('l',"FAILOVER: Trying failover link for \002%s\002: \002%s\002...", x->Name.c_str(), TryThisOne->Name.c_str());
			Creator->ConnectServer(TryThisOne);
		}
		else
		{
			ServerInstance->SNO->WriteToSnoMask('l',"FAILOVER: Invalid failover server specified for server \002%s\002, will not follow!", x->Name.c_str());
		}
	}
}

Link* SpanningTreeUtilities::FindLink(const std::string& name)
{
	for (std::vector<Link>::iterator x = LinkBlocks.begin(); x < LinkBlocks.end(); x++)
	{
		if (ServerInstance->MatchText(x->Name.c_str(), name.c_str()))
		{
			return &(*x);
		}
	}
	return NULL;
}

class ModuleSpanningTreeFactory : public ModuleFactory
{
 public:
	ModuleSpanningTreeFactory()
	{
	}
	
	~ModuleSpanningTreeFactory()
	{
	}
	
	virtual Module * CreateModule(InspIRCd* Me)
	{
		return new ModuleSpanningTree(Me);
	}
	
};


extern "C" void * init_module( void )
{
	return new ModuleSpanningTreeFactory;
}
