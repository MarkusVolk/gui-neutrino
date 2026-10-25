/*
	ycert: the certificates of the HTTPS server

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

#include <yconfig.h>
#include "ycert.h"
#include "ylogging.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#define CERT_DIR		CONFIGDIR "/https"
#define CA_KEY			CERT_DIR "/ca.key"
#define CA_CERT			CERT_DIR "/ca.crt"
#define SERVER_KEY		CERT_DIR "/server.key"
#define SERVER_CERT		CERT_DIR "/server.crt"
#define SERVER_NAMES		CERT_DIR "/server.names"
#define CA_DAYS			3650
/* browsers take no longer for a server certificate, whoever issued it */
#define SERVER_DAYS		397
#define RENEW_DAYS		30

namespace
{

std::mutex lock;

std::string read_file(const char *name)
{
	std::ifstream in(name);
	std::stringstream s;
	s << in.rdbuf();
	return s.str();
}

bool write_file(const char *name, const std::string &text)
{
	std::string tmp = std::string(name) + ".new";
	int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return false;
	bool ok = write(fd, text.data(), text.size()) == (ssize_t) text.size();
	ok &= fsync(fd) == 0;
	ok &= close(fd) == 0;
	if (!ok || rename(tmp.c_str(), name) != 0) {
		unlink(tmp.c_str());
		return false;
	}
	return true;
}

std::string pem_of_key(EVP_PKEY *key)
{
	std::string out;
	BIO *b = BIO_new(BIO_s_mem());
	if (b && PEM_write_bio_PrivateKey(b, key, NULL, NULL, 0, NULL, NULL)) {
		char *p;
		long n = BIO_get_mem_data(b, &p);
		out.assign(p, n);
	}
	BIO_free(b);
	return out;
}

std::string pem_of_cert(X509 *x)
{
	std::string out;
	BIO *b = BIO_new(BIO_s_mem());
	if (b && PEM_write_bio_X509(b, x)) {
		char *p;
		long n = BIO_get_mem_data(b, &p);
		out.assign(p, n);
	}
	BIO_free(b);
	return out;
}

EVP_PKEY *key_of_pem(const std::string &pem)
{
	BIO *b = BIO_new_mem_buf(pem.data(), (int) pem.size());
	EVP_PKEY *k = b ? PEM_read_bio_PrivateKey(b, NULL, NULL, NULL) : NULL;
	BIO_free(b);
	return k;
}

X509 *cert_of_pem(const std::string &pem)
{
	BIO *b = BIO_new_mem_buf(pem.data(), (int) pem.size());
	X509 *x = b ? PEM_read_bio_X509(b, NULL, NULL, NULL) : NULL;
	BIO_free(b);
	return x;
}

bool add_ext(X509 *x, X509 *issuer, int nid, const char *value)
{
	X509V3_CTX ctx;
	X509V3_set_ctx_nodb(&ctx);
	X509V3_set_ctx(&ctx, issuer, x, NULL, NULL, 0);
	X509_EXTENSION *ext = X509V3_EXT_conf_nid(NULL, &ctx, nid, value);
	if (!ext)
		return false;
	bool ok = X509_add_ext(x, ext, -1) == 1;
	X509_EXTENSION_free(ext);
	return ok;
}

std::string host_name()
{
	char name[256] = "";
	if (gethostname(name, sizeof(name) - 1) != 0 || !name[0])
		strcpy(name, "neutrino");
	return name;
}

/* the subject alternative names the server certificate needs now */
std::string current_names()
{
	std::set<std::string> ips;
	ips.insert("127.0.0.1");
	ips.insert("::1");
	struct ifaddrs *list = NULL;
	if (getifaddrs(&list) == 0) {
		for (struct ifaddrs *i = list; i; i = i->ifa_next) {
			if (!i->ifa_addr || (i->ifa_flags & IFF_LOOPBACK))
				continue;
			char buf[INET6_ADDRSTRLEN] = "";
			if (i->ifa_addr->sa_family == AF_INET)
				inet_ntop(AF_INET, &((struct sockaddr_in *) i->ifa_addr)->sin_addr, buf, sizeof(buf));
			else if (i->ifa_addr->sa_family == AF_INET6) {
				const struct in6_addr *a = &((struct sockaddr_in6 *) i->ifa_addr)->sin6_addr;
				/* a link-local address needs a zone, which no URL of a phone has */
				if (IN6_IS_ADDR_LINKLOCAL(a))
					continue;
				inet_ntop(AF_INET6, a, buf, sizeof(buf));
			} else
				continue;
			ips.insert(buf);
		}
		freeifaddrs(list);
	}
	std::string host = host_name();
	std::string names = "DNS:" + host + ",DNS:" + host + ".local,DNS:localhost";
	for (std::set<std::string>::const_iterator it = ips.begin(); it != ips.end(); ++it)
		names += ",IP:" + *it;
	return names;
}

bool set_serial(X509 *x)
{
	BIGNUM *bn = BN_new();
	bool ok = bn && BN_rand(bn, 127, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY)
		&& BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(x));
	BN_free(bn);
	return ok;
}

X509 *new_cert(EVP_PKEY *key, const std::string &cn, int days)
{
	X509 *x = X509_new();
	X509_NAME *name = X509_NAME_new();
	bool ok = x && name && X509_set_version(x, 2) && set_serial(x)
		&& X509_gmtime_adj(X509_getm_notBefore(x), -3600)
		&& X509_gmtime_adj(X509_getm_notAfter(x), (long) days * 24 * 3600)
		&& X509_set_pubkey(x, key)
		&& X509_NAME_add_entry_by_txt(name, "O", MBSTRING_UTF8, (const unsigned char *) "Neutrino", -1, -1, 0)
		&& X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8, (const unsigned char *) cn.c_str(), -1, -1, 0)
		&& X509_set_subject_name(x, name);
	X509_NAME_free(name);
	if (!ok) {
		X509_free(x);
		return NULL;
	}
	return x;
}

bool make_ca(std::string &key_pem, std::string &cert_pem)
{
	EVP_PKEY *key = EVP_EC_gen("P-256");
	X509 *x = key ? new_cert(key, "Neutrino " + host_name() + " CA", CA_DAYS) : NULL;
	bool ok = x && X509_set_issuer_name(x, X509_get_subject_name(x))
		&& add_ext(x, x, NID_basic_constraints, "critical,CA:TRUE,pathlen:0")
		&& add_ext(x, x, NID_key_usage, "critical,keyCertSign,cRLSign")
		&& add_ext(x, x, NID_subject_key_identifier, "hash")
		&& X509_sign(x, key, EVP_sha256()) > 0;
	if (ok) {
		key_pem = pem_of_key(key);
		cert_pem = pem_of_cert(x);
		ok = !key_pem.empty() && !cert_pem.empty() && write_file(CA_KEY, key_pem) && write_file(CA_CERT, cert_pem);
	}
	X509_free(x);
	EVP_PKEY_free(key);
	return ok;
}

bool make_server(const std::string &ca_key_pem, const std::string &ca_cert_pem, const std::string &names)
{
	EVP_PKEY *ca_key = key_of_pem(ca_key_pem);
	X509 *ca = cert_of_pem(ca_cert_pem);
	EVP_PKEY *key = EVP_EC_gen("P-256");
	X509 *x = key ? new_cert(key, host_name(), SERVER_DAYS) : NULL;
	bool ok = ca_key && ca && x && X509_set_issuer_name(x, X509_get_subject_name(ca))
		&& add_ext(x, ca, NID_basic_constraints, "critical,CA:FALSE")
		&& add_ext(x, ca, NID_key_usage, "critical,digitalSignature")
		&& add_ext(x, ca, NID_ext_key_usage, "serverAuth")
		&& add_ext(x, ca, NID_subject_key_identifier, "hash")
		&& add_ext(x, ca, NID_authority_key_identifier, "keyid:always")
		&& add_ext(x, ca, NID_subject_alt_name, names.c_str())
		&& X509_sign(x, ca_key, EVP_sha256()) > 0;
	if (ok)
		ok = write_file(SERVER_KEY, pem_of_key(key)) && write_file(SERVER_CERT, pem_of_cert(x))
			&& write_file(SERVER_NAMES, names);
	X509_free(x);
	EVP_PKEY_free(key);
	X509_free(ca);
	EVP_PKEY_free(ca_key);
	return ok;
}

bool expiring(const std::string &cert_pem)
{
	X509 *x = cert_of_pem(cert_pem);
	if (!x)
		return true;
	int days = 0, secs = 0;
	bool out = !ASN1_TIME_diff(&days, &secs, NULL, X509_get0_notAfter(x)) || days < RENEW_DAYS;
	X509_free(x);
	return out;
}

bool ready_dir()
{
	if (mkdir(CERT_DIR, 0700) != 0 && errno != EEXIST)
		return false;
	chmod(CERT_DIR, 0700);
	return true;
}

} // namespace

bool ycert::server_pem(std::string &key, std::string &chain)
{
	std::lock_guard<std::mutex> l(lock);
	if (!ready_dir()) {
		aprintf("[ycert] cannot create %s\n", CERT_DIR);
		return false;
	}
	std::string ca_key = read_file(CA_KEY), ca_cert = read_file(CA_CERT);
	bool new_ca = ca_key.empty() || ca_cert.empty() || expiring(ca_cert);
	if (new_ca && !make_ca(ca_key, ca_cert)) {
		aprintf("[ycert] cannot make the CA certificate\n");
		return false;
	}
	std::string names = current_names();
	key = read_file(SERVER_KEY);
	std::string cert = read_file(SERVER_CERT);
	if (new_ca || key.empty() || cert.empty() || read_file(SERVER_NAMES) != names || expiring(cert)) {
		if (!make_server(ca_key, ca_cert, names)) {
			aprintf("[ycert] cannot make the server certificate\n");
			return false;
		}
		aprintf("[ycert] server certificate for %s\n", names.c_str());
		key = read_file(SERVER_KEY);
		cert = read_file(SERVER_CERT);
	}
	chain = cert + ca_cert;
	return !key.empty() && !cert.empty();
}

std::string ycert::ca_pem()
{
	std::lock_guard<std::mutex> l(lock);
	return read_file(CA_CERT);
}

std::string ycert::ca_fingerprint()
{
	X509 *x = cert_of_pem(ca_pem());
	if (!x)
		return "";
	unsigned char md[EVP_MAX_MD_SIZE];
	unsigned int n = 0;
	std::string out;
	if (X509_digest(x, EVP_sha256(), md, &n)) {
		char hex[4];
		for (unsigned int i = 0; i < n; i++) {
			snprintf(hex, sizeof(hex), i ? ":%02X" : "%02X", md[i]);
			out += hex;
		}
	}
	X509_free(x);
	return out;
}

bool ycert::renew()
{
	std::lock_guard<std::mutex> l(lock);
	if (!ready_dir())
		return false;
	std::string ca_key, ca_cert;
	return make_ca(ca_key, ca_cert) && make_server(ca_key, ca_cert, current_names());
}
