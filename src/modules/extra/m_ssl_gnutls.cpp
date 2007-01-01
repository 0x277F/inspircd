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

#include <string>
#include <vector>

#include <gnutls/gnutls.h>
#include <gnutls/x509.h>

#include "inspircd_config.h"
#include "configreader.h"
#include "users.h"
#include "channels.h"
#include "modules.h"

#include "socket.h"
#include "hashcomp.h"
#include "inspircd.h"

#include "transport.h"

/* $ModDesc: Provides SSL support for clients */
/* $CompileFlags: `libgnutls-config --cflags` */
/* $LinkerFlags: `perl extra/gnutls_rpath.pl` */
/* $ModDep: transport.h */


enum issl_status { ISSL_NONE, ISSL_HANDSHAKING_READ, ISSL_HANDSHAKING_WRITE, ISSL_HANDSHAKEN, ISSL_CLOSING, ISSL_CLOSED };

bool isin(int port, const std::vector<int> &portlist)
{
	for(unsigned int i = 0; i < portlist.size(); i++)
		if(portlist[i] == port)
			return true;
			
	return false;
}

/** Represents an SSL user's extra data
 */
class issl_session : public classbase
{
public:
	gnutls_session_t sess;
	issl_status status;
	std::string outbuf;
	int inbufoffset;
	char* inbuf;
	int fd;
};

class ModuleSSLGnuTLS : public Module
{
	
	ConfigReader* Conf;

	char* dummy;
	
	CullList* culllist;
	
	std::vector<int> listenports;
	
	int inbufsize;
	issl_session sessions[MAX_DESCRIPTORS];
	
	gnutls_certificate_credentials x509_cred;
	gnutls_dh_params dh_params;
	
	std::string keyfile;
	std::string certfile;
	std::string cafile;
	std::string crlfile;
	int dh_bits;
	
 public:
	
	ModuleSSLGnuTLS(InspIRCd* Me)
		: Module::Module(Me)
	{
		

		culllist = new CullList(ServerInstance);

		ServerInstance->PublishInterface("InspSocketHook", this);
		
		// Not rehashable...because I cba to reduce all the sizes of existing buffers.
		inbufsize = ServerInstance->Config->NetBufferSize;
		
		gnutls_global_init(); // This must be called once in the program

		if(gnutls_certificate_allocate_credentials(&x509_cred) != 0)
			ServerInstance->Log(DEFAULT, "m_ssl_gnutls.so: Failed to allocate certificate credentials");

		// Guessing return meaning
		if(gnutls_dh_params_init(&dh_params) < 0)
			ServerInstance->Log(DEFAULT, "m_ssl_gnutls.so: Failed to initialise DH parameters");

		// Needs the flag as it ignores a plain /rehash
		OnRehash("ssl");
		
		// Void return, guess we assume success
		gnutls_certificate_set_dh_params(x509_cred, dh_params);
	}
	
	virtual void OnRehash(const std::string &param)
	{
		if(param != "ssl")
			return;
	
		Conf = new ConfigReader(ServerInstance);
		
		for(unsigned int i = 0; i < listenports.size(); i++)
		{
			ServerInstance->Config->DelIOHook(listenports[i]);
		}
		
		listenports.clear();
		
		for(int i = 0; i < Conf->Enumerate("bind"); i++)
		{
			// For each <bind> tag
			if(((Conf->ReadValue("bind", "type", i) == "") || (Conf->ReadValue("bind", "type", i) == "clients")) && (Conf->ReadValue("bind", "ssl", i) == "gnutls"))
			{
				// Get the port we're meant to be listening on with SSL
				std::string port = Conf->ReadValue("bind", "port", i);
				irc::portparser portrange(port, false);
				long portno = -1;
				while ((portno = portrange.GetToken()))
				{
					if (ServerInstance->Config->AddIOHook(portno, this))
					{
						listenports.push_back(portno);
						for (unsigned int i = 0; i < ServerInstance->stats->BoundPortCount; i++)
							if (ServerInstance->Config->ports[i] == portno)
								ServerInstance->Config->openSockfd[i]->SetDescription("ssl");
						ServerInstance->Log(DEFAULT, "m_ssl_gnutls.so: Enabling SSL for port %d", portno);
					}
					else
					{
						ServerInstance->Log(DEFAULT, "m_ssl_gnutls.so: FAILED to enable SSL on port %d, maybe you have another ssl or similar module loaded?", portno);
					}
				}
			}
		}
		
		std::string confdir(CONFIG_FILE);
		// +1 so we the path ends with a /
		confdir = confdir.substr(0, confdir.find_last_of('/') + 1);
		
		cafile	= Conf->ReadValue("gnutls", "cafile", 0);
		crlfile	= Conf->ReadValue("gnutls", "crlfile", 0);
		certfile	= Conf->ReadValue("gnutls", "certfile", 0);
		keyfile	= Conf->ReadValue("gnutls", "keyfile", 0);
		dh_bits	= Conf->ReadInteger("gnutls", "dhbits", 0, false);
		
		// Set all the default values needed.
		if(cafile == "")
			cafile = "ca.pem";
			
		if(crlfile == "")
			crlfile = "crl.pem";
			
		if(certfile == "")
			certfile = "cert.pem";
			
		if(keyfile == "")
			keyfile = "key.pem";
			
		if((dh_bits != 768) && (dh_bits != 1024) && (dh_bits != 2048) && (dh_bits != 3072) && (dh_bits != 4096))
			dh_bits = 1024;
			
		// Prepend relative paths with the path to the config directory.	
		if(cafile[0] != '/')
			cafile = confdir + cafile;
		
		if(crlfile[0] != '/')
			crlfile = confdir + crlfile;
			
		if(certfile[0] != '/')
			certfile = confdir + certfile;
			
		if(keyfile[0] != '/')
			keyfile = confdir + keyfile;
		
		if(gnutls_certificate_set_x509_trust_file(x509_cred, cafile.c_str(), GNUTLS_X509_FMT_PEM) < 0)
			ServerInstance->Log(DEFAULT, "m_ssl_gnutls.so: Failed to set X.509 trust file: %s", cafile.c_str());
			
		if(gnutls_certificate_set_x509_crl_file (x509_cred, crlfile.c_str(), GNUTLS_X509_FMT_PEM) < 0)
			ServerInstance->Log(DEFAULT, "m_ssl_gnutls.so: Failed to set X.509 CRL file: %s", crlfile.c_str());
		
		// Guessing on the return value of this, manual doesn't say :|
		if(gnutls_certificate_set_x509_key_file (x509_cred, certfile.c_str(), keyfile.c_str(), GNUTLS_X509_FMT_PEM) < 0)
			ServerInstance->Log(DEFAULT, "m_ssl_gnutls.so: Failed to set X.509 certificate and key files: %s and %s", certfile.c_str(), keyfile.c_str());	
			
		// This may be on a large (once a day or week) timer eventually.
		GenerateDHParams();
		
		DELETE(Conf);
	}
	
	void GenerateDHParams()
	{
 		// Generate Diffie Hellman parameters - for use with DHE
		// kx algorithms. These should be discarded and regenerated
		// once a day, once a week or once a month. Depending on the
		// security requirements.
		
		if(gnutls_dh_params_generate2(dh_params, dh_bits) < 0)
			ServerInstance->Log(DEFAULT, "m_ssl_gnutls.so: Failed to generate DH parameters (%d bits)", dh_bits);
	}
	
	virtual ~ModuleSSLGnuTLS()
	{
		gnutls_dh_params_deinit(dh_params);
		gnutls_certificate_free_credentials(x509_cred);
		gnutls_global_deinit();
		delete culllist;
	}
	
	virtual void OnCleanup(int target_type, void* item)
	{
		if(target_type == TYPE_USER)
		{
			userrec* user = (userrec*)item;
			
			if(user->GetExt("ssl", dummy) && isin(user->GetPort(), listenports))
			{
				// User is using SSL, they're a local user, and they're using one of *our* SSL ports.
				// Potentially there could be multiple SSL modules loaded at once on different ports.
				ServerInstance->Log(DEBUG, "m_ssl_gnutls.so: Adding user %s to cull list", user->nick);
				culllist->AddItem(user, "SSL module unloading");
			}
			if (user->GetExt("ssl_cert", dummy) && isin(user->GetPort(), listenports))
			{
				ssl_cert* tofree;
				user->GetExt("ssl_cert", tofree);
				delete tofree;
				user->Shrink("ssl_cert");
			}
		}
	}
	
	virtual void OnUnloadModule(Module* mod, const std::string &name)
	{
		if(mod == this)
		{
			// We're being unloaded, kill all the users added to the cull list in OnCleanup
			int numusers = culllist->Apply();
			ServerInstance->Log(DEBUG, "m_ssl_gnutls.so: Killed %d users for unload of GnuTLS SSL module", numusers);
			
			for(unsigned int i = 0; i < listenports.size(); i++)
			{
				ServerInstance->Config->DelIOHook(listenports[i]);
				for (unsigned int j = 0; j < ServerInstance->stats->BoundPortCount; j++)
					if (ServerInstance->Config->ports[j] == listenports[i])
						ServerInstance->Config->openSockfd[j]->SetDescription("plaintext");
			}
		}
	}
	
	virtual Version GetVersion()
	{
		return Version(1, 1, 0, 0, VF_VENDOR, API_VERSION);
	}

	void Implements(char* List)
	{
		List[I_OnRawSocketConnect] = List[I_OnRawSocketAccept] = List[I_OnRawSocketClose] = List[I_OnRawSocketRead] = List[I_OnRawSocketWrite] = List[I_OnCleanup] = 1;
		List[I_OnRequest] = List[I_OnSyncUserMetaData] = List[I_OnDecodeMetaData] = List[I_OnUnloadModule] = List[I_OnRehash] = List[I_OnWhois] = List[I_OnPostConnect] = 1;
	}

        virtual char* OnRequest(Request* request)
	{
		ISHRequest* ISR = (ISHRequest*)request;
		if (strcmp("IS_NAME", request->GetId()) == 0)
		{
			return "gnutls";
		}
		else if (strcmp("IS_HOOK", request->GetId()) == 0)
		{
			char* ret = "OK";
			try
			{
				ret = ServerInstance->Config->AddIOHook((Module*)this, (InspSocket*)ISR->Sock) ? (char*)"OK" : NULL;
			}
			catch (ModuleException &e)
			{
				return NULL;
			}
			return ret;
		}
		else if (strcmp("IS_UNHOOK", request->GetId()) == 0)
		{
			return ServerInstance->Config->DelIOHook((InspSocket*)ISR->Sock) ? (char*)"OK" : NULL;
		}
		else if (strcmp("IS_HSDONE", request->GetId()) == 0)
		{
			issl_session* session = &sessions[ISR->Sock->GetFd()];
			return (session->status == ISSL_HANDSHAKING_READ || session->status == ISSL_HANDSHAKING_WRITE) ? NULL : (char*)"OK";
		}
		else if (strcmp("IS_ATTACH", request->GetId()) == 0)
		{
			issl_session* session = &sessions[ISR->Sock->GetFd()];
			if (session)
			{
				VerifyCertificate(session, (InspSocket*)ISR->Sock);
				return "OK";
			}
		}
		return NULL;
	}


	virtual void OnRawSocketAccept(int fd, const std::string &ip, int localport)
	{
		issl_session* session = &sessions[fd];
	
		session->fd = fd;
		session->inbuf = new char[inbufsize];
		session->inbufoffset = 0;
	
		gnutls_init(&session->sess, GNUTLS_SERVER);

		gnutls_set_default_priority(session->sess); // Avoid calling all the priority functions, defaults are adequate.
		gnutls_credentials_set(session->sess, GNUTLS_CRD_CERTIFICATE, x509_cred);
		gnutls_certificate_server_set_request(session->sess, GNUTLS_CERT_REQUEST); // Request client certificate if any.
		gnutls_dh_set_prime_bits(session->sess, dh_bits);
		
		/* This is an experimental change to avoid a warning on 64bit systems about casting between integer and pointer of different sizes
		 * This needs testing, but it's easy enough to rollback if need be
		 * Old: gnutls_transport_set_ptr(session->sess, (gnutls_transport_ptr_t) fd); // Give gnutls the fd for the socket.
		 * New: gnutls_transport_set_ptr(session->sess, &fd); // Give gnutls the fd for the socket.
		 *
		 * With testing this seems to...not work :/
		 */
		
		gnutls_transport_set_ptr(session->sess, (gnutls_transport_ptr_t) fd); // Give gnutls the fd for the socket.

		Handshake(session);
	}

	virtual void OnRawSocketConnect(int fd)
	{
		issl_session* session = &sessions[fd];

		session->fd = fd;
		session->inbuf = new char[inbufsize];
		session->inbufoffset = 0;

		gnutls_init(&session->sess, GNUTLS_SERVER);

		gnutls_set_default_priority(session->sess); // Avoid calling all the priority functions, defaults are adequate.
		gnutls_credentials_set(session->sess, GNUTLS_CRD_CERTIFICATE, x509_cred);
		gnutls_dh_set_prime_bits(session->sess, dh_bits);

		gnutls_transport_set_ptr(session->sess, (gnutls_transport_ptr_t) fd); // Give gnutls the fd for the socket.

		Handshake(session);
	}

	virtual void OnRawSocketClose(int fd)
	{
		ServerInstance->Log(DEBUG, "OnRawSocketClose: %d", fd);
		CloseSession(&sessions[fd]);

		EventHandler* user = ServerInstance->SE->GetRef(fd);

		if ((user) && (user->GetExt("ssl_cert", dummy)))
		{
			ssl_cert* tofree;
			user->GetExt("ssl_cert", tofree);
			delete tofree;
			user->Shrink("ssl_cert");
		}
	}
	
	virtual int OnRawSocketRead(int fd, char* buffer, unsigned int count, int &readresult)
	{
		issl_session* session = &sessions[fd];
		
		if (!session->sess)
		{
			ServerInstance->Log(DEBUG, "m_ssl_gnutls.so: OnRawSocketRead: No session to read from");
			readresult = 0;
			CloseSession(session);
			return 1;
		}

		if (session->status == ISSL_HANDSHAKING_READ)
		{
			// The handshake isn't finished, try to finish it.
			
			if(Handshake(session))
			{
				// Handshake successfully resumed.
				ServerInstance->Log(DEBUG, "m_ssl_gnutls.so: OnRawSocketRead: successfully resumed handshake");
			}
			else
			{
				// Couldn't resume handshake.	
				ServerInstance->Log(DEBUG, "m_ssl_gnutls.so: OnRawSocketRead: failed to resume handshake");
				return -1;
			}
		}
		else if (session->status == ISSL_HANDSHAKING_WRITE)
		{
			ServerInstance->Log(DEBUG, "m_ssl_gnutls.so: OnRawSocketRead: handshake wants to write data but we are currently reading");
			return -1;
		}
		
		// If we resumed the handshake then session->status will be ISSL_HANDSHAKEN.
		
		if (session->status == ISSL_HANDSHAKEN)
		{
			// Is this right? Not sure if the unencrypted data is garaunteed to be the same length.
			// Read into the inbuffer, offset from the beginning by the amount of data we have that insp hasn't taken yet.
			ServerInstance->Log(DEBUG, "m_ssl_gnutls.so: gnutls_record_recv(sess, inbuf+%d, %d-%d)", session->inbufoffset, inbufsize, session->inbufoffset);
			
			int ret = gnutls_record_recv(session->sess, session->inbuf + session->inbufoffset, inbufsize - session->inbufoffset);

			if (ret == 0)
			{
				// Client closed connection.
				ServerInstance->Log(DEBUG, "m_ssl_gnutls.so: Client closed the connection");
				readresult = 0;
				CloseSession(session);
				return 1;
			}
			else if (ret < 0)
			{
				if (ret == GNUTLS_E_AGAIN || ret == GNUTLS_E_INTERRUPTED)
				{
					ServerInstance->Log(DEBUG, "m_ssl_gnutls.so: OnRawSocketRead: Not all SSL data read: %s", gnutls_strerror(ret));
					return -1;
				}
				else
				{
					ServerInstance->Log(DEBUG, "m_ssl_gnutls.so: OnRawSocketRead: Error reading SSL data: %s", gnutls_strerror(ret));
					readresult = 0;
					CloseSession(session);
				}
			}
			else
			{
				// Read successfully 'ret' bytes into inbuf + inbufoffset
				// There are 'ret' + 'inbufoffset' bytes of data in 'inbuf'
				// 'buffer' is 'count' long
				
				unsigned int length = ret + session->inbufoffset;
						
				if(count <= length)
				{
					memcpy(buffer, session->inbuf, count);
					// Move the stuff left in inbuf to the beginning of it
					memcpy(session->inbuf, session->inbuf + count, (length - count));
					// Now we need to set session->inbufoffset to the amount of data still waiting to be handed to insp.
					session->inbufoffset = length - count;
					// Insp uses readresult as the count of how much data there is in buffer, so:
					readresult = count;
				}
				else
				{
					// There's not as much in the inbuf as there is space in the buffer, so just copy the whole thing.
					memcpy(buffer, session->inbuf, length);
					// Zero the offset, as there's nothing there..
					session->inbufoffset = 0;
					// As above
					readresult = length;
				}
			}
		}
		else if(session->status == ISSL_CLOSING)
		{
			ServerInstance->Log(DEBUG, "m_ssl_gnutls.so: OnRawSocketRead: session closing...");
			readresult = 0;
		}
		
		return 1;
	}
	
	virtual int OnRawSocketWrite(int fd, const char* buffer, int count)
	{
		if (!count)
			return 0;

		issl_session* session = &sessions[fd];
		const char* sendbuffer = buffer;

		if(!session->sess)
		{
			ServerInstance->Log(DEBUG, "m_ssl_gnutls.so: OnRawSocketWrite: No session to write to");
			CloseSession(session);
			return 1;
		}
		
		if(session->status == ISSL_HANDSHAKING_WRITE)
		{
			// The handshake isn't finished, try to finish it.
			
			if(Handshake(session))
			{
				// Handshake successfully resumed.
				ServerInstance->Log(DEBUG, "m_ssl_gnutls.so: OnRawSocketWrite: successfully resumed handshake");
			}
			else
			{
				// Couldn't resume handshake.	
				ServerInstance->Log(DEBUG, "m_ssl_gnutls.so: OnRawSocketWrite: failed to resume handshake"); 
			}
		}
		else if(session->status == ISSL_HANDSHAKING_READ)
		{
			ServerInstance->Log(DEBUG, "m_ssl_gnutls.so: OnRawSocketWrite: handshake wants to read data but we are currently writing");
		}

		session->outbuf.append(sendbuffer, count);
		sendbuffer = session->outbuf.c_str();
		count = session->outbuf.size();

		if(session->status == ISSL_HANDSHAKEN)
		{	
			int ret = gnutls_record_send(session->sess, sendbuffer, count);
		
			if(ret == 0)
			{
				ServerInstance->Log(DEBUG, "m_ssl_gnutls.so: OnRawSocketWrite: Client closed the connection");
				CloseSession(session);
			}
			else if(ret < 0)
			{
				if(ret == GNUTLS_E_AGAIN || ret == GNUTLS_E_INTERRUPTED)
				{
					ServerInstance->Log(DEBUG, "m_ssl_gnutls.so: OnRawSocketWrite: Not all SSL data written: %s", gnutls_strerror(ret));
				}
				else
				{
					ServerInstance->Log(DEBUG, "m_ssl_gnutls.so: OnRawSocketWrite: Error writing SSL data: %s", gnutls_strerror(ret));
					CloseSession(session);					
				}
			}
			else
			{
				session->outbuf = session->outbuf.substr(ret);
			}
		}
		else if(session->status == ISSL_CLOSING)
		{
			ServerInstance->Log(DEBUG, "m_ssl_gnutls.so: OnRawSocketWrite: session closing...");
		}
		
		return 1;
	}
	
	// :kenny.chatspike.net 320 Om Epy|AFK :is a Secure Connection
	virtual void OnWhois(userrec* source, userrec* dest)
	{
		// Bugfix, only send this numeric for *our* SSL users
		if(dest->GetExt("ssl", dummy) || (IS_LOCAL(dest) &&  isin(dest->GetPort(), listenports)))
		{
			ServerInstance->SendWhoisLine(source, dest, 320, "%s %s :is using a secure connection", source->nick, dest->nick);
		}
	}
	
	virtual void OnSyncUserMetaData(userrec* user, Module* proto, void* opaque, const std::string &extname)
	{
		// check if the linking module wants to know about OUR metadata
		if(extname == "ssl")
		{
			// check if this user has an swhois field to send
			if(user->GetExt(extname, dummy))
			{
				// call this function in the linking module, let it format the data how it
				// sees fit, and send it on its way. We dont need or want to know how.
				proto->ProtoSendMetaData(opaque, TYPE_USER, user, extname, "ON");
			}
		}
	}
	
	virtual void OnDecodeMetaData(int target_type, void* target, const std::string &extname, const std::string &extdata)
	{
		// check if its our metadata key, and its associated with a user
		if ((target_type == TYPE_USER) && (extname == "ssl"))
		{
			userrec* dest = (userrec*)target;
			// if they dont already have an ssl flag, accept the remote server's
			if (!dest->GetExt(extname, dummy))
			{
				dest->Extend(extname, "ON");
			}
		}
	}
	
	bool Handshake(issl_session* session)
	{		
		int ret = gnutls_handshake(session->sess);
      
		if(ret < 0)
		{
			if(ret == GNUTLS_E_AGAIN || ret == GNUTLS_E_INTERRUPTED)
			{
				// Handshake needs resuming later, read() or write() would have blocked.
				
				if(gnutls_record_get_direction(session->sess) == 0)
				{
					// gnutls_handshake() wants to read() again.
					session->status = ISSL_HANDSHAKING_READ;
					ServerInstance->Log(DEBUG, "m_ssl_gnutls.so: Handshake needs resuming (reading) later, error string: %s", gnutls_strerror(ret));
				}
				else
				{
					// gnutls_handshake() wants to write() again.
					session->status = ISSL_HANDSHAKING_WRITE;
					ServerInstance->Log(DEBUG, "m_ssl_gnutls.so: Handshake needs resuming (writing) later, error string: %s", gnutls_strerror(ret));
					MakePollWrite(session);	
				}
			}
			else
			{
				// Handshake failed.
				CloseSession(session);
			   ServerInstance->Log(DEBUG, "m_ssl_gnutls.so: Handshake failed, error string: %s", gnutls_strerror(ret));
			   session->status = ISSL_CLOSING;
			}
			
			return false;
		}
		else
		{
			// Handshake complete.
			ServerInstance->Log(DEBUG, "m_ssl_gnutls.so: Handshake completed");
			
			// This will do for setting the ssl flag...it could be done earlier if it's needed. But this seems neater.
			userrec* extendme = ServerInstance->FindDescriptor(session->fd);
			if (extendme)
			{
				if (!extendme->GetExt("ssl", dummy))
					extendme->Extend("ssl", "ON");
			}

			// Change the seesion state
			session->status = ISSL_HANDSHAKEN;
			
			// Finish writing, if any left
			MakePollWrite(session);
			
			return true;
		}
	}

	virtual void OnPostConnect(userrec* user)
	{
		// This occurs AFTER OnUserConnect so we can be sure the
		// protocol module has propogated the NICK message.
		if ((user->GetExt("ssl", dummy)) && (IS_LOCAL(user)))
		{
			// Tell whatever protocol module we're using that we need to inform other servers of this metadata NOW.
			std::deque<std::string>* metadata = new std::deque<std::string>;
			metadata->push_back(user->nick);
			metadata->push_back("ssl");		// The metadata id
			metadata->push_back("ON");		// The value to send
			Event* event = new Event((char*)metadata,(Module*)this,"send_metadata");
			event->Send(ServerInstance);		// Trigger the event. We don't care what module picks it up.
			DELETE(event);
			DELETE(metadata);

			VerifyCertificate(&sessions[user->GetFd()],user);
		}
	}
	
	void MakePollWrite(issl_session* session)
	{
		OnRawSocketWrite(session->fd, NULL, 0);
	}
	
	void CloseSession(issl_session* session)
	{
		if(session->sess)
		{
			gnutls_bye(session->sess, GNUTLS_SHUT_WR);
			gnutls_deinit(session->sess);
		}
		
		if(session->inbuf)
		{
			delete[] session->inbuf;
		}
		
		session->outbuf.clear();
		session->inbuf = NULL;
		session->sess = NULL;
		session->status = ISSL_NONE;
	}

	void VerifyCertificate(issl_session* session, Extensible* user)
	{
		unsigned int status;
		const gnutls_datum_t* cert_list;
		int ret;
		unsigned int cert_list_size;
		gnutls_x509_crt_t cert;
		char name[MAXBUF];
		unsigned char digest[MAXBUF];
		size_t digest_size = sizeof(digest);
		size_t name_size = sizeof(name);
		ssl_cert* certinfo = new ssl_cert;

		user->Extend("ssl_cert",certinfo);

		/* This verification function uses the trusted CAs in the credentials
		 * structure. So you must have installed one or more CA certificates.
		 */
		ret = gnutls_certificate_verify_peers2(session->sess, &status);

		if (ret < 0)
		{
			certinfo->data.insert(std::make_pair("error",std::string(gnutls_strerror(ret))));
			return;
		}

		if (status & GNUTLS_CERT_INVALID)
		{
			certinfo->data.insert(std::make_pair("invalid",ConvToStr(1)));
		}
		else
		{
			certinfo->data.insert(std::make_pair("invalid",ConvToStr(0)));
		}
		if (status & GNUTLS_CERT_SIGNER_NOT_FOUND)
		{
			certinfo->data.insert(std::make_pair("unknownsigner",ConvToStr(1)));
		}
		else
		{
			certinfo->data.insert(std::make_pair("unknownsigner",ConvToStr(0)));
		}
		if (status & GNUTLS_CERT_REVOKED)
		{
			certinfo->data.insert(std::make_pair("revoked",ConvToStr(1)));
		}
		else
		{
			certinfo->data.insert(std::make_pair("revoked",ConvToStr(0)));
		}
		if (status & GNUTLS_CERT_SIGNER_NOT_CA)
		{
			certinfo->data.insert(std::make_pair("trusted",ConvToStr(0)));
		}
		else
		{
			certinfo->data.insert(std::make_pair("trusted",ConvToStr(1)));
		}
	
		/* Up to here the process is the same for X.509 certificates and
		 * OpenPGP keys. From now on X.509 certificates are assumed. This can
		 * be easily extended to work with openpgp keys as well.
		 */
		if (gnutls_certificate_type_get(session->sess) != GNUTLS_CRT_X509)
		{
			certinfo->data.insert(std::make_pair("error","No X509 keys sent"));
			return;
		}

		ret = gnutls_x509_crt_init(&cert);
		if (ret < 0)
		{
			certinfo->data.insert(std::make_pair("error",gnutls_strerror(ret)));
			return;
		}

		cert_list_size = 0;
		cert_list = gnutls_certificate_get_peers(session->sess, &cert_list_size);
		if (cert_list == NULL)
		{
			certinfo->data.insert(std::make_pair("error","No certificate was found"));
			return;
		}

		/* This is not a real world example, since we only check the first 
		 * certificate in the given chain.
		 */

		ret = gnutls_x509_crt_import(cert, &cert_list[0], GNUTLS_X509_FMT_DER);
		if (ret < 0)
		{
			certinfo->data.insert(std::make_pair("error",gnutls_strerror(ret)));
			return;
		}

		gnutls_x509_crt_get_dn(cert, name, &name_size);

		certinfo->data.insert(std::make_pair("dn",name));

		gnutls_x509_crt_get_issuer_dn(cert, name, &name_size);

		certinfo->data.insert(std::make_pair("issuer",name));

		if ((ret = gnutls_x509_crt_get_fingerprint(cert, GNUTLS_DIG_MD5, digest, &digest_size)) < 0)
		{
			certinfo->data.insert(std::make_pair("error",gnutls_strerror(ret)));
		}
		else
		{
			certinfo->data.insert(std::make_pair("fingerprint",irc::hex(digest, digest_size)));
		}

		/* Beware here we do not check for errors.
		 */
		if ((gnutls_x509_crt_get_expiration_time(cert) < time(0)) || (gnutls_x509_crt_get_activation_time(cert) > time(0)))
		{
			certinfo->data.insert(std::make_pair("error","Not activated, or expired certificate"));
		}

		gnutls_x509_crt_deinit(cert);

		return;
	}

};

class ModuleSSLGnuTLSFactory : public ModuleFactory
{
 public:
	ModuleSSLGnuTLSFactory()
	{
	}
	
	~ModuleSSLGnuTLSFactory()
	{
	}
	
	virtual Module * CreateModule(InspIRCd* Me)
	{
		return new ModuleSSLGnuTLS(Me);
	}
};


extern "C" void * init_module( void )
{
	return new ModuleSSLGnuTLSFactory;
}
