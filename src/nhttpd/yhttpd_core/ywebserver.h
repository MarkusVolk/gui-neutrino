//=============================================================================
// YHTTPD
// Webserver Class : Until now: exact one instance
// The transport is libmicrohttpd; requests are handed to the hook chain
//=============================================================================
#ifndef __yhttpd_ywebserver_h__
#define __yhttpd_ywebserver_h__

#include <pthread.h>
#include <string>
#include <yconfig.h>
#include "ytypes_globals.h"

struct MHD_Daemon;

class CWebserver
{
private:
	pthread_mutex_t	run_mutex;
	pthread_cond_t	run_cond;
	struct MHD_Daemon *daemon;
	bool		start(unsigned int _port);
protected:
	bool 		terminate;				// flag: indicate to terminate the Webserver
	unsigned int 	port;					// Port to listen on
	std::string	bindAddress;				// Address/interface to bind to

public:
	static bool 	is_threading;				// Use a thread per connection
	CStringVector 	conf_no_keep_alive_ips;			// List of IP for NO keep-alive

	// constructor & destructor
	CWebserver();
	~CWebserver(void);

	void 		init(unsigned int _port, bool _is_threading, const std::string &_bindAddress = std::string()) // Initialize Webserver Settings
				{port=_port; is_threading=_is_threading; bindAddress = _bindAddress;}
	void		set_conf_no_keep_alive_ips(CStringVector _conf_no_keep_alive_ips)
				{conf_no_keep_alive_ips=_conf_no_keep_alive_ips;}
	bool 		run(void);				// Start the Webserver, return when stopped
	void 		stop(void);				// Stop the Webserver
	unsigned int	get_port(void) {return port;}
	bool		CheckKeepAliveAllowedByIP(std::string client_ip); // Check if IP is allowed for keep-alive
};

#endif /* __yhttpd_ywebserver_h__ */
