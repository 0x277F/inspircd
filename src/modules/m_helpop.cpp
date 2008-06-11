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

/* $ModDesc: /helpop Command, Works like Unreal helpop */
static std::map<irc::string, std::string> helpop_map;


/** Handles user mode +h
 */
class Helpop : public SimpleUserModeHandler
{
 public:
	Helpop(InspIRCd* Instance) : SimpleUserModeHandler(Instance, 'h') { }
};

/** Handles /HELPOP
 */
class CommandHelpop : public Command
{
 public:
	CommandHelpop (InspIRCd* Instance) : Command(Instance, "HELPOP", 0, 0)
	{
		this->source = "m_helpop.so";
		syntax = "<any-text>";
	}

	CmdResult Handle (const std::vector<std::string> &parameters, User *user)
	{
		irc::string parameter("start");
		if (parameters.size() > 0)
			parameter = parameters[0].c_str();

		if (parameter == "index")
		{
			/* iterate over all helpop items */
			user->WriteServ("NOTICE %s :HELPOP topic index", user->nick.c_str());
			for (std::map<irc::string, std::string>::iterator iter = helpop_map.begin(); iter != helpop_map.end(); iter++)
			{
				user->WriteServ("NOTICE %s :    %s", user->nick.c_str(), iter->first.c_str());
			}
			user->WriteServ("NOTICE %s :*** End of HELPOP topic index", user->nick.c_str());
		}
		else
		{
			user->WriteServ("NOTICE %s :*** HELPOP for %s", user->nick.c_str(), parameter.c_str());

			std::map<irc::string, std::string>::iterator iter = helpop_map.find(parameter);

			if (iter == helpop_map.end())
			{
				iter = helpop_map.find("nohelp");
			}

			std::string value = iter->second;
			irc::sepstream stream(value, '\n');
			std::string token = "*";

			while (stream.GetToken(token))
				user->WriteServ("NOTICE %s :%s", user->nick.c_str(), token.c_str());

			user->WriteServ("NOTICE %s :*** End of HELPOP", user->nick.c_str());
		}

		/* We dont want these going out over the network, return CMD_FAILURE
		 * to make sure the protocol module thinks theyre not worth sending.
		 */
		return CMD_FAILURE;
	}
};

class ModuleHelpop : public Module
{
	private:
		std::string  h_file;
		CommandHelpop* mycommand;
		Helpop* ho;

	public:
		ModuleHelpop(InspIRCd* Me)
			: Module(Me)
		{
			ReadConfig();
			ho = new Helpop(ServerInstance);
			if (!ServerInstance->Modes->AddMode(ho))
				throw ModuleException("Could not add new modes!");
			mycommand = new CommandHelpop(ServerInstance);
			ServerInstance->AddCommand(mycommand);
		Implementation eventlist[] = { I_OnRehash, I_OnWhois };
		ServerInstance->Modules->Attach(eventlist, this, 2);
		}

		virtual void ReadConfig()
		{
			ConfigReader *MyConf = new ConfigReader(ServerInstance);

			helpop_map.clear();

			for (int i = 0; i < MyConf->Enumerate("helpop"); i++)
			{
				irc::string key = assign(MyConf->ReadValue("helpop", "key", i));
				std::string value = MyConf->ReadValue("helpop", "value", i, true); /* Linefeeds allowed! */

				if (key == "index")
				{
					throw ModuleException("m_helpop: The key 'index' is reserved for internal purposes. Please remove it.");
				}

				helpop_map[key] = value;
			}

			if (helpop_map.find("start") == helpop_map.end())
			{
				// error!
				throw ModuleException("m_helpop: Helpop file is missing important entries. Please check the example conf.");
			}
			else if (helpop_map.find("nohelp") == helpop_map.end())
			{
				// error!
				throw ModuleException("m_helpop: Helpop file is missing important entries. Please check the example conf.");
			}

		}


		virtual void OnRehash(User* user, const std::string &parameter)
		{
			ReadConfig();
		}

		virtual void OnWhois(User* src, User* dst)
		{
			if (dst->IsModeSet('h'))
			{
				ServerInstance->SendWhoisLine(src, dst, 310, std::string(src->nick)+" "+std::string(dst->nick)+" :is available for help.");
			}
		}

		virtual ~ModuleHelpop()
		{
			ServerInstance->Modes->DelMode(ho);
			delete ho;
		}

		virtual Version GetVersion()
		{
			return Version(1,2,0,1,VF_COMMON|VF_VENDOR,API_VERSION);
		}
};

MODULE_INIT(ModuleHelpop)
