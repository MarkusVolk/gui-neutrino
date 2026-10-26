/*
	Secrets: API keys and tokens kept outside of neutrino.conf

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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "secrets.h"

#include <map>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/rand.h>

#ifdef ENABLE_LIBSECRET
#include <libsecret/secret.h>
#endif

#include <configfile.h>
#include <system/helpers.h>

#define SECRETS_FILE		CONFIGDIR "/secrets.conf"
#define SECRETS_CRED_DIR	CONFIGDIR "/credentials"
/* outside CONFIGDIR, so that a copy or backup of the settings holds no usable secret */
#define SECRETS_KEY_FILE	DATADIR_VAR "/secrets.key"
#define SEALED_PREFIX		"enc1:"

namespace
{

enum store_t { STORE_NONE = -1, STORE_SECRET_SERVICE, STORE_SYSTEMD_CREDS, STORE_FILE, STORE_COUNT };

const char *store_names[STORE_COUNT] = { "secret service", "systemd-creds", SECRETS_FILE };

std::map<std::string, std::string> cache;

/* runs argv without a shell, feeds input to its stdin and collects its stdout */
bool run(const std::vector<std::string> &args, const std::string &input, std::string *output)
{
	int in[2], out[2];
	if (pipe2(in, O_CLOEXEC))
		return false;
	if (pipe2(out, O_CLOEXEC)) {
		close(in[0]);
		close(in[1]);
		return false;
	}
	pid_t pid = fork();
	if (pid < 0) {
		close(in[0]); close(in[1]);
		close(out[0]); close(out[1]);
		return false;
	}
	if (pid == 0) {
		dup2(in[0], STDIN_FILENO);
		dup2(out[1], STDOUT_FILENO);
		int null = open("/dev/null", O_WRONLY);
		if (null >= 0)
			dup2(null, STDERR_FILENO);
		std::vector<char *> argv;
		for (size_t i = 0; i < args.size(); i++)
			argv.push_back(const_cast<char *>(args[i].c_str()));
		argv.push_back(NULL);
		execvp(argv[0], &argv[0]);
		_exit(127);
	}
	close(in[0]);
	close(out[1]);

	void (*old_sigpipe)(int) = signal(SIGPIPE, SIG_IGN);
	size_t done = 0;
	while (done < input.size()) {
		ssize_t n = write(in[1], input.data() + done, input.size() - done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		done += n;
	}
	close(in[1]);
	signal(SIGPIPE, old_sigpipe);

	std::string result;
	char buf[1024];
	for (;;) {
		ssize_t n = read(out[0], buf, sizeof(buf));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		result.append(buf, n);
	}
	close(out[0]);

	int status;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	if (output)
		*output = result;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* ---- Secret Service ---------------------------------------------------- */

#ifdef ENABLE_LIBSECRET
const SecretSchema *secret_schema()
{
	static SecretSchema schema;
	if (!schema.name) {
		schema.name = "org.tuxbox.Neutrino.Secret";
		schema.flags = SECRET_SCHEMA_NONE;
		schema.attributes[0].name = "name";
		schema.attributes[0].type = SECRET_SCHEMA_ATTRIBUTE_STRING;
	}
	return &schema;
}

bool secret_service_usable()
{
	static int usable = -1;
	if (usable < 0) {
		usable = 0;
		const char *runtime = getenv("XDG_RUNTIME_DIR");
		if (getenv("DBUS_SESSION_BUS_ADDRESS") || (runtime && file_exists((std::string(runtime) + "/bus").c_str()))) {
			GError *error = NULL;
			gchar *probe = secret_password_lookup_sync(secret_schema(), NULL, &error, "name", "neutrino-probe", NULL);
			if (error)
				g_error_free(error);
			else
				usable = 1;
			if (probe)
				secret_password_free(probe);
		}
	}
	return usable == 1;
}

bool secret_service_load(const std::string &name, std::string &value)
{
	GError *error = NULL;
	gchar *secret = secret_password_lookup_sync(secret_schema(), NULL, &error, "name", name.c_str(), NULL);
	if (error) {
		g_error_free(error);
		return false;
	}
	if (!secret)
		return false;
	value = secret;
	secret_password_free(secret);
	return true;
}

bool secret_service_store(const std::string &name, const std::string &value)
{
	GError *error = NULL;
	std::string label = "Neutrino: " + name;
	gboolean ok = secret_password_store_sync(secret_schema(), SECRET_COLLECTION_DEFAULT, label.c_str(),
						 value.c_str(), NULL, &error, "name", name.c_str(), NULL);
	if (error) {
		g_error_free(error);
		return false;
	}
	return ok;
}

void secret_service_remove(const std::string &name)
{
	GError *error = NULL;
	secret_password_clear_sync(secret_schema(), NULL, &error, "name", name.c_str(), NULL);
	if (error)
		g_error_free(error);
}
#else
bool secret_service_usable() { return false; }
bool secret_service_load(const std::string &, std::string &) { return false; }
bool secret_service_store(const std::string &, const std::string &) { return false; }
void secret_service_remove(const std::string &) {}
#endif

/* ---- systemd-creds ----------------------------------------------------- */

std::string cred_path(const std::string &name)
{
	return std::string(SECRETS_CRED_DIR) + "/" + name + ".cred";
}

std::vector<std::string> creds_command(const char *verb, const std::string &name)
{
	std::vector<std::string> args;
	args.push_back("systemd-creds");
	if (geteuid())
		args.push_back("--user");
	args.push_back(verb);
	args.push_back("--name=neutrino-" + name);
	return args;
}

bool systemd_creds_usable()
{
	static int usable = -1;
	if (usable < 0)
		usable = find_executable("systemd-creds").empty() ? 0 : 1;
	return usable == 1;
}

bool systemd_creds_load(const std::string &name, std::string &value)
{
	std::string path = cred_path(name);
	if (!file_exists(path.c_str()))
		return false;
	std::vector<std::string> args = creds_command("decrypt", name);
	args.push_back(path);
	args.push_back("-");
	return run(args, "", &value);
}

bool systemd_creds_store(const std::string &name, const std::string &value)
{
	mkdir(SECRETS_CRED_DIR, 0700);
	std::string path = cred_path(name);
	std::string tmp = path + ".tmp";
	std::vector<std::string> args = creds_command("encrypt", name);
	args.push_back("-");
	args.push_back(tmp);
	if (!run(args, value, NULL)) {
		unlink(tmp.c_str());
		return false;
	}
	chmod(tmp.c_str(), 0600);
	return rename(tmp.c_str(), path.c_str()) == 0;
}

void systemd_creds_remove(const std::string &name)
{
	unlink(cred_path(name).c_str());
}

/* ---- file only its owner can read -------------------------------------- */

/* The values in the file are sealed with AES-256-GCM under a key of the box
   kept in SECRETS_KEY_FILE. The box opens them on its own, so root on the box
   can as well; the seal keeps them out of settings backups and copies. */
bool file_key(unsigned char key[32], bool create)
{
	int fd = open(SECRETS_KEY_FILE, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		bool ok = read(fd, key, 32) == 32;
		close(fd);
		return ok;
	}
	if (!create || RAND_bytes(key, 32) != 1)
		return false;
	safe_mkdir(DATADIR_VAR);
	std::string tmp = std::string(SECRETS_KEY_FILE) + ".tmp";
	fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return false;
	bool ok = write(fd, key, 32) == 32 && fsync(fd) == 0;
	close(fd);
	if (!ok || rename(tmp.c_str(), SECRETS_KEY_FILE) != 0) {
		unlink(tmp.c_str());
		return false;
	}
	return true;
}

bool seal(const std::string &value, std::string &out)
{
	unsigned char key[32], iv[12], tag[16];
	if (!file_key(key, true) || RAND_bytes(iv, sizeof(iv)) != 1)
		return false;
	std::vector<unsigned char> buf(sizeof(iv) + value.size() + sizeof(tag));
	memcpy(buf.data(), iv, sizeof(iv));
	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	int len = 0, fin = 0;
	bool ok = ctx && EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, key, iv) == 1
		&& EVP_EncryptUpdate(ctx, buf.data() + sizeof(iv), &len, (const unsigned char *) value.data(), value.size()) == 1
		&& EVP_EncryptFinal_ex(ctx, buf.data() + sizeof(iv) + len, &fin) == 1
		&& EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, sizeof(tag), tag) == 1;
	EVP_CIPHER_CTX_free(ctx);
	OPENSSL_cleanse(key, sizeof(key));
	if (!ok)
		return false;
	memcpy(buf.data() + sizeof(iv) + len + fin, tag, sizeof(tag));
	std::vector<unsigned char> b64(4 * ((buf.size() + 2) / 3) + 1);
	int n = EVP_EncodeBlock(b64.data(), buf.data(), buf.size());
	out = SEALED_PREFIX + std::string((const char *) b64.data(), n);
	return true;
}

/* false for a sealed value whose key is gone or another one */
bool unseal(const std::string &text, std::string &value, bool &was_sealed)
{
	was_sealed = text.compare(0, strlen(SEALED_PREFIX), SEALED_PREFIX) == 0;
	if (!was_sealed) {
		value = text;
		return true;
	}
	std::string b64 = text.substr(strlen(SEALED_PREFIX));
	std::vector<unsigned char> buf(3 * (b64.size() / 4) + 3);
	int n = EVP_DecodeBlock(buf.data(), (const unsigned char *) b64.data(), b64.size());
	if (n < 0)
		return false;
	for (size_t i = b64.size(); i > 0 && b64[i - 1] == '='; i--)
		n--;
	unsigned char key[32];
	if (n < 28 || !file_key(key, false))
		return false;
	const unsigned char *iv = buf.data(), *ct = buf.data() + 12, *tag = buf.data() + n - 16;
	int ct_len = n - 28;
	std::vector<unsigned char> plain(ct_len + 16);
	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	int len = 0, fin = 0;
	bool ok = ctx && EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, key, iv) == 1
		&& EVP_DecryptUpdate(ctx, plain.data(), &len, ct, ct_len) == 1
		&& EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void *) tag) == 1
		&& EVP_DecryptFinal_ex(ctx, plain.data() + len, &fin) == 1;
	EVP_CIPHER_CTX_free(ctx);
	OPENSSL_cleanse(key, sizeof(key));
	if (ok)
		value.assign((const char *) plain.data(), len + fin);
	OPENSSL_cleanse(plain.data(), plain.size());
	return ok;
}

bool file_store(const std::string &name, const std::string &value);

bool file_load(const std::string &name, std::string &value)
{
	CConfigFile file(',');
	if (!file_exists(SECRETS_FILE) || !file.loadConfig(SECRETS_FILE))
		return false;
	std::string text = file.getString(name, "");
	if (text.empty())
		return false;
	bool was_sealed;
	if (!unseal(text, value, was_sealed)) {
		fprintf(stderr, "[secrets] %s cannot be opened: %s is missing or another one\n", name.c_str(), SECRETS_KEY_FILE);
		return false;
	}
	/* a value from before the seal is sealed now */
	if (!was_sealed)
		file_store(name, value);
	return !value.empty();
}

bool file_save(CConfigFile &file)
{
	mode_t old = umask(077);
	bool ok = file.saveConfig(SECRETS_FILE);
	umask(old);
	chmod(SECRETS_FILE, 0600);
	return ok;
}

bool file_store(const std::string &name, const std::string &value)
{
	CConfigFile file(',');
	if (file_exists(SECRETS_FILE))
		file.loadConfig(SECRETS_FILE);
	std::string sealed;
	/* without a key file (a read-only /var) the file's own mode is all that is left */
	if (!seal(value, sealed)) {
		fprintf(stderr, "[secrets] cannot seal %s, it is kept unsealed\n", name.c_str());
		sealed = value;
	}
	file.setString(name, sealed);
	return file_save(file);
}

void file_remove(const std::string &name)
{
	CConfigFile file(',');
	if (!file_exists(SECRETS_FILE) || !file.loadConfig(SECRETS_FILE))
		return;
	if (file.deleteKey(name))
		file_save(file);
}

/* ---- the stores in order ----------------------------------------------- */

bool usable(int s)
{
	switch (s) {
	case STORE_SECRET_SERVICE: return secret_service_usable();
	case STORE_SYSTEMD_CREDS: return systemd_creds_usable();
	default: return true;
	}
}

bool load_from(int s, const std::string &name, std::string &value)
{
	switch (s) {
	case STORE_SECRET_SERVICE: return secret_service_load(name, value);
	case STORE_SYSTEMD_CREDS: return systemd_creds_load(name, value);
	default: return file_load(name, value);
	}
}

bool store_to(int s, const std::string &name, const std::string &value)
{
	switch (s) {
	case STORE_SECRET_SERVICE: return secret_service_store(name, value);
	case STORE_SYSTEMD_CREDS: return systemd_creds_store(name, value);
	default: return file_store(name, value);
	}
}

void remove_from(int s, const std::string &name)
{
	switch (s) {
	case STORE_SECRET_SERVICE: secret_service_remove(name); break;
	case STORE_SYSTEMD_CREDS: systemd_creds_remove(name); break;
	default: file_remove(name); break;
	}
}

int best_store()
{
	for (int s = 0; s < STORE_COUNT; s++)
		if (usable(s))
			return s;
	return STORE_NONE;
}

/* writes value to the best store that takes it and removes it from all others */
bool put(const std::string &name, const std::string &value)
{
	int kept = STORE_NONE;
	if (!value.empty()) {
		for (int s = 0; s < STORE_COUNT && kept == STORE_NONE; s++)
			if (usable(s) && store_to(s, name, value))
				kept = s;
		if (kept == STORE_NONE)
			return false;
	}
	for (int s = 0; s < STORE_COUNT; s++)
		if (s != kept && (s == STORE_FILE || usable(s)))
			remove_from(s, name);
	return true;
}

} /* namespace */

std::string secrets::load(const std::string &name)
{
	std::map<std::string, std::string>::iterator it = cache.find(name);
	if (it != cache.end())
		return it->second;

	std::string value;
	int found = STORE_NONE;
	for (int s = 0; s < STORE_COUNT && found == STORE_NONE; s++)
		if (usable(s) && load_from(s, name, value))
			found = s;

	/* a secret left in a weaker store moves to the best one */
	if (found != STORE_NONE && found != best_store())
		put(name, value);

	cache[name] = value;
	return value;
}

bool secrets::store(const std::string &name, const std::string &value)
{
	std::map<std::string, std::string>::iterator it = cache.find(name);
	if (it != cache.end() && it->second == value)
		return true;
	if (!put(name, value))
		return false;
	cache[name] = value;
	return true;
}

const char *secrets::backend()
{
	int s = best_store();
	return s == STORE_NONE ? "none" : store_names[s];
}
