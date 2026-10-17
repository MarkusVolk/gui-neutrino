//=============================================================================
// YHTTPD
// Module: SendFile (mod_sendfile)
//-----------------------------------------------------------------------------
// Send a File (main) with given path and filename.
// It procuced a Response-Header (SendHeader).
// It supports Client caching mechanism "If-Modified-Since".
//-----------------------------------------------------------------------------
// RFC 2616 / 14.25 If-Modified-Since
//
//	The If-Modified-Since request-header field is used with a method to
//	make it conditional: if the requested variant has not been modified
//	since the time specified in this field, an entity will not be
//	returned from the server; instead, a 304 (not modified) response will
//	be returned without any message-body.
//
//	If-Modified-Since = "If-Modified-Since" ":" HTTP-date
//	An example of the field is:
//
//		If-Modified-Since: Sat, 29 Oct 1994 19:43:31 GMT
//
//	A GET method with an If-Modified-Since header and no Range header
//	requests that the identified entity be transferred only if it has
//	been modified since the date given by the If-Modified-Since header.
//	The algorithm for determining this includes the following cases:
//
//		a)	If the request would normally result in anything other than a
//			200 (OK) status, or if the passed If-Modified-Since date is
//			invalid, the response is exactly the same as for a normal GET.
//			A date which is later than the server's current time is
//			invalid.
//
//		b)	If the variant has been modified since the If-Modified-Since
//			date, the response is exactly the same as for a normal GET.
//
//		c)	If the variant has not been modified since a valid If-
//			Modified-Since date, the server SHOULD return a 304 (Not
//			Modified) response.
//
//	yjogol: ASSUMPTION Date-Format is ONLY RFC 1123 compatible!
//=============================================================================

// system
#include <cstdio>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <inttypes.h>
#include <map>
#include <tuple>
#include <system/helpers.h>
// yhttpd
#include <yconfig.h>
#include <ytypes_globals.h>
#include <helper.h>
#include "mod_sendfile.h"

//=============================================================================
// Initialization of static variables
//=============================================================================
CStringList CmodSendfile::sendfileTypes;

/* files up to this size get an entity tag from their content; the web
 * pages are far below it, recordings are not read for it */
#define ETAG_MAX_SIZE (1024 * 1024)

static std::string ContentTag(int fd, off_t size)
{
	uint64_t hash = 14695981039346656037ULL; /* FNV-1a */
	unsigned char buf[16384];
	ssize_t n;
	off_t pos = 0;
	while ((n = pread(fd, buf, sizeof(buf), pos)) > 0) {
		for (ssize_t i = 0; i < n; i++) {
			hash ^= buf[i];
			hash *= 1099511628211ULL;
		}
		pos += n;
	}
	if (n < 0 || pos != size)
		return "";
	return string_printf("\"%llx-%016llx\"", (unsigned long long) size, (unsigned long long) hash);
}

/* the tags of the files sent lately, by what identifies a file's content;
 * the map is cleared when full, which only costs the next hashes */
#define ETAG_CACHE_SIZE 256
struct TagKey {
	dev_t dev;
	ino_t ino;
	off_t size;
	time_t mtime;
	long mtime_nsec;
	bool operator<(const TagKey &o) const
	{
		return std::tie(dev, ino, size, mtime, mtime_nsec) < std::tie(o.dev, o.ino, o.size, o.mtime, o.mtime_nsec);
	}
};
static std::map<TagKey, std::string> tag_cache;
static pthread_mutex_t tag_mutex = PTHREAD_MUTEX_INITIALIZER;

static std::string CachedContentTag(int fd, const struct stat &st)
{
	TagKey key = { st.st_dev, st.st_ino, st.st_size, st.st_mtime, st.st_mtim.tv_nsec };
	pthread_mutex_lock(&tag_mutex);
	std::map<TagKey, std::string>::const_iterator it = tag_cache.find(key);
	std::string tag = it == tag_cache.end() ? "" : it->second;
	pthread_mutex_unlock(&tag_mutex);
	if (it != tag_cache.end())
		return tag;
	tag = ContentTag(fd, st.st_size);
	if (tag.empty()) /* a read error is not kept */
		return tag;
	pthread_mutex_lock(&tag_mutex);
	if (tag_cache.size() >= ETAG_CACHE_SIZE)
		tag_cache.clear();
	tag_cache[key] = tag;
	pthread_mutex_unlock(&tag_mutex);
	return tag;
}

static bool EtagMatches(const std::string &header, const std::string &tag)
{
	if (header.empty())
		return false;
	if (trim(header) == "*")
		return true;
	std::string rest = header, item;
	bool more = true;
	while (more) {
		more = ySplitStringExact(rest, ",", item, rest);
		std::string t = trim(item);
		if (t.compare(0, 2, "W/") == 0)
			t = t.substr(2);
		if (t == tag)
			return true;
	}
	return false;
}

//-----------------------------------------------------------------------------
// HOOK: Response Prepare Handler
// Response Prepare Check.
//-----------------------------------------------------------------------------
THandleStatus CmodSendfile::Hook_PrepareResponse(CyhookHandler *hh) {
	hh->status = HANDLED_NONE;

	log_level_printf(4, "mod_sendfile prepare hook start url:%s\n", hh->UrlData["fullurl"].c_str());

#ifdef Y_CONFIG_USE_HOSTEDWEB
	// for hosted webs: rewrite URL
	std::string _hosted=HOSTEDDOCUMENTURL;
	if((hh->UrlData["path"]).compare(0,_hosted.length(),HOSTEDDOCUMENTURL) == 0) // hosted Web ?
		hh->UrlData["path"]=hh->WebserverConfigList["WebsiteMain.hosted_directory"]+(hh->UrlData["path"]).substr(_hosted.length()-1);
#endif //Y_CONFIG_USE_HOSTEDWEB

	std::string mime = sendfileTypes[hh->UrlData["fileext"]];
	if (((!mime.empty()) || (hh->WebserverConfigList["mod_sendfile.sendAll"] == "true"))
			&& !(hh->UrlData["fileext"] == "yhtm" || hh->UrlData["fileext"] == "yjs" || hh->UrlData["fileext"] == "ysh")) {
		//TODO: Check allowed directories / actually in GetFileName
		// build filename
		std::string fullfilename = GetFileName(hh, hh->UrlData["path"],
				hh->UrlData["filename"]);
		int filed;
		if ((filed = OpenFile(hh, fullfilename)) != -1) //can access file?
		{
			struct stat statbuf;
			hh->LastModified = (time_t) 0;
			// It is a regular file?
			fstat(filed, &statbuf);
			if (S_ISREG(statbuf.st_mode)) {
				// get file size and modify date
				hh->ContentLength = statbuf.st_size;
				hh->LastModified = statbuf.st_mtime;
			}
			if (S_ISREG(statbuf.st_mode) && statbuf.st_size <= ETAG_MAX_SIZE)
				hh->ETag = CachedContentTag(filed, statbuf);
			close(filed);

			// check If-Modified-Since
			time_t if_modified_since = (time_t) - 1;
			if (!hh->HeaderList["If-Modified-Since"].empty()) {
				struct tm mod;
				if (strptime(hh->HeaderList["If-Modified-Since"].c_str(),
						RFC1123FMT, &mod) != NULL) {
					mod.tm_isdst = 0; // daylight saving flag!
					if_modified_since = mktime(&mod);
				}
			}

			// normalize obj_last_modified to GMT
			struct tm *tmp = gmtime(&(hh->LastModified));
			time_t LastModifiedGMT = mktime(tmp);
			bool modified = (if_modified_since == (time_t) - 1)
					|| (if_modified_since < LastModifiedGMT);
			/* With an entity tag only the tag counts: every file of a
			 * reproducibly built image carries the same date, so the date
			 * would keep files the browser has from an older image. */
			if (!hh->ETag.empty())
				modified = !EtagMatches(hh->HeaderList["If-None-Match"], hh->ETag);

			// Send normal or not-modified header
			if (modified) {
				hh->RangeStart = 0;
				hh->RangeEnd = hh->ContentLength - 1;
				const char *range = (hh->HeaderList["Range"].empty()) ? NULL : hh->HeaderList["Range"].c_str();
				if ((range &&
				     (sscanf(range, "bytes=%" PRId64 "-%" PRId64, &hh->RangeStart, &hh->RangeEnd) != 2) &&
				     (sscanf(range, "bytes=%" PRId64 "-", &hh->RangeStart) != 1))
				 || (hh->RangeStart > hh->RangeEnd)
				 || (hh->RangeEnd > hh->ContentLength - 1)) {
					hh->SetError(HTTP_REQUEST_RANGE_NOT_SATISFIABLE);
					aprintf("mod_sendfile: Client requested range '%s' which is outside of [0,%lld]\n", range, hh->ContentLength - 1);
				} else {
					hh->SendFile(fullfilename);
					hh->ResponseMimeType = mime;
					if (hh->RangeStart && (hh->RangeEnd != hh->ContentLength - 1))
						hh->httpStatus = HTTP_PARTIAL_CONTENT;
				}
			} else
				hh->SetHeader(HTTP_NOT_MODIFIED, mime, HANDLED_READY);
		} else {
			aprintf("mod_sendfile: File not found. url:(%s) fullfilename:(%s)\n",
					hh->UrlData["url"].c_str(), fullfilename.c_str());
			hh->SetError(HTTP_NOT_FOUND);
		}
	}
	log_level_printf(4, "mod_sendfile prepare hook end status:%d\n",
			(int) hh->status);

	return hh->status;
}

//-----------------------------------------------------------------------------
// HOOK: Hook_ReadConfig
// This hook ist called from ReadConfig
//-----------------------------------------------------------------------------
THandleStatus CmodSendfile::Hook_ReadConfig(CConfigFile *Config,
		CStringList &ConfigList) {
	std::string exttypes = Config->getString("mod_sendfile.mime_types",
			HTTPD_SENDFILE_EXT);
	ConfigList["mod_sendfile.mime_types"] = exttypes;
	ConfigList["mod_sendfile.sendAll"] = Config->getString(
			"mod_sendfile.sendAll", HTTPD_SENDFILE_ALL);

	bool ende = false;
	std::string item, ext, mime;
	sendfileTypes.clear();
	while (!ende) {
		if (!ySplitStringExact(exttypes, ",", item, exttypes))
			ende = true;
		if (ySplitStringExact(item, ":", ext, mime)) {
			ext = trim(ext);
			sendfileTypes[ext] = trim(mime);
		}
	}
	return HANDLED_CONTINUE;
}

//-----------------------------------------------------------------------------
// Send File: Build Filename
// First Look at PublicDocumentRoot than PrivateDocumentRoot than pure path
//-----------------------------------------------------------------------------
std::string CmodSendfile::GetFileName(CyhookHandler *hh, std::string path, std::string filename) {
	std::string tmpfilename;
	if (path[path.length() - 1] != '/')
		tmpfilename = path + "/" + filename;
	else
		tmpfilename = path + filename;
	if (access(hh->WebserverConfigList["WebsiteMain.override_directory"] + tmpfilename, R_OK) == 0)
		tmpfilename = hh->WebserverConfigList["WebsiteMain.override_directory"] + tmpfilename;
	else if (access(hh->WebserverConfigList["WebsiteMain.override_directory"] + tmpfilename + ".gz", R_OK) == 0)
		tmpfilename = hh->WebserverConfigList["WebsiteMain.override_directory"] + tmpfilename + ".gz";
	else if (access(hh->WebserverConfigList["WebsiteMain.directory"] + tmpfilename, R_OK) == 0)
		tmpfilename = hh->WebserverConfigList["WebsiteMain.directory"] + tmpfilename;
	else if (access(hh->WebserverConfigList["WebsiteMain.directory"] + tmpfilename + ".gz", R_OK) == 0)
		tmpfilename = hh->WebserverConfigList["WebsiteMain.directory"] + tmpfilename + ".gz";
#ifdef Y_CONFIG_FEATUE_SENDFILE_CAN_ACCESS_ALL
	else if(access(tmpfilename,R_OK) == 0)
	;
#endif
	else {
		return "";
	}
	return tmpfilename;
}
//-----------------------------------------------------------------------------
// Send File: Open File and check file type
//-----------------------------------------------------------------------------
int CmodSendfile::OpenFile(CyhookHandler *, std::string fullfilename) {
	int fd = -1;
	if (!fullfilename.empty()) {
		fd = open(fullfilename.c_str(), O_RDONLY | O_LARGEFILE);
		if (fd <= 0) {
			aprintf("cannot open file %s: ", fullfilename.c_str());
			dperror("");
		}
	}
	return fd;
}
//-----------------------------------------------------------------------------
// Send File: Determine MIME-Type for File-Extention
//-----------------------------------------------------------------------------
std::string CmodSendfile::GetContentType(std::string ext) {
	std::string ctype = "text/plain";
	ext = string_tolower(ext);
	for (unsigned int i = 0; i < (sizeof(MimeFileExtensions)
			/ sizeof(MimeFileExtensions[0])); i++)
		if (MimeFileExtensions[i].fileext == ext) {
			ctype = MimeFileExtensions[i].mime;
			break;
		}
	return ctype;
}
