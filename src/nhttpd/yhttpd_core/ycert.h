/*
	ycert: the certificates of the HTTPS server

	The box is its own small certificate authority: a CA certificate that
	a phone or a browser can be told to trust once, and a server
	certificate signed by it for the names and addresses the box has.
	When an address changes only the server certificate is made anew, so
	whoever trusts the CA keeps trusting the box. Keys and certificates
	are kept in CONFIGDIR/https, readable by the owner only.

	License: GPL

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public
	License as published by the Free Software Foundation; either
	version 2 of the License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
	General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef __yhttpd_ycert_h__
#define __yhttpd_ycert_h__

#include <string>

namespace ycert
{
	/* the private key and the certificate chain (server, then CA) for
	 * the HTTPS server; made or renewed first where needed */
	bool server_pem(std::string &key, std::string &chain);
	/* the CA certificate in PEM, empty before there is one */
	std::string ca_pem();
	/* SHA-256 of the CA certificate, as AA:BB:..; empty without one */
	std::string ca_fingerprint();
	/* a new CA and server certificate; trust given to the old CA ends */
	bool renew();
}

#endif
