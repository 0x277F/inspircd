/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  InspIRCd: (C) 2002-2009 InspIRCd Development Team
 * See: http://www.inspircd.org/wiki/index.php/Credits
 *
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

/* Contains a code of Unreal IRCd + Bynets patch ( http://www.unrealircd.com/ and http://www.bynets.org/ )
   Changed at 2008-06-15 - 2008-12-15
   by Chernov-Phoenix Alexey (Phoenix@RusNet) mailto:phoenix /email address separator/ pravmail.ru */

#include "inspircd.h"
#include "caller.h"
#include <fstream>

/* $ModDesc: Provides an ability to have non-RFC1459 nicks & support for national CASEMAPPING */

DEFINE_HANDLER2(lwbNickHandler, bool, const char*, size_t);

								 /*,m_reverse_additionalUp[256];*/
static unsigned char m_reverse_additional[256],m_additionalMB[256],m_additionalUtf8[256],m_additionalUtf8range[256];

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


char utf8checkrest(unsigned char * mb, unsigned char cnt)
{
	for (unsigned char * tmp=mb; tmp<mb+cnt; tmp++)
	{
		if ((*tmp<128)||(*tmp>191))
			return -1;
	}
	return cnt+1;
}


char utf8size(unsigned char * mb)
{
	if (!*mb)
		return -1;
	if (!(*mb & 128))
		return 1;
	if ((*mb & 224)==192)
		return utf8checkrest(mb+1,1);
	if ((*mb & 240)==224)
		return utf8checkrest(mb+1,2);
	if ((*mb & 248)==240)
		return utf8checkrest(mb+1,3);
	return -1;
}


/* Conditions added */
bool lwbNickHandler::Call(const char* n, size_t max)
{
	if (!n || !*n)
		return false;

	unsigned int p = 0;
	for (const char* i = n; *i; i++, p++)
	{
		/* 1. Multibyte encodings support:  */
		/* 1.1. 16bit char. areas, e.g. chinese:*/

		/* if current character is the last, we DO NOT check it against multibyte table */
								 /* if there are mbtable ranges, use ONLY them. No 8bit at all */
		if (i[1] && m_additionalMB[0])
		{
			/* otherwise let's take a look at the current character and the following one */
			bool found=false;
			for(unsigned char * mb=m_additionalMB; (*mb) && (mb<m_additionalMB+sizeof(m_additionalMB)); mb+=4)
			{
				if ( (i[0]>=mb[0]) && (i[0]<=mb[1]) && (i[1]>=mb[2]) && (i[1]<=mb[3]) )
				{
					/* multibyte range character found */
					i++;p++;
					found=true;
					break;
				}
			}
			if (found)
				/* next char! */
				continue;
			else
				/* there are ranges, but incorrect char (8bit?) given, sorry */
				return false;
		}

		/* 2. 8bit character support */

		if ( ((*i >= 'A') && (*i <= '}'))
			|| m_reverse_additional[(unsigned char)*i])
		{
			/* "A"-"}" can occur anywhere in a nickname */
			continue;
		}

		if ((((*i >= '0') && (*i <= '9')) || (*i == '-')) && (i > n))
		{
			/* "0"-"9", "-" can occur anywhere BUT the first char of a nickname */
			continue;
		}

		/* 3.1. Check against a simple UTF-8 characters enumeration */
		int cursize,ncursize;	 /*size of a current character*/
		ncursize=utf8size((unsigned char *)i);
		/* do check only if current multibyte character is valid UTF-8 only */
		if (ncursize!=-1)
		{
			bool found=false;
			for(unsigned char * mb=m_additionalUtf8;
				(utf8size(mb)!=-1) && (mb<m_additionalUtf8+sizeof(m_additionalUtf8));
				mb+=cursize)
			{
				cursize=utf8size(mb);
				/* Size differs? Pick the next! */
				if (cursize!=ncursize)
					continue;
				if (!strncmp(i,(char *)mb,cursize))
				{
					i+=cursize-1;
					p+=cursize-1;
					found=true;
					break;
				}
			}
			if (found)
				continue;
			/* 3.2. Check against an UTF-8 ranges: <start character> and <lenght of the range>.
			Also char. is to be checked if it is a valid UTF-8 one */

			found=false;
			for(unsigned char * mb=m_additionalUtf8range;
				(utf8size(mb)!=-1) && (mb<m_additionalUtf8range+sizeof(m_additionalUtf8range));
				mb+=cursize+1)
			{
				cursize=utf8size(mb);
				/* Size differs? Pick the next! */
				if ((cursize!=ncursize)||(!mb[cursize]))
					continue;

				unsigned char uright[5]={0,0,0,0,0};
				strncpy((char *)uright,(char *)mb, cursize);

				if((uright[cursize-1]+mb[cursize]-1>0xff) && (cursize!=1))
				{
					uright[cursize-2]+=1;
				}
				uright[cursize-1]=(uright[cursize-1]+mb[cursize]-1) % 0x100;

				if ((strncmp(i,(char *)mb,cursize)>=0) && (strncmp(i,(char *)uright,cursize)<=0))
				{
					i+=cursize-1;
					p+=cursize-1;
					found=true;
					break;
				}
			}
			if (found)
				continue;
		}

		/* invalid character! abort */
		return false;
	}

	/* too long? or not -- pointer arithmetic rocks */
	return (p < max);
}


class ModuleNationalChars : public Module
{
	private:
		InspIRCd* ServerInstance;
		lwbNickHandler * myhandler;
		std::string charset,casemapping;
		unsigned char  m_additional[256],m_additionalUp[256],m_lower[256], m_upper[256];
		caller2<bool, const char*, size_t> * rememberer;
		bool forcequit;
		const unsigned char * lowermap_rememberer;
	public:
		ModuleNationalChars(InspIRCd* Me)
			: Module(Me)
		{
			rememberer=(caller2<bool, const char*, size_t> *) malloc(sizeof(rememberer));
			lowermap_rememberer=national_case_insensitive_map;
			memcpy(m_lower,rfc_case_insensitive_map,256);
			national_case_insensitive_map=m_lower;

			ServerInstance=Me;
			*rememberer=ServerInstance->IsNick;
			myhandler=new lwbNickHandler(ServerInstance);
			ServerInstance->IsNick=myhandler;
			Implementation eventlist[] = { I_OnRehash, I_On005Numeric };
			ServerInstance->Modules->Attach(eventlist, this, 2);
			OnRehash(NULL, "");
		}

		virtual void On005Numeric(std::string &output)
		{
			std::string tmp(casemapping);
			tmp.insert(0,"CASEMAPPING=");
			SearchAndReplace(output,"CASEMAPPING=rfc1459",tmp);
		}

		virtual void OnRehash(User* user, const std::string &parameter)
		{
			ConfigReader* conf = new ConfigReader(ServerInstance);
			charset = conf->ReadValue("nationalchars", "file", 0);
			casemapping = conf->ReadValue("nationalchars", "casemapping", charset, 0, false);
			charset.insert(0,"../locales/");
			unsigned char * tables[7]=
			{
				m_additional,m_additionalMB,m_additionalUp,m_lower,m_upper,
				m_additionalUtf8,m_additionalUtf8range
			};
			loadtables(charset,tables,7,5);
			forcequit = conf->ReadFlag("nationalchars", "forcequit", 0);
			CheckForceQuit("National character set changed");
			delete conf;
		}

		void CheckForceQuit(const char * message)
		{
			if (!forcequit)
				return;

			std::vector<User*> purge;
			std::vector<User*>::iterator iter;
			purge.clear();
			for (iter=ServerInstance->Users->local_users.begin();iter!=ServerInstance->Users->local_users.end();++iter)
			{
				if (!ServerInstance->IsNick((*iter)->nick.c_str(), ServerInstance->Config->Limits.NickMax))
					purge.push_back(*iter);
			}
			for (iter=purge.begin();iter!=purge.end();++iter)
			{
				ServerInstance->Users->QuitUser((*iter), message);
			}
		}
		virtual ~ModuleNationalChars()
		{
			delete myhandler;
			ServerInstance->IsNick= *rememberer;
			free(rememberer);
			national_case_insensitive_map=lowermap_rememberer;
			CheckForceQuit("National characters module unloaded");
		}

		virtual Version GetVersion()
		{
			return Version("$Id: m_nationalchars.cpp 0 2008-12-15 14:24:12SAMT phoenix $",VF_COMMON,API_VERSION);
		}

		/*make an array to check against it 8bit characters a bit faster. Whether allowed or uppercase (for your needs).*/

		void makereverse(unsigned char * from, unsigned  char * to, unsigned int cnt)
		{
			memset(to, 0, cnt);
			for(unsigned char * n=from; (*n) && ((*n)<cnt) && (n<from+cnt); n++)
			{
				to[*n]=1;
			}
		}

		/*so Bynets Unreal distribution stuff*/
		void loadtables(std::string filename, unsigned char ** tables, unsigned char cnt, char faillimit)
		{
			std::ifstream ifs(filename.c_str());
			if (ifs.fail())
			{
				ServerInstance->Logs->Log("m_nationalchars",DEFAULT,"loadtables() called for missing file: %s", filename.c_str());
				return;
			}

			unsigned char n;
			for (n=0;n<cnt;n++)
			{
				memset(tables[n], 0, 256);
			}

			memcpy(m_lower,rfc_case_insensitive_map,256);

			for (n=0;n<cnt;n++)
			{
				if (loadtable(ifs, tables[n], 255) && (n<faillimit))
				{
					ServerInstance->Logs->Log("m_nationalchars",DEFAULT,"loadtables() called for illegal file: %s (line %d)", filename.c_str(), n+1);
					return;
				}
			}
			/*			ServerInstance->Logs->Log("m_nationalchars",DEFAULT,"loadtables() : %s", ((char *)national_case_insensitive_map)+1);*/

			makereverse(m_additional, m_reverse_additional, sizeof(m_additional));
			/*	Do you need faster access to additional 8bit uppercase table? No? Oh, sorry :( Let's comment this out */
			/*	makereverse(m_additionalUp, m_reverse_additionalUp, sizeof(m_additional)); */
		}

		unsigned char symtoi(const char *t,unsigned char base)
		/* base = 16 for hexadecimal, 10 for decimal, 8 for octal ;) */
		{
			unsigned char tmp=0,current;
			while ((*t)&&(*t!=' ')&&(*t!=13)&&(*t!=10)&&(*t!=','))
			{
				tmp*=base;
				current=ascii_case_insensitive_map[(unsigned char)*t];
				if (current>='a')
					current=current-'a'+10;
				else
					current=current-'0';
				tmp+=current;
				t++;
			}
			return tmp;
		}

		int loadtable(std::ifstream &ifs , unsigned char *chartable, unsigned int maxindex)
		{
			std::string buf;
			getline(ifs, buf);

			unsigned int i=0;
			int fail=0;

			buf.erase(buf.find_last_not_of("\n")+1);

			if (buf[0]=='.')	 /* simple plain-text string after dot */
			{
				i=buf.size()-1;
				if (i>(maxindex+1)) i=maxindex+1;
				memcpy(chartable,buf.c_str()+1,i);
			}
			else
			{
				const char * p=buf.c_str();
				while (*p)
				{
					if (i>maxindex)
					{
						fail=1;
						break;
					}

					if (*p!='\'')/* decimal or hexadecimal char code */
					{
						if (*p=='0')
						{
							if (p[1]=='x')
								 /* hex with the leading "0x" */
								chartable[i] = symtoi(p+2,16);
							else
								chartable[i] = symtoi(p+1,8);
						}
								 /* hex form */
						else if (*p=='x')
						{
							chartable[i] = symtoi(p+1,16);
						}else	 /* decimal form */
						{
							chartable[i] = symtoi(p,10);
						}
					} else		 /* plain-text char between '' */
					{
						if (*(p+1)=='\\')
						{
							chartable[i] = *(p+2);
							p+=3;
						}else
						{
							chartable[i] = *(p+1);
							p+=2;
						}
					}

					while (*p&& (*p!=',')&&(*p!=' ')&&(*p!=13)&&(*p!=10) ) p++;
					while (*p&&((*p==',')||(*p==' ')||(*p==13)||(*p==10))) p++;

					i++;

				}
			}

			return fail;
		}

};

MODULE_INIT(ModuleNationalChars)
