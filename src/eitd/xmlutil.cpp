/*
 * (C) 2001 by fnbrd,
 * Copyright (C) 2008-2009, 2012-2013 Stefan Seyfried
 *
 * Copyright (C) 2011-2012 CoolStream International Ltd
 *
 * License: GPLv2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <netdb.h>
#include <time.h>
#include <string>
#include <set>
#include <system/helpers.h>

#include <system/helpers.h>

#include <xmltree/xmlinterface.h>
#include <zapit/client/zapittools.h>
#include <zapit/bouquets.h>

#include <driver/abstime.h>

#include "xmlutil.h"
#include "eitd.h"
#include "debug.h"
#include <system/set_threadname.h>
#ifdef ENABLE_XMLTV_XZ
#include <lzma.h>
#endif

void addEvent(const SIevent &evt, const time_t zeit, bool cn = false);
extern MySIeventsOrderServiceUniqueKeyFirstStartTimeEventUniqueKey mySIeventsOrderServiceUniqueKeyFirstStartTimeEventUniqueKey;
void readerStarted(void);
void readerFinished(void);
extern int sectionsd_stop;
extern pthread_rwlock_t eventsLock;
extern bool dvb_time_update;
extern CBouquetManager *g_bouquetManager;

std::string epg_filter_dir = ZAPITDIR "/epgfilter.xml";
std::string dvbtime_filter_dir = ZAPITDIR "/dvbtimefilter.xml";
bool epg_filter_is_whitelist = false;
bool epg_filter_except_current_next = false;

inline void readLockEvents(void)
{
	pthread_rwlock_rdlock(&eventsLock);
}
inline void unlockEvents(void)
{
	pthread_rwlock_unlock(&eventsLock);
}

struct EPGFilter
{
	t_original_network_id onid;
	t_transport_stream_id tsid;
	t_service_id sid;
	EPGFilter *next;
};

struct ChannelBlacklist
{
	t_channel_id chan;
	t_channel_id mask;
	ChannelBlacklist *next;
};

struct ChannelNoDVBTimelist
{
	t_channel_id chan;
	t_channel_id mask;
	ChannelNoDVBTimelist *next;
};

static EPGFilter *CurrentEPGFilter = NULL;
static ChannelBlacklist *CurrentBlacklist = NULL;
static ChannelNoDVBTimelist *CurrentNoDVBTime = NULL;

bool checkEPGFilter(t_original_network_id onid, t_transport_stream_id tsid, t_service_id sid)
{
	EPGFilter *filterptr = CurrentEPGFilter;
	while (filterptr)
	{
		if (((filterptr->onid == onid) || (filterptr->onid == 0)) &&
				((filterptr->tsid == tsid) || (filterptr->tsid == 0)) &&
				((filterptr->sid == sid) || (filterptr->sid == 0)))
			return true;
		filterptr = filterptr->next;
	}
	return false;
}

bool checkBlacklist(t_channel_id channel_id)
{
	ChannelBlacklist *blptr = CurrentBlacklist;
	while (blptr)
	{
		if (blptr->chan == (channel_id & blptr->mask))
			return true;
		blptr = blptr->next;
	}
	return false;
}

bool checkNoDVBTimelist(t_channel_id channel_id)
{
	ChannelNoDVBTimelist *blptr = CurrentNoDVBTime;
	while (blptr)
	{
		if (blptr->chan == (channel_id & blptr->mask))
			return true;
		blptr = blptr->next;
	}
	return false;
}

void addEPGFilter(t_original_network_id onid, t_transport_stream_id tsid, t_service_id sid)
{
	if (!checkEPGFilter(onid, tsid, sid))
	{
		debug(DEBUG_INFO, "Add EPGFilter for onid=\"%04x\" tsid=\"%04x\" service_id=\"%04x\"", onid, tsid, sid);
		EPGFilter *node = new EPGFilter;
		node->onid = onid;
		node->tsid = tsid;
		node->sid = sid;
		node->next = CurrentEPGFilter;
		CurrentEPGFilter = node;
	}
}

void clearEPGFilter()
{
	EPGFilter *filterptr = CurrentEPGFilter;
	while (filterptr)
	{
		EPGFilter *filternext = filterptr->next;
		delete filterptr;
		filterptr = filternext;
	}
	CurrentEPGFilter = NULL;
}

static void addBlacklist(t_original_network_id onid, t_transport_stream_id tsid, t_service_id sid)
{
	t_channel_id channel_id =
		CREATE_CHANNEL_ID(sid, onid, tsid);
	t_channel_id mask =
		CREATE_CHANNEL_ID(
				(sid ? 0xFFFF : 0), (onid ? 0xFFFF : 0), (tsid ? 0xFFFF : 0)
				);
	if (!checkBlacklist(channel_id))
	{
		debug(DEBUG_ERROR, "Add Channel Blacklist for channel 0x%012" PRIx64 ", mask 0x%012" PRIx64, channel_id, mask);
		ChannelBlacklist *node = new ChannelBlacklist;
		node->chan = channel_id;
		node->mask = mask;
		node->next = CurrentBlacklist;
		CurrentBlacklist = node;
	}
}

static void addNoDVBTimelist(t_original_network_id onid, t_transport_stream_id tsid, t_service_id sid)
{
	t_channel_id channel_id =
		CREATE_CHANNEL_ID(sid, onid, tsid);
	t_channel_id mask =
		CREATE_CHANNEL_ID(
				(sid ? 0xFFFF : 0), (onid ? 0xFFFF : 0), (tsid ? 0xFFFF : 0)
				);
	if (!checkNoDVBTimelist(channel_id))
	{
		debug(DEBUG_ERROR, "Add channel 0x%012" PRIx64 ", mask 0x%012" PRIx64 " to NoDVBTimelist", channel_id, mask);
		ChannelNoDVBTimelist *node = new ChannelNoDVBTimelist;
		node->chan = channel_id;
		node->mask = mask;
		node->next = CurrentNoDVBTime;
		CurrentNoDVBTime = node;
	}
}

bool readEPGFilter(void)
{
	xmlDocPtr filter_parser = parseXmlFile(epg_filter_dir.c_str());

	t_original_network_id onid = 0;
	t_transport_stream_id tsid = 0;
	t_service_id sid = 0;

	if (filter_parser != NULL)
	{
		debug(DEBUG_INFO, "Reading EPGFilters");

		xmlNodePtr filter = xmlDocGetRootElement(filter_parser);
		if (xmlGetNumericAttribute(filter, "is_whitelist", 10) == 1)
			epg_filter_is_whitelist = true;
		if (xmlGetNumericAttribute(filter, "except_current_next", 10) == 1)
			epg_filter_except_current_next = true;
		filter = xmlChildrenNode(filter);

		while (filter) {

			onid = xmlGetNumericAttribute(filter, "onid", 16);
			tsid = xmlGetNumericAttribute(filter, "tsid", 16);
			sid  = xmlGetNumericAttribute(filter, "serviceID", 16);
			if (xmlGetNumericAttribute(filter, "blacklist", 10) == 1)
				addBlacklist(onid, tsid, sid);
			else
				addEPGFilter(onid, tsid, sid);

			filter = xmlNextNode(filter);
		}
	}
	xmlFreeDoc(filter_parser);
	return (CurrentEPGFilter != NULL);
}

void readDVBTimeFilter(void)
{
	xmlDocPtr filter_parser = parseXmlFile(dvbtime_filter_dir.c_str());

	t_original_network_id onid = 0;
	t_transport_stream_id tsid = 0;
	t_service_id sid = 0;

	if (filter_parser != NULL)
	{
		debug(DEBUG_INFO, "Reading DVBTimeFilters");

		xmlNodePtr filter = xmlDocGetRootElement(filter_parser);
		filter = xmlChildrenNode(filter);

		while (filter) {

			onid = xmlGetNumericAttribute(filter, "onid", 16);
			tsid = xmlGetNumericAttribute(filter, "tsid", 16);
			sid  = xmlGetNumericAttribute(filter, "serviceID", 16);
			addNoDVBTimelist(onid, tsid, sid);

			filter = xmlNextNode(filter);
		}
		xmlFreeDoc(filter_parser);
	}
	else
	{
		dvb_time_update = true;
	}
}

/* removes the files of the old index that the new save did not write again */
static void deleteOldfileEvents(const char *epgdir, const std::set<std::string> &keep)
{
	std::string indexname = std::string(epgdir) + "/index.xml";
	xmlDocPtr filter_parser = parseXmlFile(indexname.c_str());

	if (filter_parser != NULL)
	{
		xmlNodePtr filter = xmlDocGetRootElement(filter_parser);
		filter = xmlChildrenNode(filter);

		for (; filter; filter = xmlNextNode(filter)) {
			const char * name = xmlGetAttribute(filter, "name");
			if (name && !keep.count(name))
				unlink((std::string(epgdir) + "/" + name).c_str());
		}
		xmlFreeDoc(filter_parser);
	}
}

bool readEventsFromFile(std::string &epgname, int &ev_count)
{
	xmlDocPtr event_parser = NULL;
	xmlNodePtr service;
	xmlNodePtr event;
	t_original_network_id onid = 0;
	t_transport_stream_id tsid = 0;
	t_service_id sid = 0;

	if (!(event_parser = parseXmlFile(epgname.c_str()))) {
		debug(DEBUG_INFO, "unable to open %s for reading", epgname.c_str());
		return false;
	}
	service = xmlDocGetRootElement(event_parser);
	service = xmlChildrenNode(service);

	while (service) {
		onid = xmlGetNumericAttribute(service, "original_network_id", 16);
		tsid = xmlGetNumericAttribute(service, "transport_stream_id", 16);
		sid = xmlGetNumericAttribute(service, "service_id", 16);
		if(!onid || !tsid || !sid){
			service = xmlNextNode(service);
			continue;
		}
		event = xmlChildrenNode(service);

		while (event) {
			SIevent e(onid,tsid,sid,xmlGetNumericAttribute(event, "id", 16));
			uint8_t tid = xmlGetNumericAttribute(event, "tid", 16);
			std::string contentClassification, userClassification;
			if(tid)
				e.table_id = tid;
			e.table_id |= 0x80; /* make sure on-air data has a lower table_id */

			xmlNodePtr node;

			node = xmlChildrenNode(event);
			while ((node = xmlGetNextOccurence(node, "name"))) {
				const char *s = xmlGetAttribute(node, "string");
				if (s)
					e.setName(ZapitTools::UTF8_to_Latin1(xmlGetAttribute(node, "lang")), s);
				node = xmlNextNode(node);
			}

			node = xmlChildrenNode(event);
			while ((node = xmlGetNextOccurence(node, "text"))) {
				const char *s = xmlGetAttribute(node, "string");
				if (s)
					e.setText(ZapitTools::UTF8_to_Latin1(xmlGetAttribute(node, "lang")), s);
				node = xmlNextNode(node);
			}
			node = xmlChildrenNode(event);
			while ((node = xmlGetNextOccurence(node, "item"))) {
#ifdef USE_ITEM_DESCRIPTION
				const char *s = xmlGetAttribute(node, "string");
				if (s)
					e.item = s;
#endif
				node = xmlNextNode(node);
			}

			node = xmlChildrenNode(event);
			while ((node = xmlGetNextOccurence(node, "item_description"))) {
#ifdef USE_ITEM_DESCRIPTION
				const char *s = xmlGetAttribute(node, "string");
				if (s)
					e.itemDescription = s;
#endif
				node = xmlNextNode(node);
			}
			node = xmlChildrenNode(event);
			while ((node = xmlGetNextOccurence(node, "extended_text"))) {
				const char *l = xmlGetAttribute(node, "lang");
				const char *s = xmlGetAttribute(node, "string");
				if (l && s)
					e.appendExtendedText(ZapitTools::UTF8_to_Latin1(l), s);
				node = xmlNextNode(node);
			}

			node = xmlChildrenNode(event);
			while ((node = xmlGetNextOccurence(node, "time"))) {
				e.times.insert(SItime(xmlGetNumericAttribute(node, "start_time", 10),
							xmlGetNumericAttribute(node, "duration", 10)));
				node = xmlNextNode(node);
			}

			node = xmlChildrenNode(event);
			while ((node = xmlGetNextOccurence(node, "content"))) {
				const char cl = xmlGetNumericAttribute(node, "class", 16);
				contentClassification += cl;
				const char cl2 = xmlGetNumericAttribute(node, "user", 16);
				userClassification += cl2;
				node = xmlNextNode(node);
			}

			node = xmlChildrenNode(event);
			while ((node = xmlGetNextOccurence(node, "component"))) {
				SIcomponent c;
				c.streamContent = xmlGetNumericAttribute(node, "stream_content", 16);
				c.componentType = xmlGetNumericAttribute(node, "type", 16);
				c.componentTag = xmlGetNumericAttribute(node, "tag", 16);
				const char *s = xmlGetAttribute(node, "text");
				if (s)
					c.setComponent(s);
				//e.components.insert(c);
				e.components.push_back(c);
				node = xmlNextNode(node);
			}

			node = xmlChildrenNode(event);
			while ((node = xmlGetNextOccurence(node, "parental_rating"))) {
				const char *s = xmlGetAttribute(node, "country");
				if (s)
#if 0
					e.ratings.insert(SIparentalRating(ZapitTools::UTF8_to_Latin1(s),
								(unsigned char) xmlGetNumericAttribute(node, "rating", 10)));
#endif
				e.ratings.push_back(SIparentalRating(ZapitTools::UTF8_to_Latin1(s),
							(unsigned char) xmlGetNumericAttribute(node, "rating", 10)));
				node = xmlNextNode(node);
			}

			node = xmlChildrenNode(event);
			while ((node = xmlGetNextOccurence(node, "linkage"))) {
				SIlinkage l;
				l.linkageType = xmlGetNumericAttribute(node, "type", 16);
				l.transportStreamId = xmlGetNumericAttribute(node, "transport_stream_id", 16);
				l.originalNetworkId = xmlGetNumericAttribute(node, "original_network_id", 16);
				l.serviceId = xmlGetNumericAttribute(node, "service_id", 16);
				const char *s = xmlGetAttribute(node, "linkage_descriptor");
				if (s)
					l.name = s;
				e.linkage_descs.insert(e.linkage_descs.end(), l);
				node = xmlNextNode(node);
			}

			if (!contentClassification.empty()) {
#ifdef FULL_CONTENT_CLASSIFICATION
				ssize_t off = e.classifications.reserve(2 * contentClassification.size());
				if (off > -1)
					for (unsigned i = 0; i < contentClassification.size(); i++)
						off = e.classifications.set(off, contentClassification.at(i), userClassification.at(i));
#else
				e.classifications.content = contentClassification.at(0);
				e.classifications.user = userClassification.at(0);
#endif
			}
			addEvent(e, 0);
			ev_count++;

			event = xmlNextNode(event);
		}

		service = xmlNextNode(service);
	}
	xmlFreeDoc(event_parser);
	return true;
}

/* an XMLTV time, "20261009053000 +0200", as seconds since the epoch */
static time_t xmltvTime(const char *s)
{
	struct tm t;
	char sign = '+';
	int offset_h = 0, offset_m = 0;
	memset(&t, 0, sizeof(t));
	if (!s || sscanf(s, "%04d%02d%02d%02d%02d%02d %c%02d%02d", &t.tm_year, &t.tm_mon, &t.tm_mday,
			 &t.tm_hour, &t.tm_min, &t.tm_sec, &sign, &offset_h, &offset_m) < 6)
		return 0;
	t.tm_year -= 1900;
	t.tm_mon -= 1;
	return timegm(&t) - (sign == '-' ? -1 : 1) * (offset_h * 3600 + offset_m * 60);
}

bool readEventsFromXMLTV(std::string &epgname, int &ev_count, bool delete_after)
{
	xmlDocPtr event_parser = NULL;
	xmlNodePtr tv;
	xmlNodePtr programme;
	t_original_network_id onid = 0;
	t_transport_stream_id tsid = 0;
	t_service_id sid = 0;
	std::map<std::string,t_channel_id> cache_epgid;

	if (!(event_parser = parseXmlFile(epgname.c_str())))
	{
		debug(DEBUG_NORMAL, "unable to open %s for reading", epgname.c_str());
		if (delete_after)
		{
			if (unlink(epgname.c_str()))
				printf("Failed to delete file: %s\n", epgname.c_str());
		}
		return false;
	}

	tv = xmlDocGetRootElement(event_parser);
	programme = xmlChildrenNode(tv);

	while ((programme = xmlGetNextOccurence(programme,"programme")))
	{
		const char *chan = xmlGetAttribute(programme, "channel");
		const char *start = xmlGetAttribute(programme, "start");
		const char *stop  = xmlGetAttribute(programme, "stop");

		/* XMLTV gives the time of day with its offset to UTC */
		time_t start_time = xmltvTime(start);
		time_t duration = xmltvTime(stop) - start_time;

		t_channel_id epgid = 0;
		time_t current_time;
		time(&current_time);
		double time_diff = difftime(current_time, start_time + duration);

		// just loads events if they end is in the future
		if (time_diff < 0)
		{
			std::string tchan = chan;
			if (cache_epgid[tchan] != 0)
			{
				epgid = cache_epgid[chan];
			}
			if (epgid == 0)
			{
				epgid = getepgid(chan);
				if (epgid == 0)
					epgid = 1;//skip check channels not found in channellist
				cache_epgid[tchan] = epgid;
			}

		}
		if (epgid != 0 && epgid != 1)
		{
			//debug(DEBUG_NORMAL, "\e[1;34m%s - %d - %s 0x%012" PRIx64 "(%ld) (%ld)\e[0m",__func__, __LINE__,chan, epgid, start_time, duration);
			onid = GET_ORIGINAL_NETWORK_ID_FROM_CHANNEL_ID(epgid);
			tsid = GET_TRANSPORT_STREAM_ID_FROM_CHANNEL_ID(epgid);
			sid  = GET_SERVICE_ID_FROM_CHANNEL_ID(epgid);

			SIevent e(onid, tsid, sid, ev_count+0x8000);
			e.table_id = 0x50;
			e.times.insert(SItime(start_time, duration));
			xmlNodePtr node;
			node = xmlChildrenNode(programme);
			while ((node = xmlGetNextOccurence(node, "title")))
			{
				const char *title = xmlGetData(node);
				if(title != NULL)
					e.setName(std::string(ZapitTools::UTF8_to_Latin1("deu")), std::string(title));
				node = xmlNextNode(node);
			}
			node = xmlChildrenNode(programme);
			while ((node = xmlGetNextOccurence(node, "sub-title")))
			{
				const char *subtitle = xmlGetData(node);
				if(subtitle != NULL)
					e.setText(std::string(ZapitTools::UTF8_to_Latin1("deu")), std::string(subtitle));
				node = xmlNextNode(node);
			}
			node = xmlChildrenNode(programme);
			int cnt = 1;
			while ((node = xmlGetNextOccurence(node, "desc")))
			{
				const char *description = xmlGetData(node);
				if (description != NULL)
				{
					std::string desc = std::string(description);
					std::string nline = "";
					if (cnt > 1)
						nline = '\n';
					e.appendExtendedText(std::string(ZapitTools::UTF8_to_Latin1("deu")), nline + desc);
					cnt++;
				}
				node = xmlNextNode(node);
			}
			debug(DEBUG_INFO, "XML DEBUG: %s channel 0x%012" PRIx64, chan, epgid);

			addEvent(e, 0);

			ev_count++;
		}
		programme = xmlNextNode(programme);
	}

	xmlFreeDoc(event_parser);

	if (delete_after)
	{
		if (unlink(epgname.c_str()))
			printf("Failed to delete file: %s\n", epgname.c_str());
	}

	return true;
}

t_channel_id getepgid(std::string epg_name)
{
	t_channel_id epgid = 0;
	bool match_found = false;

	CBouquetManager::ChannelIterator cit = g_bouquetManager->tvChannelsBegin();

	for (int m = CZapitClient::MODE_TV; m < CZapitClient::MODE_ALL; m++)
	{
		if (m == CZapitClient::MODE_RADIO)
			cit = g_bouquetManager->radioChannelsBegin();

		for (; g_bouquetManager->empty || !(cit.EndOfChannels()); cit++)
		{
			if(g_bouquetManager->empty)
				break;

			std::string tvg_id = (*cit)->getEPGmap();
			if (tvg_id.empty())
				continue;

			std::size_t found = tvg_id.find("#"+epg_name);
			if (found != std::string::npos)
			{
				if (match_found)
				{
					if(g_bouquetManager->empty)
						break;
					if ((*cit)->getEpgID() == epgid) continue;
					(*cit)->setEPGid(epgid);
				}
				else
				{
					tvg_id = tvg_id.substr(tvg_id.find_first_of("="));
					sscanf(tvg_id.c_str(), "=%" SCNx64, &epgid);
					match_found = true;
				}
			}
			else
				continue;
		}
	}

	return epgid;
}

static int my_filter(const struct dirent *entry)
{
	int len = strlen(entry->d_name);
	if (len > 3 && entry->d_name[len-3] == 'x' && entry->d_name[len-2] == 'm' && entry->d_name[len-1] == 'l')
		return 1;
	return 0;
}

bool readEventsFromDir(std::string &epgdir, int &ev_count)
{
	struct dirent **namelist;
	int n = scandir(epgdir.c_str(), &namelist, my_filter, NULL);
	debug(DEBUG_NORMAL, "Reading data from directory %s, file count %d", epgdir.c_str(), n);
	if (n <= 0)
		return false;

	for (int i = 0; i < n; i++) {
		std::string epgname = epgdir + namelist[i]->d_name;
		readEventsFromFile(epgname, ev_count);
		free(namelist[i]);
	}
	free(namelist);
	return true;
}

/* startXMLReader() has counted this reader already */
void *insertEventsfromFile(void * data)
{
	set_threadname(__func__);
	std::string indexname;
	std::string filename;
	std::string epgname;
	xmlNodePtr eventfile;
	int ev_count = 0;
	if (!data) {
		readerFinished();
		pthread_exit(NULL);
	}
	std::string epg_dir = (char *) data;
	free(data); /* heap copy made by the caller (sectionsd.cpp startXMLReader) */
	indexname = epg_dir + "index.xml";

	int64_t now = time_monotonic_ms();
	xmlDocPtr index_parser = parseXmlFile(indexname.c_str());

	if (index_parser == NULL) {
		readEventsFromDir(epg_dir, ev_count);
		debug(DEBUG_NORMAL, "Reading data finished after %" PRId64 " ms (%d events) from %s",
				time_monotonic_ms()-now, ev_count, epg_dir.c_str());
		readerFinished();
		pthread_exit(NULL);
	}
	debug(DEBUG_NORMAL, "Reading data from file: %s", indexname.c_str());

	eventfile = xmlDocGetRootElement(index_parser);
	eventfile = xmlChildrenNode(eventfile);

	while (eventfile) {
		const char * name = xmlGetAttribute(eventfile, "name");
		if(name)
			filename=name;

		epgname = epg_dir + filename;
		readEventsFromFile(epgname, ev_count);

		debug(DEBUG_DEBUG, "Reading data finished after %" PRId64 " ms (%d events) from %s",
			time_monotonic_ms()-now, ev_count, epgname.c_str());

		eventfile = xmlNextNode(eventfile);
	}

	xmlFreeDoc(index_parser);

	readerFinished();

	pthread_exit(NULL);
}

#ifdef ENABLE_XMLTV_XZ
/* the XML readers know gzip only, so a file that is packed with xz is unpacked into a file first */
static bool unpackXz(const std::string &from, const std::string &to)
{
	FILE *in = fopen(from.c_str(), "rb");
	FILE *out = in ? fopen(to.c_str(), "wb") : NULL;
	lzma_stream strm = LZMA_STREAM_INIT;
	bool ok = out && lzma_stream_decoder(&strm, UINT64_MAX, LZMA_CONCATENATED) == LZMA_OK;
	if (ok)
	{
		uint8_t inbuf[32 * 1024], outbuf[32 * 1024];
		lzma_action action = LZMA_RUN;
		lzma_ret ret = LZMA_OK;
		strm.next_out = outbuf;
		strm.avail_out = sizeof(outbuf);
		while (ret == LZMA_OK)
		{
			if (strm.avail_in == 0 && action == LZMA_RUN)
			{
				strm.next_in = inbuf;
				strm.avail_in = fread(inbuf, 1, sizeof(inbuf), in);
				if (ferror(in))
					break;
				if (feof(in))
					action = LZMA_FINISH;
			}
			ret = lzma_code(&strm, action);
			if (strm.avail_out == 0 || ret == LZMA_STREAM_END)
			{
				size_t len = sizeof(outbuf) - strm.avail_out;
				if (fwrite(outbuf, 1, len, out) != len)
					break;
				strm.next_out = outbuf;
				strm.avail_out = sizeof(outbuf);
			}
		}
		ok = ret == LZMA_STREAM_END;
		lzma_end(&strm);
	}
	if (in)
		fclose(in);
	if (out && fclose(out))
		ok = false;
	if (!ok)
		unlink(to.c_str());
	return ok;
}
#endif

/* reads the file as it is, or what is in it when it is packed with xz */
static void readEventsFromXMLTVFile(std::string &name, int &ev_count, bool delete_after)
{
	if (getFileExt(name) != "xz")
	{
		readEventsFromXMLTV(name, ev_count, delete_after);
		return;
	}
#ifdef ENABLE_XMLTV_XZ
	std::string xml_name = randomFile("xml", "/tmp", 8);
	bool unpacked = unpackXz(name, xml_name);
	if (delete_after)
		unlink(name.c_str());
	if (unpacked)
		readEventsFromXMLTV(xml_name, ev_count, true);
	else
		printf("[sectionsd] unable to unpack %s\n", name.c_str());
#else
	printf("[sectionsd] %s: built without --enable-xmltv-xz\n", name.c_str());
	if (delete_after)
		unlink(name.c_str());
#endif
}

/* the addresses whose kept download has been read since the start */
static std::set<std::string> xmltv_cache_read;
static pthread_mutex_t xmltv_cache_lock = PTHREAD_MUTEX_INITIALIZER;

#define XMLTV_NAME_SERVER_WAIT_S 60

/*
	Right after the start no name server may answer yet. Wait while that
	is so, asking every second, and say whether the name is known now; a
	name that does not exist is not waited for.
*/
static bool waitForXMLTVNameServer(const std::string &url)
{
	std::string::size_type begin = url.find("://");
	if (begin == std::string::npos)
		return false;
	begin += 3;
	std::string host = url.substr(begin, url.find_first_of("/?#", begin) - begin);
	std::string::size_type at = host.rfind('@'); /* user:pass@host */
	if (at != std::string::npos)
		host.erase(0, at + 1);
	std::string::size_type colon = host.find(':');
	if (colon != std::string::npos)
		host.erase(colon);
	if (host.empty() || host[0] == '[')
		return false;

	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	const int64_t start = time_monotonic_ms();
	bool waited_for_ns = false; /* true only if the name server was not there at first */
	for (;;)
	{
		struct addrinfo *result = NULL;
		int ret = getaddrinfo(host.c_str(), NULL, &hints, &result);
		if (result)
			freeaddrinfo(result);
		int waited = (time_monotonic_ms() - start) / 1000;
		if (ret != EAI_AGAIN)
		{
			if (waited > 0)
				debug(DEBUG_NORMAL, "Waited %d s for a name server for %s", waited, host.c_str());
			/* a download that failed although the name resolved at once is not retried */
			return ret == 0 && waited_for_ns;
		}
		waited_for_ns = true;
		if (waited >= XMLTV_NAME_SERVER_WAIT_S)
		{
			debug(DEBUG_NORMAL, "No name server for %s after %d s", host.c_str(), waited);
			return false;
		}
		if (sectionsd_stop)
			return false;
		sleep(1);
	}
}

void readEventsFromXMLTVUrl(std::string url)
{
	readerStarted();
	int ev_count = 0;
	std::string tmp_name = randomFile(getFileExt(url), "/tmp", 8);

	int64_t now = time_monotonic_ms();

	if (url.compare(0, 1, "/") == 0)
	{
		readEventsFromXMLTVFile(url, ev_count, false);
	}
	else
	{
		/*
			The programme guide is there at once from the download
			that was kept, also without a network. What comes from
			the network afterwards is only read when it has changed.
		*/
		std::string cache_name = downloadCacheFile(url);
		if (!cache_name.empty() && !access(cache_name.c_str(), R_OK))
		{
			pthread_mutex_lock(&xmltv_cache_lock);
			bool first = xmltv_cache_read.insert(url).second;
			pthread_mutex_unlock(&xmltv_cache_lock);
			if (first)
			{
				readEventsFromXMLTVFile(cache_name, ev_count, false);
				debug(DEBUG_NORMAL, "Read %d events kept from %s", ev_count, url.c_str());
			}
		}
		if (!::downloadUrl(url, tmp_name) &&
		    !(waitForXMLTVNameServer(url) && ::downloadUrl(url, tmp_name)))
		{
			unlink(tmp_name.c_str());
			readerFinished();
			return;
		}
		if (!access(tmp_name.c_str(), R_OK))
		{
			if (cache_name.empty() || updateDownloadCache(tmp_name, cache_name))
			{
				pthread_mutex_lock(&xmltv_cache_lock);
				xmltv_cache_read.insert(url);
				pthread_mutex_unlock(&xmltv_cache_lock);
				readEventsFromXMLTVFile(tmp_name, ev_count, true);
			}
			else
				unlink(tmp_name.c_str());
		}
	}

	debug(DEBUG_NORMAL, "Reading data finished after %" PRId64 " ms (%d events) from %s",
	       time_monotonic_ms()-now, ev_count, url.c_str());

	readerFinished();
}

static void write_epg_xml_header(FILE * fd, const t_original_network_id onid, const t_transport_stream_id tsid, const t_service_id sid)
{
	fprintf(fd,
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<!--\n"
		"  This file was automatically generated by the sectionsd.\n"
		"  It contains all event entries which have been cached\n"
		"  at time the box was shut down.\n"
		"-->\n"
		"<dvbepg>\n");
	fprintf(fd,"\t<service original_network_id=\"%04x\" transport_stream_id=\"%04x\" service_id=\"%04x\">\n",onid,tsid,sid);
}

static void write_index_xml_header(FILE * fd)
{
	fprintf(fd,
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<!--\n"
		"  This file was automatically generated by the sectionsd.\n"
		"  It contains all event entries which have been cached\n"
		"  at time the box was shut down.\n"
		"-->\n"
		"<dvbepgfiles>\n");
}

static void write_epgxml_footer(FILE *fd)
{
	fprintf(fd, "\t</service>\n");
	fprintf(fd, "</dvbepg>\n");
}

static void write_indexxml_footer(FILE *fd)
{
	fprintf(fd, "</dvbepgfiles>\n");
}

void writeEventsToFile(const char *epgdir)
{
	if(check_dir(epgdir)){
		return;
	}

	FILE * indexfile = NULL;
	FILE * eventfile = NULL;
	std::string filename("");
	std::string tmpname("");
	char eventname[17] = "";
	t_original_network_id onid = 0;
	t_transport_stream_id tsid = 0;
	t_service_id sid = 0;
	/* every file is written under a temporary name and the old save stays
	   whole until the new one is: a save cut short keeps the old EPG */
	std::set<std::string> written;

	tmpname  = (std::string)epgdir + "/index.tmp";

	if (!(indexfile = fopen(tmpname.c_str(), "w"))) {
		debug(DEBUG_NORMAL, "unable to open %s for writing", tmpname.c_str());
		return;
	}

	debug(DEBUG_NORMAL, "Writing Information to file: %s", tmpname.c_str());

	write_index_xml_header(indexfile);

	readLockEvents();

	MySIeventsOrderServiceUniqueKeyFirstStartTimeEventUniqueKey::iterator e =
		mySIeventsOrderServiceUniqueKeyFirstStartTimeEventUniqueKey.begin();
	for (; e != mySIeventsOrderServiceUniqueKeyFirstStartTimeEventUniqueKey.end(); ++e) {
		if ( (onid != (*e)->original_network_id) || (tsid != (*e)->transport_stream_id) || (sid != (*e)->service_id) ) {
			if (eventfile != NULL) {
				write_epgxml_footer(eventfile);
				fclose(eventfile);
			}
			onid = (*e)->original_network_id;
			tsid = (*e)->transport_stream_id;
			sid = (*e)->service_id;
			snprintf(eventname, 17, "%04x%04x%04x.xml", tsid, onid, sid);
			filename  = (std::string)epgdir + "/" + (std::string)eventname + ".tmp";
			if (!(eventfile = fopen(filename.c_str(), "w"))) {
				goto _done;
			}
			written.insert(eventname);
			fprintf(indexfile, "\t<eventfile name=\"%s\"/>\n", eventname);
			write_epg_xml_header(eventfile, onid, tsid, sid);
		}
		(*e)->saveXML(eventfile);
	}
	if (eventfile != NULL) {
		write_epgxml_footer(eventfile);
		fclose(eventfile);
	}
_done:
	unlockEvents();
	write_indexxml_footer(indexfile);
	fclose(indexfile);

	for (std::set<std::string>::iterator it = written.begin(); it != written.end(); ++it) {
		filename = (std::string)epgdir + "/" + *it;
		rename((filename + ".tmp").c_str(), filename.c_str());
	}
	deleteOldfileEvents(epgdir, written);

	filename  = (std::string)epgdir + "/index.xml";
	rename(tmpname.c_str(), filename.c_str());

	// sync();
	debug(DEBUG_NORMAL, "Writing Information finished");
	return ;
}
