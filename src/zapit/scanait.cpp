/*
 * Copyright (C) 2011 CoolStream International Ltd
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

#include "debug.h"
#include "getservices.h"
#include "scanait.h"
#include <hardware/dmx.h>
#include <dvbsi++/descriptor_tag.h>
#include <dvbsi++/linkage_descriptor.h>
#include <dvbsi++/private_data_specifier_descriptor.h>
#include <dvbsi++/transport_protocol_descriptor.h>
#include <dvbsi++/simple_application_location_descriptor.h>
#include <dvbsi++/simple_application_boundary_descriptor.h>
#include <algorithm>
#include <cstring>
#include <eitd/edvbstring.h>
#include <system/set_threadname.h>

/* sections are read in slices so Stop() never waits long for a zap */
#define AIT_READ_SLICE_MS 200
#define AIT_READ_TIMEOUT_MS 3000

CAit::CAit()
{
	dmxnum = 0;
	pid = 0;
	running = false;
	channel_id = 0;
	apps_channel = 0;
}

void CAit::clearSections()
{
	ApplicationInformationSectionList::iterator it;
	for (it = sections.begin(); it != sections.end(); ++it)
		delete *it;

	sections.clear();
}

void CAit::setDemux(int dnum)
{
	dmxnum = dnum;
}

bool CAit::Start()
{
	if (running)
		return false;
	running = true;

	int ret = start();
	if (ret != 0)
		running = false;
	return (ret == 0);
}

bool CAit::Stop()
{
	if (!running)
		return false;
	running = false;

	int ret = join();
	return (ret == 0);
}

void CAit::run()
{
	set_threadname("zap:ait");
	if (Parse())
		printf("[scan] AIT finished.\n");
	else
		printf("[scan] AIT failed !\n");
}

CAit::~CAit()
{
	Stop();
	clearSections();
}

/* all sections of one AIT version, section 0 to last_section_number */
bool CAit::Read()
{
	cDemux *dmx = new cDemux(dmxnum);
	dmx->Open(DMX_PSI_CHANNEL);

	unsigned char buffer[AIT_SECTION_SIZE];
	unsigned char filter[DMX_FILTER_SIZE];
	unsigned char mask[DMX_FILTER_SIZE];

	memset(filter, 0x00, DMX_FILTER_SIZE);
	memset(mask, 0x00, DMX_FILTER_SIZE);
	filter[0] = 0x74;
	mask[0] = 0xFF;

	bool ok = dmx->sectionFilter(pid, filter, mask, 1);
	std::vector<bool> seen;
	int version = -1;
	int waited = 0;
	while (ok && running && waited < AIT_READ_TIMEOUT_MS)
	{
		int len = dmx->Read(buffer, AIT_SECTION_SIZE, AIT_READ_SLICE_MS);
		if (len < 0)
			break;
		if (len == 0)
		{
			waited += AIT_READ_SLICE_MS;
			continue;
		}
		ApplicationInformationSection *ait = new ApplicationInformationSection(buffer);
		if (version != ait->getVersionNumber())
		{
			clearSections();
			version = ait->getVersionNumber();
			seen.assign(ait->getLastSectionNumber() + 1, false);
		}
		unsigned number = ait->getSectionNumber();
		if (number >= seen.size() || seen[number])
		{
			delete ait;
			continue;
		}
		seen[number] = true;
		sections.push_back(ait);
		if (std::find(seen.begin(), seen.end(), false) == seen.end())
			break;
	}
	dmx->Stop();
	delete dmx;

	if (sections.empty())
		printf("CAit::Read: pid %x failed\n", pid);
	else
		printf("CAit::Read: pid %x, %u section(s)\n", pid, (unsigned) sections.size());
	return !sections.empty();
}

bool CAit::Parse()
{
	printf("[ait] trying to parse AIT\n");
	clearSections();

	if (!Read())
		return false;

	std::vector<CAitApp> found;
	short int profilecode = 0;
	int orgid = 0, appid = 0, profileVersion = 0;

	int sectionLength = 0;
	ApplicationInformationSectionConstIterator sit;
	for (sit = sections.begin(); sit != sections.end() && running; ++sit)
	{
		std::list<ApplicationInformation *>::const_iterator i = (*sit)->getApplicationInformation()->begin();
		sectionLength = (*sit)->getSectionLength() + 3;

		for (; i != (*sit)->getApplicationInformation()->end(); ++i)
		{
			std::string hbbtvUrl = "", applicationName = "";
			std::string TPDescPath = "", SALDescPath = "";
			int controlCode = (*i)->getApplicationControlCode();
			ApplicationIdentifier *applicationIdentifier = (ApplicationIdentifier *)(*i)->getApplicationIdentifier();
			profilecode = 0;
			orgid = applicationIdentifier->getOrganisationId();
			appid = applicationIdentifier->getApplicationId();
			if (controlCode == CAitApp::AUTOSTART || controlCode == CAitApp::PRESENT)
			{
				for (DescriptorConstIterator desc = (*i)->getDescriptors()->begin();
					desc != (*i)->getDescriptors()->end(); ++desc)
				{
					switch ((*desc)->getTag())
					{
						case APPLICATION_DESCRIPTOR:
						{
							ApplicationDescriptor *applicationDescriptor = (ApplicationDescriptor *)(*desc);
							const ApplicationProfileList *applicationProfiles = applicationDescriptor->getApplicationProfiles();
							ApplicationProfileConstIterator interactionit = applicationProfiles->begin();
							for (; interactionit != applicationProfiles->end(); ++interactionit)
							{
								profilecode = (*interactionit)->getApplicationProfile();
								profileVersion = PACK_VERSION(
										(*interactionit)->getVersionMajor(),
										(*interactionit)->getVersionMinor(),
										(*interactionit)->getVersionMicro()
									);
							}
							break;
						}
						case APPLICATION_NAME_DESCRIPTOR:
						{
							ApplicationNameDescriptor *nameDescriptor = (ApplicationNameDescriptor *)(*desc);
							ApplicationNameConstIterator interactionit = nameDescriptor->getApplicationNames()->begin();
							if (interactionit != nameDescriptor->getApplicationNames()->end())
								applicationName = (*interactionit)->getApplicationName();
							break;
						}
						case TRANSPORT_PROTOCOL_DESCRIPTOR:
						{
							TransportProtocolDescriptor *transport = (TransportProtocolDescriptor *)(*desc);
							if (transport->getProtocolId() == 3) /* interaction channel; carousel and ip are not used */
							{
								InterActionTransportConstIterator interactionit = transport->getInteractionTransports()->begin();
								if (interactionit != transport->getInteractionTransports()->end())
									TPDescPath = (*interactionit)->getUrlBase()->getUrl();
							}
							break;
						}
						case SIMPLE_APPLICATION_LOCATION_DESCRIPTOR:
						{
							SimpleApplicationLocationDescriptor *applicationlocation = (SimpleApplicationLocationDescriptor *)(*desc);
							SALDescPath = applicationlocation->getInitialPath();
							break;
						}
					}
				}
				hbbtvUrl = TPDescPath + SALDescPath;
			}
			if (!hbbtvUrl.empty())
			{
				printf("[ait] detected AppID: %d, AppName: %s, Url: %s profilecode: %i orgid: %i sectionLength: %i profileVersion %i control: %d\n", appid, applicationName.c_str(), hbbtvUrl.c_str(), profilecode, orgid, sectionLength, profileVersion, controlCode);
				CAitApp app = { appid, orgid, controlCode, applicationName, hbbtvUrl };
				found.push_back(app);
			}
		}
	}
	clearSections();
	if (!running)
		return false;

	OpenThreads::ScopedLock<OpenThreads::Mutex> lock(apps_mutex);
	apps_channel = channel_id;
	apps.swap(found);
	return true;
}

bool CAit::Parse(CZapitChannel *const channel)
{
	Stop();
	{
		OpenThreads::ScopedLock<OpenThreads::Mutex> lock(apps_mutex);
		apps_channel = 0;
		apps.clear();
	}
	pid = channel->getAitPid();
	name = channel->getName();
	channel_id = channel->getChannelID();
	if (pid > 0)
		return Start();
	return false;
}

bool CAit::getApps(t_channel_id channel, std::vector<CAitApp> &out)
{
	OpenThreads::ScopedLock<OpenThreads::Mutex> lock(apps_mutex);
	if (!channel || channel != apps_channel)
		return false;
	out = apps;
	return true;
}
