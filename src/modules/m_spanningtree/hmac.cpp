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
#include "commands/cmd_whois.h"
#include "commands/cmd_stats.h"
#include "socket.h"
#include "wildcard.h"
#include "xline.h"
#include "transport.h"
#include "m_hash.h"
#include "socketengine.h"

#include "m_spanningtree/main.h"
#include "m_spanningtree/utils.h"
#include "m_spanningtree/treeserver.h"
#include "m_spanningtree/link.h"
#include "m_spanningtree/treesocket.h"
#include "m_spanningtree/resolvers.h"
#include "m_spanningtree/handshaketimer.h"

/* $ModDep: m_spanningtree/timesynctimer.h m_spanningtree/resolvers.h m_spanningtree/main.h m_spanningtree/utils.h m_spanningtree/treeserver.h m_spanningtree/link.h m_spanningtree/treesocket.h m_hash.h */

const std::string& TreeSocket::GetOurChallenge()
{
	return this->ourchallenge;
}

void TreeSocket::SetOurChallenge(const std::string &c)
{
	this->ourchallenge = c;
}

const std::string& TreeSocket::GetTheirChallenge()
{
	return this->theirchallenge;
}

void TreeSocket::SetTheirChallenge(const std::string &c)
{
	this->theirchallenge = c;
}

std::string TreeSocket::MakePass(const std::string &password, const std::string &challenge)
{
	/* This is a simple (maybe a bit hacky?) HMAC algorithm, thanks to jilles for
	 * suggesting the use of HMAC to secure the password against various attacks.
	 *
	 * Note: If m_sha256.so is not loaded, we MUST fall back to plaintext with no
	 *       HMAC challenge/response.
	 */
	Module* sha256 = Instance->Modules->Find("m_sha256.so");
	if (Utils->ChallengeResponse && sha256 && !challenge.empty())
	{
		/* XXX: This is how HMAC is supposed to be done:
		 *
		 * sha256( (pass xor 0x5c) + sha256((pass xor 0x36) + m) )
		 *
		 * Note that we are encoding the hex hash, not the binary
		 * output of the hash which is slightly different to standard.
		 *
		 * Don't ask me why its always 0x5c and 0x36... it just is.
		 */
		std::string hmac1, hmac2;

		for (size_t n = 0; n < password.length(); n++)
		{
			hmac1 += static_cast<char>(password[n] ^ 0x5C);
			hmac2 += static_cast<char>(password[n] ^ 0x36);
		}

		hmac2 += challenge;
		HashResetRequest(Utils->Creator, sha256).Send();
		hmac2 = HashSumRequest(Utils->Creator, sha256, hmac2).Send();

		HashResetRequest(Utils->Creator, sha256).Send();
		std::string hmac = hmac1 + hmac2;
		hmac = HashSumRequest(Utils->Creator, sha256, hmac).Send();

		return "HMAC-SHA256:"+ hmac;
	}
	else if (!challenge.empty() && !sha256)
		Instance->Log(DEFAULT,"Not authenticating to server using SHA256/HMAC because we don't have m_sha256 loaded!");

	return password;
}

std::string TreeSocket::RandString(unsigned int length)
{
	char* randombuf = new char[length+1];
	std::string out;
#ifdef WINDOWS
	int fd = -1;
#else
	int fd = open("/dev/urandom", O_RDONLY, 0);
#endif

	if (fd >= 0)
	{
#ifndef WINDOWS
		read(fd, randombuf, length);
		close(fd);
#endif
	}
	else
	{
		for (unsigned int i = 0; i < length; i++)
			randombuf[i] = rand();
	}

	for (unsigned int i = 0; i < length; i++)
	{
		char randchar = static_cast<char>((randombuf[i] & 0x7F) | 0x21);
		out += (randchar == '=' ? '_' : randchar);
	}

	delete[] randombuf;
	return out;
}

