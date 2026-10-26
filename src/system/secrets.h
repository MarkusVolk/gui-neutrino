/*
	Secrets: API keys, tokens and share passwords kept outside of neutrino.conf

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

#ifndef __system_secrets__
#define __system_secrets__

#include <string>

/*
 * A secret is kept in the first store that works here:
 *  - the Secret Service of the desktop session (gnome-keyring, KWallet),
 *    when built with --enable-libsecret and a session bus is there,
 *  - a credential encrypted by systemd-creds under CONFIGDIR/credentials,
 *    with the host key or a TPM2 as root, with the user's key otherwise,
 *  - CONFIGDIR/secrets.conf, readable by its owner only, its values sealed
 *    with a key in DATADIR_VAR/secrets.key (a settings backup holds none).
 * The empty string removes a secret.
 */
namespace secrets
{
	std::string load(const std::string &name);
	bool store(const std::string &name, const std::string &value);
	const char *backend();
}

#endif
