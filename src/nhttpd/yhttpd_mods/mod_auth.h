//=============================================================================
// YHTTPD
// mod_auth : Authentication
//
// Reads the mod_auth settings of nhttpd.conf. Who may do what is decided
// by the REST API (yapi::level_of), for /control as well; the static files
// of the web interface are open to everyone.
//=============================================================================
#ifndef __yhttpd_mod_auth_h__
#define __yhttpd_mod_auth_h__

#include <yhook.h>
class CmAuth: public Cyhook {
public:
	CmAuth() {}
	~CmAuth() {}

	// virtual functions for HookHandler/Hook
	virtual std::string	getHookName(void) {return std::string("mod_auth");}
	virtual std::string	getHookVersion(void) {return std::string("$Revision$");}
	virtual THandleStatus	Hook_ReadConfig(CConfigFile *Config, CStringList &ConfigList);

	static std::string decodeBase64(const char *b64buffer);
};
#endif // __yhttpd_mod_auth_h__
