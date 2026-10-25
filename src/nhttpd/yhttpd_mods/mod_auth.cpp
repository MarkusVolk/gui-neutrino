//=============================================================================
// YHTTPD
// mod_auth : Authentication
//=============================================================================

#include <cstring>
#include <cstdlib>
#include "mod_auth.h"
#include <helper.h>

//-----------------------------------------------------------------------------
// HOOK: webserver_readconfig_hook Handler
// This hook ist called from ReadConfig
//-----------------------------------------------------------------------------
THandleStatus CmAuth::Hook_ReadConfig(CConfigFile *Config,
		CStringList &ConfigList) {
	ConfigList["mod_auth.username"] = Config->getString("mod_auth.username", AUTHUSER);
	ConfigList["mod_auth.password"] = Config->getString("mod_auth.password", AUTHPASSWORD);
	ConfigList["mod_auth.no_auth_client"] = Config->getString("mod_auth.no_auth_client", "");
	ConfigList["mod_auth.authenticate"] = Config->getBool("mod_auth.authenticate", false) ? "true" : "false";
	return HANDLED_CONTINUE;
}

//-----------------------------------------------------------------------------
// decode Base64 buffer to String
//-----------------------------------------------------------------------------
std::string CmAuth::decodeBase64(const char *b64buffer) {
	if(b64buffer==NULL)
		return "";
	char *newString; //shorter then b64buffer
	std::string result;
	if ((newString = (char *) malloc(sizeof(char) * strlen(b64buffer) + 1))
			!= NULL) {
		char *org_newString = newString;
		int i = 0;
		unsigned long c = 0;

		while (*b64buffer) {
			int oneChar = *b64buffer++;
			if (oneChar >= '0' && oneChar <= '9')
				oneChar = oneChar - '0' + 52;
			else if (oneChar >= 'A' && oneChar <= 'Z')
				oneChar = oneChar - 'A';
			else if (oneChar >= 'a' && oneChar <= 'z')
				oneChar = oneChar - 'a' + 26;
			else if (oneChar == '+')
				oneChar = 62;
			else if (oneChar == '/')
				oneChar = 63;
			else if (oneChar == '=')
				oneChar = 0;
			else
				continue;

			c = (c << 6) | oneChar;
			if (++i == 4) {
				*newString++ = (char) (c >> 16);
				*newString++ = (char) (c >> 8);
				*newString++ = (char) c;
				i = 0;
			}
		}
		*newString++ = '\0';
		result = std::string(org_newString);
		free(org_newString);
		return result;
	} else
		return "";
}

