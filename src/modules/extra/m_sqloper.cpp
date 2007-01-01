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
#include "users.h"
#include "channels.h"
#include "modules.h"
#include "inspircd.h"
#include "configreader.h"

#include "m_sqlv2.h"
#include "m_sqlutils.h"
#include "commands/cmd_oper.h"

/* $ModDesc: Allows storage of oper credentials in an SQL table */
/* $ModDep: m_sqlv2.h m_sqlutils.h */

class ModuleSQLOper : public Module
{
	InspIRCd* Srv;
	Module* SQLutils;
	std::string databaseid;

public:
	ModuleSQLOper(InspIRCd* Me)
	: Module::Module(Me), Srv(Me)
	{
		ServerInstance->UseInterface("SQLutils");
		ServerInstance->UseInterface("SQL");

		SQLutils = ServerInstance->FindModule("m_sqlutils.so");
		if (!SQLutils)
			throw ModuleException("Can't find m_sqlutils.so. Please load m_sqlutils.so before m_sqloper.so.");

		OnRehash(NULL,"");
	}

	virtual ~ModuleSQLOper()
	{
		ServerInstance->DoneWithInterface("SQL");
		ServerInstance->DoneWithInterface("SQLutils");
	}

	void Implements(char* List)
	{
		List[I_OnRequest] = List[I_OnRehash] = List[I_OnPreCommand] = 1;
	}

	virtual void OnRehash(userrec* user, const std::string &parameter)
	{
		ConfigReader Conf(Srv);
		
		databaseid = Conf.ReadValue("sqloper", "dbid", 0); /* Database ID of a database configured for the service provider module */
	}

	virtual int OnPreCommand(const std::string &command, const char** parameters, int pcnt, userrec *user, bool validated, const std::string &original_line)
	{
		if ((validated) && (command == "OPER"))
		{
			if (LookupOper(user, parameters[0], parameters[1]))
			{	
				/* Returning true here just means the query is in progress, or on it's way to being
				 * in progress. Nothing about the /oper actually being successful..
				 * If the oper lookup fails later, we pass the command to the original handler
				 * for /oper by calling its Handle method directly.
				 */
				return 1;
			}
		}
		return 0;
	}

	bool LookupOper(userrec* user, const std::string &username, const std::string &password)
	{
		Module* target;
		
		target = Srv->FindFeature("SQL");

		if (target)
		{
			SQLrequest req = SQLreq(this, target, databaseid, "SELECT username, password, hostname, type FROM ircd_opers WHERE username = '?' AND password=md5('?')", username, password);
			
			if (req.Send())
			{
				/* When we get the query response from the service provider we will be given an ID to play with,
				 * just an ID number which is unique to this query. We need a way of associating that ID with a userrec
				 * so we insert it into a map mapping the IDs to users.
				 * Thankfully m_sqlutils provides this, it will associate a ID with a user or channel, and if the user quits it removes the
				 * association. This means that if the user quits during a query we will just get a failed lookup from m_sqlutils - telling
				 * us to discard the query.
			 	 */
				ServerInstance->Log(DEBUG, "Sent query, got given ID %lu", req.id);
				
				AssociateUser(this, SQLutils, req.id, user).Send();

				user->Extend("oper_user", strdup(username.c_str()));
				user->Extend("oper_pass", strdup(password.c_str()));
					
				return true;
			}
			else
			{
				ServerInstance->Log(DEBUG, "SQLrequest failed: %s", req.error.Str());
			
				return false;
			}
		}
		else
		{
			ServerInstance->Log(SPARSE, "WARNING: Couldn't find SQL provider module. NOBODY will be able to oper up unless their o:line is statically configured");
			return false;
		}
	}
	
	virtual char* OnRequest(Request* request)
	{
		if (strcmp(SQLRESID, request->GetId()) == 0)
		{
			SQLresult* res;
		
			res = static_cast<SQLresult*>(request);
			
			ServerInstance->Log(DEBUG, "Got SQL result (%s) with ID %lu", res->GetId(), res->id);
			
			userrec* user = GetAssocUser(this, SQLutils, res->id).S().user;
			UnAssociate(this, SQLutils, res->id).S();

			char* tried_user = NULL;
			char* tried_pass = NULL;

			user->GetExt("oper_user", tried_user);
			user->GetExt("oper_pass", tried_pass);
			
			if (user)
			{
				if (res->error.Id() == NO_ERROR)
				{				
					ServerInstance->Log(DEBUG, "Associated query ID %lu with user %s", res->id, user->nick);			
					ServerInstance->Log(DEBUG, "Got result with %d rows and %d columns", res->Rows(), res->Cols());
			
					if (res->Rows())
					{
						/* We got a row in the result, this means there was a record for the oper..
						 * now we just need to check if their host matches, and if it does then
						 * oper them up.
						 * 
						 * We now (previous versions of the module didn't) support multiple SQL
						 * rows per-oper in the same way the config file does, all rows will be tried
						 * until one is found which matches. This is useful to define several different
						 * hosts for a single oper.
						 * 
						 * The for() loop works as SQLresult::GetRowMap() returns an empty map when there
						 * are no more rows to return.
						 */
						
						for (SQLfieldMap& row = res->GetRowMap(); row.size(); row = res->GetRowMap())
						{
							ServerInstance->Log(DEBUG, "Trying to oper user %s with username = '%s', passhash = '%s', hostname = '%s', type = '%s'", user->nick, row["username"].d.c_str(), row["password"].d.c_str(), row["hostname"].d.c_str(), row["type"].d.c_str());
							
							if (OperUser(user, row["username"].d, row["password"].d, row["hostname"].d, row["type"].d))
							{
								/* If/when one of the rows matches, stop checking and return */
								return SQLSUCCESS;
							}
						}
					}
					else
					{
						/* No rows in result, this means there was no oper line for the user,
						 * we should have already checked the o:lines so now we need an
						 * "insufficient awesomeness" (invalid credentials) error
						 */
						if (tried_user && tried_pass)
						{
							LoginFail(user, tried_user, tried_pass);
							free(tried_user);
							free(tried_pass);
							user->Shrink("oper_user");
							user->Shrink("oper_pass");
						}
					}
				}
				else
				{
					/* This one shouldn't happen, the query failed for some reason.
					 * We have to fail the /oper request and give them the same error
					 * as above.
					 */
					ServerInstance->Log(DEBUG, "Query failed: %s", res->error.Str());

					if (tried_user && tried_pass)
					{
						LoginFail(user, tried_user, tried_pass);
						free(tried_user);
						free(tried_pass);
						user->Shrink("oper_user");
						user->Shrink("oper_pass");
					}

				}
			}
			else
			{
				ServerInstance->Log(DEBUG, "Got query with unknown ID, this probably means the user quit while the query was in progress");
			}
		
			return SQLSUCCESS;
		}
		
		ServerInstance->Log(DEBUG, "Got unsupported API version string: %s", request->GetId());

		return NULL;
	}

	void LoginFail(userrec* user, const std::string &username, const std::string &pass)
	{
		command_t* oper_command = ServerInstance->Parser->GetHandler("OPER");

		if (oper_command)
		{
			const char* params[] = { username.c_str(), pass.c_str() };
			oper_command->Handle(params, 2, user);
		}
		else
		{
			ServerInstance->Log(DEBUG, "WHAT?! Why do we have no OPER command?!");
		}
	}

	bool OperUser(userrec* user, const std::string &username, const std::string &password, const std::string &pattern, const std::string &type)
	{
		ConfigReader Conf(Srv);
		
		for (int j = 0; j < Conf.Enumerate("type"); j++)
		{
			std::string tname = Conf.ReadValue("type","name",j);
			
			ServerInstance->Log(DEBUG, "Scanning opertype: %s", tname.c_str());
			
			std::string hostname(user->ident);
			hostname.append("@").append(user->host);
							
			if ((tname == type) && OneOfMatches(hostname.c_str(), user->GetIPString(), pattern.c_str()))
			{
				/* Opertype and host match, looks like this is it. */
				ServerInstance->Log(DEBUG, "Host (%s matched %s OR %s) and type (%s)", pattern.c_str(), hostname.c_str(), user->GetIPString(), type.c_str());
				
				std::string operhost = Conf.ReadValue("type", "host", j);
							
				if (operhost.size())
					user->ChangeDisplayedHost(operhost.c_str());
								
				Srv->SNO->WriteToSnoMask('o',"%s (%s@%s) is now an IRC operator of type %s", user->nick, user->ident, user->host, type.c_str());
				user->WriteServ("381 %s :You are now an IRC operator of type %s", user->nick, type.c_str());
				
				if (!user->modes[UM_OPERATOR])
					user->Oper(type);
								
				return true;
			}
		}
		
		return false;
	}

	virtual Version GetVersion()
	{
		return Version(1,1,1,0,VF_VENDOR,API_VERSION);
	}
	
};

class ModuleSQLOperFactory : public ModuleFactory
{
 public:
	ModuleSQLOperFactory()
	{
	}
	
	~ModuleSQLOperFactory()
	{
	}
	
	virtual Module * CreateModule(InspIRCd* Me)
	{
		return new ModuleSQLOper(Me);
	}
	
};


extern "C" void * init_module( void )
{
	return new ModuleSQLOperFactory;
}

