
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <global.h>
#include <neutrino.h>

#include <driver/framebuffer.h>

#include <gui/infoclock.h>
#include <gui/infoviewer.h>
#include <gui/timeosd.h>
#include <gui/volumebar.h>
#include <gui/osd_helpers.h>
#include <gui/radiotext_window.h>

#include <hardware/video.h>

extern CInfoClock *InfoClock;
extern CTimeOSD *FileTimeOSD;
extern cVideo *videoDecoder;

COsdHelpers::COsdHelpers()
{
}

COsdHelpers::~COsdHelpers()
{
}

COsdHelpers* COsdHelpers::getInstance()
{
	static COsdHelpers* osdh = NULL;
	if(!osdh)
		osdh = new COsdHelpers();

	return osdh;
}

/* lines of the picture the video system puts out */
static int videoSystemHeight(int vs)
{
	switch (vs) {
	case VIDEO_STD_720P50:
	case VIDEO_STD_720P60:
		return 720;
	case VIDEO_STD_1080I50:
	case VIDEO_STD_1080I60:
	case VIDEO_STD_1080P24:
	case VIDEO_STD_1080P25:
	case VIDEO_STD_1080P30:
	case VIDEO_STD_1080P50:
	case VIDEO_STD_1080P60:
	case VIDEO_STD_1080P2397:
	case VIDEO_STD_1080P2997:
		return 1080;
	case VIDEO_STD_2160P24:
	case VIDEO_STD_2160P25:
	case VIDEO_STD_2160P30:
	case VIDEO_STD_2160P50:
		return 2160;
	default:
		return 576;
	}
}

void COsdHelpers::getOutputSize(int &w, int &h)
{
	unsigned generation;
	if (CFrameBuffer::getInstance()->getOutputSize(w, h, generation))
		return;
	/* a box: the hardware scales the OSD plane to what the video system puts out */
	h = videoSystemHeight(getVideoSystem());
	if (h > 720 && !g_info.hw_caps->can_osd_1080)
		h = 720;
	w = h * 16 / 9;
}

bool COsdHelpers::applyOsdSize(bool force/*=false*/)
{
	CFrameBuffer *frameBuffer = CFrameBuffer::getInstance();
	int out_w, out_h, w, h;
	getOutputSize(out_w, out_h);
	frameBuffer->osdSizeFor(out_w, out_h, w, h);
	if (!force && w == (int) frameBuffer->getScreenWidth(true) && h == (int) frameBuffer->getScreenHeight(true))
		return false;
	printf("[osd] output %dx%d, OSD %dx%d\n", out_w, out_h, w, h);

	OnBeforeChangeResolution();

	bool ivVisible = false;
	if (g_InfoViewer && g_InfoViewer->is_visible) {
		g_InfoViewer->killTitle();
		ivVisible = true;
	}

	if (frameBuffer->setMode(w, h, 8 * sizeof(fb_pixel_t)) == 0) {
		if (InfoClock)
			InfoClock->StopInfoClock(true);
		frameBuffer->Clear();
		frameBuffer->clearIconCache();
		CNeutrinoApp::getInstance()->setScreenSettings();
		CNeutrinoApp::getInstance()->SetupFonts(CNeutrinoFonts::FONTSETUP_NEUTRINO_FONT);
		CVolumeHelper::getInstance()->refresh();
		if (InfoClock)
			CInfoClock::getInstance()->ClearDisplay();
		if (FileTimeOSD)
			FileTimeOSD->Init();
		if (CNeutrinoApp::getInstance()->channelList)
			CNeutrinoApp::getInstance()->channelList->ResetModules();
		if (InfoClock)
			InfoClock->enableInfoClock();
	}
	if (g_InfoViewer) {
		g_InfoViewer->ResetModules();
		g_InfoViewer->start();
	}
	if (ivVisible) {
		CNeutrinoApp::getInstance()->StopSubtitles();
		g_InfoViewer->showTitle(CNeutrinoApp::getInstance()->channelList->getActiveChannel(), true, 0, true);
		CNeutrinoApp::getInstance()->StartSubtitles();
	}
	if (g_RadiotextWin) {
		delete g_RadiotextWin;
		g_RadiotextWin = new CRadioTextGUI();
		g_RadiotextWin->allowPaint(false);
	}
	OnAfterChangeResolution();
	return true;
}

int COsdHelpers::isVideoSystem1080(int res)
{
	if ((res == VIDEO_STD_1080I60) ||
	    (res == VIDEO_STD_1080I50) ||
	    (res == VIDEO_STD_1080P30) ||
	    (res == VIDEO_STD_1080P24) ||
	    (res == VIDEO_STD_1080P25))
		return true;

	if (g_info.hw_caps->can_osd_1080)
	{
		if ((res == VIDEO_STD_1080P50) ||
		    (res == VIDEO_STD_1080P60))
			return true;
#if HAVE_CST_HARDWARE
		if ((res == VIDEO_STD_1080P2397) ||
		    (res == VIDEO_STD_1080P2997))
			return true;
#else
		if ((res == VIDEO_STD_2160P24) ||
		    (res == VIDEO_STD_2160P25) ||
		    (res == VIDEO_STD_2160P30) ||
		    (res == VIDEO_STD_2160P50))
			return true;
#endif
	}

#if 0
	/* for testing only */
	if (res == VIDEO_STD_720P50)
		return true;
#endif

	return false;
}

int COsdHelpers::getVideoSystem()
{
	return videoDecoder->GetVideoSystem();
}

#define DEBUGINFO_SETVIDEOSYSTEM

int COsdHelpers::setVideoSystem(int newSystem, bool remember/* = true*/)
{
	if ((newSystem < 0) || (newSystem > VIDEO_STD_MAX))
		return -1;

	if (newSystem == getVideoSystem())
		return 0;

#ifdef DEBUGINFO_SETVIDEOSYSTEM
	int fd = CFrameBuffer::getInstance()->getFileHandle();
	fb_var_screeninfo var;
	fb_fix_screeninfo fix;

	ioctl(fd, FBIOGET_VSCREENINFO, &var);
	ioctl(fd, FBIOGET_FSCREENINFO, &fix);
	printf(">>>>>[%s - %s:%d] before SetVideoSystem:\n"
				"                var.xres        : %4d, var.yres    : %4d, var.yres_virtual: %4d\n"
				"                fix.line_length : %4d, fix.smem_len: %d Byte\n",
				__path_file__, __func__, __LINE__,
				var.xres, var.yres, var.yres_virtual,
				fix.line_length, fix.smem_len);
#endif

	int ret = videoDecoder->SetVideoSystem(newSystem, remember);

#ifdef DEBUGINFO_SETVIDEOSYSTEM
	ioctl(fd, FBIOGET_VSCREENINFO, &var);
	ioctl(fd, FBIOGET_FSCREENINFO, &fix);
	printf(">>>>>[%s - %s:%d] after SetVideoSystem:\n"
				"                var.xres        : %4d, var.yres    : %4d, var.yres_virtual: %4d\n"
				"                fix.line_length : %4d, fix.smem_len: %d Byte\n",
				__path_file__, __func__, __LINE__,
				var.xres, var.yres, var.yres_virtual,
				fix.line_length, fix.smem_len);
#endif

	return ret;
}
