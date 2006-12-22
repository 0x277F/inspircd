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
#include <vector>

/* $ModDesc: Provides aliases of commands. */

/** An alias definition
 */
class Alias : public classbase
{
 public:
	/** The text of the alias command */
	irc::string text;
	/** Text to replace with */
	std::string replace_with;
	/** Nickname required to perform alias */
	std::string requires;
	/** Alias requires ulined server */
	bool uline;
	/** Requires oper? */
	bool operonly;
	/** Format that must be matched for use */
	std::string format;
};

class ModuleAlias : public Module
{
 private:
	/** We cant use a map, there may be multiple aliases with the same name */
	std::vector<Alias> Aliases;
	std::vector<std::string> pars;

	virtual void ReadAliases()
	{
		ConfigReader MyConf(ServerInstance);

		Aliases.clear();
		for (int i = 0; i < MyConf.Enumerate("alias"); i++)
		{
			Alias a;
			std::string txt;
			txt = MyConf.ReadValue("alias", "text", i);
			a.text = txt.c_str();
			a.replace_with = MyConf.ReadValue("alias", "replace", i, true);
			a.requires = MyConf.ReadValue("alias", "requires", i);
			a.uline = MyConf.ReadFlag("alias", "uline", i);
			a.operonly = MyConf.ReadFlag("alias", "operonly", i);
			a.format = MyConf.ReadValue("alias", "format", i);
			Aliases.push_back(a);
		}
	}

 public:
	
	ModuleAlias(InspIRCd* Me)
		: Module::Module(Me)
	{
		ReadAliases();
		pars.resize(127);
	}

	void Implements(char* List)
	{
		List[I_OnPreCommand] = List[I_OnRehash] = 1;
	}

	virtual ~ModuleAlias()
	{
	}

	virtual Version GetVersion()
	{
		return Version(1,1,0,1,VF_VENDOR,API_VERSION);
	}

	std::string GetVar(std::string varname, const std::string &original_line)
	{
		irc::spacesepstream ss(original_line);
		varname.erase(varname.begin());
		int index = *(varname.begin()) - 48;
		varname.erase(varname.begin());
		bool everything_after = (varname == "-");
		std::string word = "";

		ServerInstance->Log(DEBUG,"Get var %d%s", index , everything_after ? " and all after it" : "");

		for (int j = 0; j < index; j++)
			word = ss.GetToken();

		if (everything_after)
		{
			std::string more = "*";
			while ((more = ss.GetToken()) != "")
			{
				word.append(" ");
				word.append(more);
			}
		}

		ServerInstance->Log(DEBUG,"Var is '%s'", word.c_str());

		return word;
	}

	void SearchAndReplace(std::string& newline, const std::string &find, const std::string &replace)
	{
		std::string::size_type x = newline.find(find);
		while (x != std::string::npos)
		{
			newline.erase(x, find.length());
			newline.insert(x, replace);
			x = newline.find(find);
		}
	}

	virtual int OnPreCommand(const std::string &command, const char** parameters, int pcnt, userrec *user, bool validated, const std::string &original_line)
	{
		userrec *u = NULL;

		/* If the command is valid, we dont want to know,
		 * and if theyre not registered yet, we dont want
		 * to know either
		 */
		if ((validated) || (user->registered != REG_ALL))
			return 0;

		irc::string c = command.c_str();
		/* The parameters for the command in their original form, with the command stripped off */
		std::string compare = original_line.substr(command.length());
		while (*(compare.c_str()) == ' ')
			compare.erase(compare.begin());

		std::string safe(original_line);

		/* Escape out any $ symbols in the user provided text */

		SearchAndReplace(safe, "$", "\r");

		for (unsigned int i = 0; i < Aliases.size(); i++)
		{
			if (Aliases[i].text == c)
			{
				/* Does it match the pattern? */
				if ((!Aliases[i].format.empty()) && (!ServerInstance->MatchText(compare, Aliases[i].format)))
				{
					continue;
				}

				if ((Aliases[i].operonly) && (!*user->oper))
					return 0;

				if (Aliases[i].requires != "")
				{
					u = ServerInstance->FindNick(Aliases[i].requires);
					if (!u)
					{
						user->WriteServ("401 "+std::string(user->nick)+" "+Aliases[i].requires+" :is currently unavailable. Please try again later.");
						return 1;
					}
				}
				if ((u != NULL) && (Aliases[i].requires != "") && (Aliases[i].uline))
				{
					if (!ServerInstance->ULine(u->server))
					{
						ServerInstance->WriteOpers("*** NOTICE -- Service "+Aliases[i].requires+" required by alias "+std::string(Aliases[i].text.c_str())+" is not on a u-lined server, possibly underhanded antics detected!"); 
						user->WriteServ("401 "+std::string(user->nick)+" "+Aliases[i].requires+" :is an imposter! Please inform an IRC operator as soon as possible.");
						return 1;
					}
				}

				/* Now, search and replace in a copy of the original_line, replacing $1 through $9 and $1- etc */

				std::string::size_type crlf = Aliases[i].replace_with.find('\n');

				if (crlf == std::string::npos)
				{
					ServerInstance->Log(DEBUG,"Single line alias: '%s'", Aliases[i].replace_with.c_str());
					DoCommand(Aliases[i].replace_with, user, safe);
					return 1;
				}
				else
				{
					ServerInstance->Log(DEBUG,"Multi line alias: '%s'", Aliases[i].replace_with.c_str());
					irc::sepstream commands(Aliases[i].replace_with, '\n');
					std::string command = "*";
					while ((command = commands.GetToken()) != "")
					{
						ServerInstance->Log(DEBUG,"Execute: '%s'", command.c_str());
						DoCommand(command, user, safe);
					}
					return 1;
				}
			}
		}
		return 0;
	}

	void DoCommand(std::string newline, userrec* user, const std::string &original_line)
	{
		for (int v = 1; v < 10; v++)
		{
			std::string var = "$";
			var.append(ConvToStr(v));
			var.append("-");
			std::string::size_type x = newline.find(var);

			while (x != std::string::npos)
			{
				newline.erase(x, var.length());
				newline.insert(x, GetVar(var, original_line));
				x = newline.find(var);
			}

			var = "$";
			var.append(ConvToStr(v));
			x = newline.find(var);

			while (x != std::string::npos)
			{
				newline.erase(x, var.length());
				newline.insert(x, GetVar(var, original_line));
				x = newline.find(var);
			}
		}

		/* Special variables */
		SearchAndReplace(newline, "$nick", user->nick);
		SearchAndReplace(newline, "$ident", user->ident);
		SearchAndReplace(newline, "$host", user->host);
		SearchAndReplace(newline, "$vhost", user->dhost);

		/* Unescape any variable names in the user text before sending */
		SearchAndReplace(newline, "\r", "$");

		irc::tokenstream ss(newline);
		const char* parv[127];
		int x = 0;

		while ((pars[x] = ss.GetToken()) != "")
		{
			parv[x] = pars[x].c_str();
			ServerInstance->Log(DEBUG,"Parameter %d: %s", x, parv[x]);
			x++;
		}

		ServerInstance->Log(DEBUG,"Call command handler on %s", parv[0]);

		if (ServerInstance->Parser->CallHandler(parv[0], &parv[1], x-1, user) == CMD_INVALID)
		{
			ServerInstance->Log(DEBUG,"Unknown command or not enough parameters");
		}
		else
		{
			ServerInstance->Log(DEBUG,"Command handler called successfully.");
		}
	}
 
	virtual void OnRehash(const std::string &parameter)
	{
		ReadAliases();
 	}
};


class ModuleAliasFactory : public ModuleFactory
{
 public:
	ModuleAliasFactory()
	{
	}

	~ModuleAliasFactory()
	{
	}

		virtual Module * CreateModule(InspIRCd* Me)
	{
		return new ModuleAlias(Me);
	}
};


extern "C" void * init_module( void )
{
	return new ModuleAliasFactory;
}

