/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  InspIRCd is copyright (C) 2002-2006 ChatSpike-Dev.
 *                       E-mail:
 *                <brain@chatspike.net>
 *                <Craig@chatspike.net>
 *
 * Written by Craig Edwards, Craig McLure, and others.
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

#include "inspircd.h"
#include <sys/select.h>
#include "socketengine_select.h"


SelectEngine::SelectEngine(InspIRCd* Instance) : SocketEngine(Instance)
{
	ServerInstance->Log(DEBUG,"SelectEngine::SelectEngine()");
	EngineHandle = 0;
	CurrentSetSize = 0;
	memset(writeable, 0, sizeof(writeable));
}

SelectEngine::~SelectEngine()
{
	ServerInstance->Log(DEBUG,"SelectEngine::~SelectEngine()");
}

bool SelectEngine::AddFd(EventHandler* eh)
{
	int fd = eh->GetFd();
	if ((fd < 0) || (fd > MAX_DESCRIPTORS))
	{
		ServerInstance->Log(DEBUG,"ERROR: FD of %d added above max of %d",fd,MAX_DESCRIPTORS);
		return false;
	}
	if (GetRemainingFds() <= 1)
	{
		ServerInstance->Log(DEBUG,"ERROR: System out of file descriptors!");
		return false;
	}

	fds[fd] = fd;

	if (ref[fd])
		return false;

	ref[fd] = eh;
	ServerInstance->Log(DEBUG,"Add socket %d",fd);

	CurrentSetSize++;
	return true;
}

void SelectEngine::WantWrite(EventHandler* eh)
{
	writeable[eh->GetFd()] = true;
}

bool SelectEngine::DelFd(EventHandler* eh)
{
	int fd = eh->GetFd();

	ServerInstance->Log(DEBUG,"SelectEngine::DelFd(%d)",fd);

	if ((fd < 0) || (fd > MAX_DESCRIPTORS))
		return false;

	std::map<int,int>::iterator t = fds.find(fd);
	if (t != fds.end())
	{
		fds.erase(t);
		ServerInstance->Log(DEBUG,"Deleted fd %d",fd);
	}

	CurrentSetSize--;
	ref[fd] = NULL;
	return true;
}

int SelectEngine::GetMaxFds()
{
	return FD_SETSIZE;
}

int SelectEngine::GetRemainingFds()
{
	return FD_SETSIZE - CurrentSetSize;
}

int SelectEngine::DispatchEvents()
{
	int result = 0;
	timeval tval;
	int sresult = 0;
	EventHandler* ev[MAX_DESCRIPTORS];
	socklen_t codesize;
	int errcode;

	FD_ZERO(&wfdset);
	FD_ZERO(&rfdset);
	FD_ZERO(&errfdset);

	for (std::map<int,int>::iterator a = fds.begin(); a != fds.end(); a++)
	{
		if (ref[a->second]->Readable())
			FD_SET (a->second, &rfdset);
		else
			FD_SET (a->second, &wfdset);
		if (writeable[a->second])
			FD_SET (a->second, &wfdset);

		FD_SET (a->second, &errfdset);
	}
	tval.tv_sec = 0;
	tval.tv_usec = 50L;
	sresult = select(FD_SETSIZE, &rfdset, &wfdset, &errfdset, &tval);
	if (sresult > 0)
	{
		for (std::map<int,int>::iterator a = fds.begin(); a != fds.end(); a++)
		{
			if ((FD_ISSET (a->second, &rfdset)) || (FD_ISSET (a->second, &wfdset)) || FD_ISSET (a->second, &errfdset))
			{
				ev[result++] = ref[a->second];
			}
		}
	}

	/** An event handler may remove its own descriptor from the list, therefore it is not
	 * safe to directly iterate over the list and dispatch events there with STL iterators.
	 * Thats a shame because it makes this code slower and more resource intensive, but maybe
	 * the user should stop using select(), as select() smells anyway.
	 */
	for (int i = 0; i < result; i++)
	{
		if (ev[i])
		{
			if (FD_ISSET (ev[i]->GetFd(), &errfdset))
			{
				if (ev[i])
				{
					if (getsockopt(ev[i]->GetFd(), SOL_SOCKET, SO_ERROR, &errcode, &codesize) < 0)
						errcode = errno;

					ev[i]->HandleEvent(EVENT_ERROR, errcode);
				}
				continue;
			}
			if (ev[i])
			{
				ServerInstance->Log(DEBUG,"Handle %s event on fd %d",writeable[ev[i]->GetFd()] || !ev[i]->Readable() ? "write" : "read", ev[i]->GetFd());
				if (writeable[ev[i]->GetFd()])
				{
					if (ev[i])
						ev[i]->HandleEvent(EVENT_WRITE);
					writeable[ev[i]->GetFd()] = false;
		
				}
				else
				{
					if (ev[i])
						ev[i]->HandleEvent(ev[i]->Readable() ? EVENT_READ : EVENT_WRITE);
				}
			}
		}
	}

	return result;
}

std::string SelectEngine::GetName()
{
	return "select";
}
