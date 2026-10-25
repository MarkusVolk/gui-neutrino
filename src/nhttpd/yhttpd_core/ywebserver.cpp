//=============================================================================
// YHTTPD
// Webserver Class : Until now: exact one instance
// libmicrohttpd accepts and parses; each request runs through the hook chain
// the way the old socket server ran it, /api/ goes to the REST API
//=============================================================================
// c++
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// system
#include <stdio.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <microhttpd.h>
// tuxbox
#include <configfile.h>

// yhttpd
#include <yhttpd.h>
#include "ywebserver.h"
#include "ylogging.h"
#include "helper.h"
#include "yhook.h"
#include "ycert.h"
#include <api/apiserver.h>
#include <system/set_threadname.h>

bool CWebserver::is_threading = true;

namespace {

// the one server, for the request handling to read its settings
CWebserver *server = NULL;

struct TRequest
{
	std::string	raw_uri;
	bool		started;
	THttp_Method	method;
	CStringList	ParameterList;
	CStringList	UrlData;
	CStringList	HeaderList;
	std::string	Body;
	std::string	content_type;
	bool		too_large;
	bool		is_api;
	MHD_PostProcessor *pp;
	std::string	field_name;
	std::string	field_value;
	CyhookHandler	*hh;

	TRequest() : started(false), method(M_UNKNOWN), too_large(false), is_api(false),
		pp(NULL), hh(NULL) {}
	~TRequest();
};

TRequest::~TRequest()
{
	if (pp)
		MHD_destroy_post_processor(pp);
	delete hh;
}

// Header names are case-insensitive (RFC 9110); they are kept as
// Content-Type, If-None-Match, ... whatever case a client sent them in
std::string canonical_header_name(const char *name)
{
	std::string out = name ? name : "";
	bool start = true;
	for (size_t i = 0; i < out.size(); i++) {
		out[i] = start ? toupper((unsigned char) out[i]) : tolower((unsigned char) out[i]);
		start = out[i] == '-';
	}
	return out;
}

// parameter = attribute "=" value; an attribute given more than once
// collects its values as <value1>,<value2>,..; every parameter is also
// stored under its position, so /control/rcem?KEY_OK finds KEY_OK as "1"
void parse_params(std::string param_string, CStringList &ParameterList)
{
	bool ende = false;
	std::string param, name, value, number;

	while (!ende) {
		if (!ySplitStringExact(param_string, "&", param, param_string))
			ende = true;
		if (ySplitStringExact(param, "=", name, value)) {
			name = decodeString(name);
			value = trim(decodeString(value));
			if (ParameterList[name].empty())
				ParameterList[name] = value;
			else {
				ParameterList[name] += ",";
				ParameterList[name] += value;
			}
		}
		else
			name = trim(decodeString(name));
		number = string_printf("%d", ParameterList.size() + 1);
		ParameterList[number] = name;
	}
}

// Split an URL into path, filename and fileext in UrlData, the query into
// ParameterList
void analyze_url(const std::string &url, CStringList &UrlData, CStringList &ParameterList)
{
	ParameterList.clear();
	std::string fullurl = trim(decodeString(url), "\r\n");
	UrlData["fullurl"] = fullurl;

	if (ySplitString(url, "?", UrlData["url"], UrlData["paramstring"])) {
		UrlData["url"] = decodeString(UrlData["url"]);
		parse_params(UrlData["paramstring"], ParameterList);
	} else {
		UrlData["url"] = fullurl;
		UrlData["paramstring"] = "";
	}

	if (!ySplitStringLast(UrlData["url"], "/", UrlData["path"], UrlData["filename"]))
		UrlData["path"] = "/";
	else
		UrlData["path"] += "/";
	if (UrlData["url"].length() == 1 || UrlData["url"][UrlData["url"].length() - 1] == '/') {
		UrlData["path"] = UrlData["url"];
		UrlData["filename"] = "index.html";
	}
	UrlData["filenamepure"] = "";
	UrlData["fileext"] = "";
	ySplitStringLast(UrlData["filename"], ".", UrlData["filenamepure"], UrlData["fileext"]);
}

THttp_Method method_of(const char *method)
{
	if (!strcmp(method, MHD_HTTP_METHOD_POST))	return M_POST;
	if (!strcmp(method, MHD_HTTP_METHOD_GET))	return M_GET;
	if (!strcmp(method, MHD_HTTP_METHOD_HEAD))	return M_HEAD;
	if (!strcmp(method, MHD_HTTP_METHOD_PUT))	return M_PUT;
	if (!strcmp(method, MHD_HTTP_METHOD_DELETE))	return M_DELETE;
	if (!strcmp(method, MHD_HTTP_METHOD_TRACE))	return M_TRACE;
	return M_UNKNOWN;
}

std::string client_address(struct MHD_Connection *connection)
{
	const union MHD_ConnectionInfo *ci =
		MHD_get_connection_info(connection, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
	if (!ci || !ci->client_addr)
		return "";
	char buf[INET6_ADDRSTRLEN] = "";
	const struct sockaddr *sa = ci->client_addr;
	if (sa->sa_family == AF_INET)
		inet_ntop(AF_INET, &((const struct sockaddr_in *) sa)->sin_addr, buf, sizeof(buf));
	else if (sa->sa_family == AF_INET6) {
		const struct in6_addr *a = &((const struct sockaddr_in6 *) sa)->sin6_addr;
		// the hooks compare IPv4 addresses as the old server gave them
		if (IN6_IS_ADDR_V4MAPPED(a))
			inet_ntop(AF_INET, &a->s6_addr[12], buf, sizeof(buf));
		else
			inet_ntop(AF_INET6, a, buf, sizeof(buf));
	}
	return buf;
}

MHD_Result collect_header(void *cls, enum MHD_ValueKind, const char *key, const char *value)
{
	CStringList *headers = (CStringList *) cls;
	std::string name = canonical_header_name(key);
	(*headers)[std::move(name)] = trim(value ? value : "");
	return MHD_YES;
}

void *uri_logger(void *, const char *uri, struct MHD_Connection *)
{
	TRequest *req = new TRequest;
	req->raw_uri = uri ? uri : "/";
	return req;
}

void request_completed(void *, struct MHD_Connection *, void **con_cls, enum MHD_RequestTerminationCode)
{
	TRequest *req = (TRequest *) *con_cls;
	if (req && req->hh)
		req->hh->Hooks_EndConnection();
	delete req;
	*con_cls = NULL;
}

// A multipart POST: its fields become the parameters, files are not taken
void finish_field(TRequest *req)
{
	if (!req->field_name.empty())
		req->ParameterList[req->field_name] = trim(decodeString(req->field_value));
	req->field_name.clear();
	req->field_value.clear();
}

MHD_Result post_iterator(void *cls, enum MHD_ValueKind, const char *key, const char *filename,
		const char *, const char *, const char *data, uint64_t, size_t size)
{
	TRequest *req = (TRequest *) cls;
	std::string name = key ? key : "";
	if (filename)
		return MHD_YES;
	if (name != req->field_name)
		finish_field(req);
	req->field_name = name;
	if (size)
		req->field_value.append(data, size);
	return MHD_YES;
}

// The header block BuildHeader() writes, as headers of the response;
// what follows the block (the body of a redirect) is left out
void add_headers(struct MHD_Response *response, const std::string &header_block)
{
	std::string head = header_block.substr(0, header_block.find("\r\n\r\n"));
	std::string::size_type pos = head.find("\r\n");
	// the first line is the status line
	while (pos != std::string::npos) {
		std::string::size_type next = head.find("\r\n", pos + 2);
		std::string line = head.substr(pos + 2, next == std::string::npos ? std::string::npos : next - pos - 2);
		pos = next;
		std::string name, value;
		if (!ySplitStringExact(line, ":", name, value))
			continue;
		name = trim(name);
		value = trim(value);
		// libmicrohttpd writes these itself
		if (!strcasecmp(name.c_str(), MHD_HTTP_HEADER_CONTENT_LENGTH)
				|| !strcasecmp(name.c_str(), MHD_HTTP_HEADER_CONNECTION)
				|| !strcasecmp(name.c_str(), MHD_HTTP_HEADER_DATE)
				|| !strcasecmp(name.c_str(), MHD_HTTP_HEADER_TRANSFER_ENCODING))
			continue;
		MHD_add_response_header(response, name.c_str(), value.c_str());
	}
}

/* queues the response and closes the connection after it for a client
 * that server.no_keep-alive_ips names */
MHD_Result queue(struct MHD_Connection *connection, unsigned int status, struct MHD_Response *response)
{
	if (server && !server->conf_no_keep_alive_ips.empty()
			&& !server->CheckKeepAliveAllowedByIP(client_address(connection)))
		MHD_add_response_header(response, MHD_HTTP_HEADER_CONNECTION, "close");
	MHD_Result ret = MHD_queue_response(connection, status, response);
	MHD_destroy_response(response);
	return ret;
}

MHD_Result queue_buffer(struct MHD_Connection *connection, int status, const std::string &header_block,
		const std::string &body)
{
	// what follows the header block (the body of a redirect), then the body, in one buffer MHD frees
	std::string::size_type tail = header_block.find("\r\n\r\n");
	size_t tail_len = tail == std::string::npos ? 0 : header_block.size() - tail - 4;
	size_t size = tail_len + body.size();
	char *content = (char *) malloc(size ? size : 1);
	if (!content)
		return MHD_NO;
	if (tail_len)
		memcpy(content, header_block.data() + tail + 4, tail_len);
	memcpy(content + tail_len, body.data(), body.size());
	struct MHD_Response *response =
		MHD_create_response_from_buffer_with_free_callback(size, content, free);
	if (!response) {
		free(content);
		return MHD_NO;
	}
	add_headers(response, header_block);
	return queue(connection, status, response);
}

MHD_Result queue_error(struct MHD_Connection *connection, CyhookHandler &hh, HttpResponseType status)
{
	hh.SetError(status);
	hh.yresult.clear();
	return queue_buffer(connection, status, hh.BuildHeader(), "");
}

MHD_Result queue_file(struct MHD_Connection *connection, CyhookHandler &hh)
{
	const std::string &filename = hh.UrlData["url"];
	int fd = open(filename.c_str(), O_RDONLY);
	struct stat st;
	if (fd < 0 || fstat(fd, &st) != 0) {
		if (fd >= 0)
			close(fd);
		return queue_error(connection, hh, HTTP_NOT_FOUND);
	}
	if (!S_ISREG(st.st_mode)) {
		close(fd);
		return queue_error(connection, hh, HTTP_NOT_FOUND);
	}
	off_t start = hh.RangeStart;
	if (start < 0)
		start = 0;
	if (start > st.st_size)
		start = st.st_size;
	off_t len = (hh.RangeStart == 0 && hh.RangeEnd == -1) ? st.st_size - start : hh.RangeEnd - start + 1;
	if (len < 0 || start + len > st.st_size)
		len = st.st_size - start;
	struct MHD_Response *response = MHD_create_response_from_fd_at_offset64(len, fd, start);
	if (!response) {
		close(fd);
		return MHD_NO;
	}
	add_headers(response, hh.BuildHeader(!hh.cached));
	return queue(connection, hh.httpStatus, response);
}

// The order the old server ran the hooks in: prepare (auth, cache,
// static files, rewrites), then produce, then a file if one was named
CyhookHandler &handler_of(TRequest *req)
{
	if (!req->hh) {
		req->hh = new CyhookHandler;
		req->hh->session_init(req->ParameterList, req->UrlData, req->HeaderList, Cyhttpd::ConfigList,
				req->method, true, req->Body);
	}
	return *req->hh;
}

MHD_Result run_hooks(struct MHD_Connection *connection, TRequest *req)
{
	CyhookHandler &hh = handler_of(req);

	CStringList discarded;
	do {
		hh.Hooks_PrepareResponse();
		if (hh.status == HANDLED_ERROR || hh.status == HANDLED_ABORT)
			return queue_buffer(connection, hh.httpStatus, hh.BuildHeader(), hh.yresult);
		if (hh.status == HANDLED_SENDFILE || hh.status == HANDLED_REWRITE)
			analyze_url(hh.NewURL, hh.UrlData, discarded);
		if (hh.status == HANDLED_REDIRECTION)
			return queue_buffer(connection, hh.httpStatus, hh.BuildHeader(), "");
	} while (hh.status == HANDLED_REWRITE);

	if (hh.httpStatus == HTTP_NOT_MODIFIED)
		return queue_buffer(connection, hh.httpStatus, hh.BuildHeader(), "");

	if (hh.status != HANDLED_SENDFILE)
		do {
			hh.Hooks_SendResponse();
			if (hh.status == HANDLED_READY || hh.status == HANDLED_CONTINUE || hh.status == HANDLED_ERROR)
				return queue_buffer(connection, hh.httpStatus, hh.BuildHeader(), hh.yresult);
			if (hh.status == HANDLED_ABORT)
				return MHD_NO;
			if (hh.status == HANDLED_SENDFILE || hh.status == HANDLED_REWRITE)
				analyze_url(hh.NewURL, hh.UrlData, discarded);
			if (hh.status == HANDLED_REDIRECTION)
				return queue_buffer(connection, hh.httpStatus, hh.BuildHeader(), "");
		} while (hh.status == HANDLED_REWRITE);

	if (hh.status == HANDLED_SENDFILE && hh.httpStatus == HTTP_REQUEST_RANGE_NOT_SATISFIABLE)
		return queue_error(connection, hh, hh.httpStatus);
	if (hh.status == HANDLED_SENDFILE)
		return queue_file(connection, hh);

	return queue_error(connection, hh, HTTP_NOT_FOUND);
}

MHD_Result run_api(struct MHD_Connection *connection, const char *method, TRequest *req)
{
	yapi::Request in;
	in.method = method;
	in.path = req->UrlData["url"];
	in.query = req->UrlData["paramstring"];
	in.body = req->Body;
	in.client = req->UrlData["clientaddr"];
	in.secure = req->UrlData["scheme"] == "https";
	in.headers = req->HeaderList;
	in.too_large = req->too_large;

	yapi::Response out;
	yapi::dispatch(in, out);

	struct MHD_Response *response;
	if (out.fd >= 0)
		response = MHD_create_response_from_fd_at_offset64(out.length, out.fd, out.offset);
	else
		response = MHD_create_response_from_buffer_copy(out.body.size(), out.body.data());
	if (!response) {
		if (out.fd >= 0)
			close(out.fd);
		return MHD_NO;
	}
	if (!out.content_type.empty())
		MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_TYPE, out.content_type.c_str());
	MHD_add_response_header(response, MHD_HTTP_HEADER_SERVER, WEBSERVERNAME);
	for (size_t i = 0; i < out.headers.size(); i++)
		MHD_add_response_header(response, out.headers[i].first.c_str(), out.headers[i].second.c_str());
	return queue(connection, out.status, response);
}

/* A page of the web interface asked for over HTTP from another machine
 * moves to HTTPS, where a paired phone signs in; programs that use /api
 * and /control over HTTP keep doing so. */
bool wants_https(TRequest *req, std::string &url)
{
	unsigned int https_port = server ? server->get_https_port() : 0;
	if (!https_port || req->UrlData["scheme"] == "https" || (req->method != M_GET && req->method != M_HEAD))
		return false;
	const std::string &client = req->UrlData["clientaddr"];
	if (client.compare(0, 4, "127.") == 0 || client == "::1")
		return false;
	const std::string &path = req->UrlData["url"];
	bool page = path == "/" || (path.size() > 5 && path.compare(path.size() - 5, 5, ".html") == 0);
	std::string host = req->HeaderList["Host"];
	if (!page || host.empty())
		return false;
	std::string::size_type end = host[0] == '[' ? host.find(']') : host.find(':');
	if (host[0] == '[' && end == std::string::npos)
		return false;
	if (end != std::string::npos)
		host = host.substr(0, host[0] == '[' ? end + 1 : end);
	if (host.find_first_of("/?#@ ") != std::string::npos)
		return false;
	url = "https://" + host + (https_port == 443 ? "" : ":" + std::to_string(https_port)) + req->raw_uri;
	return true;
}

MHD_Result access_handler(void *, struct MHD_Connection *connection, const char *, const char *method,
		const char *, const char *upload_data, size_t *upload_data_size, void **con_cls)
{
	TRequest *req = (TRequest *) *con_cls;
	if (!req) {
		req = new TRequest;
		*con_cls = req;
	}

	if (!req->started) {
		req->started = true;
		req->method = method_of(method);
		MHD_get_connection_values(connection, MHD_HEADER_KIND, collect_header, &req->HeaderList);
		analyze_url(req->raw_uri, req->UrlData, req->ParameterList);
		req->UrlData["startline"] = std::string(method) + " " + req->raw_uri + " HTTP/1.1";
		req->UrlData["httprotocol"] = "HTTP/1.1";
		req->UrlData["clientaddr"] = client_address(connection);
		const union MHD_ConnectionInfo *di = MHD_get_connection_info(connection, MHD_CONNECTION_INFO_DAEMON);
		req->UrlData["scheme"] = di && server && server->is_tls(di->daemon) ? "https" : "http";
		req->is_api = yapi::handles(req->UrlData["url"]);
		/* a tunnel or proxy in front of the box reaches MCP and its
		 * sign-in only, whatever else the build has */
		if (yapi::from_internet(req->HeaderList) && !yapi::open_to_internet(req->UrlData["url"])) {
			CyhookHandler hh;
			hh.Method = req->method;
			return queue_error(connection, hh, HTTP_FORBIDDEN);
		}
		req->content_type = req->HeaderList["Content-Type"];
		if (!req->is_api && req->method == M_POST
				&& req->content_type.compare(0, 19, "multipart/form-data") == 0)
			req->pp = MHD_create_post_processor(connection, 64 * 1024, post_iterator, req);
		if (!req->is_api && req->method == M_UNKNOWN) {
			CyhookHandler hh;
			hh.Method = M_UNKNOWN;
			return queue_error(connection, hh, HTTP_NOT_IMPLEMENTED);
		}
		return MHD_YES;
	}

	if (*upload_data_size) {
		if (req->pp) {
			if (MHD_post_process(req->pp, upload_data, *upload_data_size) != MHD_YES) {
				*upload_data_size = 0;
				return MHD_NO;
			}
		} else if (req->Body.size() + *upload_data_size > Y_CONFIG_MAX_JSON_BODY)
			req->too_large = true;
		else
			req->Body.append(upload_data, *upload_data_size);
		*upload_data_size = 0;
		return MHD_YES;
	}

	if (req->is_api)
		return run_api(connection, method, req);

	std::string https_url;
	if (wants_https(req, https_url)) {
		struct MHD_Response *response = MHD_create_response_from_buffer_copy(0, "");
		if (!response)
			return MHD_NO;
		MHD_add_response_header(response, MHD_HTTP_HEADER_LOCATION, https_url.c_str());
		MHD_add_response_header(response, MHD_HTTP_HEADER_SERVER, WEBSERVERNAME);
		return queue(connection, MHD_HTTP_FOUND, response);
	}

	if (req->pp) {
		MHD_destroy_post_processor(req->pp);
		req->pp = NULL;
		finish_field(req);
	} else if (req->too_large) {
		CyhookHandler hh;
		hh.Method = req->method;
		return queue_error(connection, hh, HTTP_REQUEST_ENTITY_TOO_LARGE);
	} else if (req->method == M_POST) {
		// the type may carry parameters: "...; charset=UTF-8"
		if (req->content_type.compare(0, 33, "application/x-www-form-urlencoded") == 0) {
			parse_params(req->Body, req->ParameterList);
			req->Body.clear();
		} else if (req->content_type.compare(0, 16, "application/json") != 0)
			req->Body.clear();
	}

	return run_hooks(connection, req);
}

bool resolve_bind_address(const std::string &host, struct sockaddr_in &sai)
{
	if (host.empty() || host == "0.0.0.0" || host == "*")
		return false;
	if (inet_pton(AF_INET, host.c_str(), &sai.sin_addr) == 1)
		return true;
	struct hostent *he = gethostbyname(host.c_str());
	if (he && he->h_addrtype == AF_INET && he->h_length == sizeof(sai.sin_addr)) {
		memcpy(&sai.sin_addr, he->h_addr_list[0], sizeof(sai.sin_addr));
		return true;
	}
	aprintf("bind address %s not usable, listening on all interfaces\n", host.c_str());
	return false;
}

} // namespace

//=============================================================================
// Constructor & Destructor & Initialization
//=============================================================================
CWebserver::CWebserver() {
	terminate = false;
	daemon = NULL;
	tls_daemon = NULL;
	tls_wanted = false;
	tls_port = HTTPS_STANDARD_PORT;
	port = HTTPD_STANDARD_PORT;
	pthread_mutex_init(&run_mutex, NULL);
	pthread_cond_init(&run_cond, NULL);
}
//-----------------------------------------------------------------------------
CWebserver::~CWebserver() {
	if (tls_daemon)
		MHD_stop_daemon(tls_daemon);
	if (daemon)
		MHD_stop_daemon(daemon);
	pthread_cond_destroy(&run_cond);
	pthread_mutex_destroy(&run_mutex);
}
//-----------------------------------------------------------------------------
struct MHD_Daemon *CWebserver::start(unsigned int _port, bool tls) {
	server = this;
	unsigned int flags = MHD_USE_ERROR_LOG;
	if (tls)
		flags |= MHD_USE_TLS;
	if (is_threading)
		flags |= MHD_USE_THREAD_PER_CONNECTION | MHD_USE_INTERNAL_POLLING_THREAD;
	else
		flags |= MHD_USE_INTERNAL_POLLING_THREAD;

	struct sockaddr_in sai;
	memset(&sai, 0, sizeof(sai));
	sai.sin_family = AF_INET;
	sai.sin_port = htons(_port);
	const bool bound = resolve_bind_address(bindAddress, sai);
	if (!bound)
		flags |= MHD_USE_DUAL_STACK;

	struct MHD_OptionItem ops[] = {
		{ MHD_OPTION_URI_LOG_CALLBACK, (intptr_t) uri_logger, NULL },
		{ MHD_OPTION_NOTIFY_COMPLETED, (intptr_t) request_completed, NULL },
		{ MHD_OPTION_CONNECTION_LIMIT, HTTPD_MAX_CONNECTIONS, NULL },
		{ MHD_OPTION_CONNECTION_TIMEOUT, 60, NULL },
		{ tls ? MHD_OPTION_HTTPS_MEM_KEY : MHD_OPTION_END, 0, (void *) tls_key.c_str() },
		{ tls ? MHD_OPTION_HTTPS_MEM_CERT : MHD_OPTION_END, 0, (void *) tls_chain.c_str() },
		{ bound ? MHD_OPTION_SOCK_ADDR : MHD_OPTION_END, 0, bound ? &sai : NULL },
		{ MHD_OPTION_END, 0, NULL }
	};
	/* an MHD_OPTION_END ends the array there; the options after it are
	 * the ones that apply, so move them up */
	struct MHD_OptionItem used[sizeof(ops) / sizeof(ops[0])];
	size_t n = 0;
	for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]) - 1; i++)
		if (ops[i].option != MHD_OPTION_END)
			used[n++] = ops[i];
	used[n] = ops[sizeof(ops) / sizeof(ops[0]) - 1];
	struct MHD_Daemon *d = MHD_start_daemon(flags, (uint16_t) _port, NULL, NULL, access_handler, NULL,
			MHD_OPTION_ARRAY, used, MHD_OPTION_END);
	if (!d && !bound) {
		// a kernel without IPv6
		flags &= ~MHD_USE_DUAL_STACK;
		d = MHD_start_daemon(flags, (uint16_t) _port, NULL, NULL, access_handler, NULL,
				MHD_OPTION_ARRAY, used, MHD_OPTION_END);
	}
	return d;
}

//-----------------------------------------------------------------------------
// HTTPS next to HTTP, on its own port, with the certificate of the box
void CWebserver::start_tls(void) {
	if (!tls_wanted)
		return;
	if (MHD_is_feature_supported(MHD_FEATURE_TLS) != MHD_YES) {
		aprintf("libmicrohttpd has no TLS, no HTTPS\n");
		return;
	}
	if (!ycert::server_pem(tls_key, tls_chain)) {
		aprintf("no certificate, no HTTPS\n");
		return;
	}
	unsigned int want = tls_port;
	tls_daemon = start(want, true);
	if (!tls_daemon && want == HTTPS_STANDARD_PORT) {
		fprintf(stderr, "[yhttpd] cannot listen on port %d. Retrying port %d.\n", HTTPS_STANDARD_PORT, HTTPS_FALLBACK_PORT);
		want = HTTPS_FALLBACK_PORT;
		tls_daemon = start(want, true);
	}
	if (tls_daemon) {
		tls_port = want;
		aprintf("HTTPS on port %d\n", tls_port);
	} else
		fprintf(stderr, "[yhttpd] cannot listen on port %d for HTTPS\n", want);
}
//-----------------------------------------------------------------------------
bool CWebserver::run(void) {
	set_threadname("ywebsrv::run");
	daemon = start(port, false);
	if (!daemon) {
		if (port != HTTPD_STANDARD_PORT) {
			// WebsiteMain.port in nhttpd.conf is changed by user
			fprintf(stderr, "[yhttpd] cannot listen on port %d. Abort.\n", port);
			return false;
		}
		fprintf(stderr, "[yhttpd] cannot listen on port %d. Retrying port %d.\n", HTTPD_STANDARD_PORT, HTTPD_FALLBACK_PORT);
		daemon = start(HTTPD_FALLBACK_PORT, false);
		if (!daemon) {
			fprintf(stderr, "[yhttpd] cannot listen on port %d. Abort.\n", HTTPD_FALLBACK_PORT);
			return false;
		}
		port = HTTPD_FALLBACK_PORT;
	}
	aprintf("listening on port %d\n", port);
	start_tls();

	pthread_mutex_lock(&run_mutex);
	while (!terminate)
		pthread_cond_wait(&run_cond, &run_mutex);
	pthread_mutex_unlock(&run_mutex);

	if (tls_daemon)
		MHD_stop_daemon(tls_daemon);
	tls_daemon = NULL;
	MHD_stop_daemon(daemon);
	daemon = NULL;
	return true;
}
//-----------------------------------------------------------------------------
void CWebserver::stop(void) {
	pthread_mutex_lock(&run_mutex);
	terminate = true;
	pthread_cond_broadcast(&run_cond);
	pthread_mutex_unlock(&run_mutex);
}
//-----------------------------------------------------------------------------
bool CWebserver::CheckKeepAliveAllowedByIP(std::string client_ip) {
	CStringVector::const_iterator it = conf_no_keep_alive_ips.begin();
	for (; it != conf_no_keep_alive_ips.end(); ++it)
		if (trim(*it) == client_ip)
			return false;
	return true;
}
