
#ifndef __osd_helpers__
#define __osd_helpers__

#include <sigc++/signal.h>

class COsdHelpers : public sigc::trackable
{
	private:

	public:
		COsdHelpers();
		~COsdHelpers();
		static COsdHelpers* getInstance();

		/* the size of the area the OSD is shown in: the window or screen
		   where the framebuffer knows it, else that of the video system */
		void getOutputSize(int &w, int &h);
		/* draws the OSD at the size that suits the output and sets up what
		   depends on it; false when the size stays as it is (and not forced) */
		bool applyOsdSize(bool force = false);
		int  isVideoSystem1080(int res);
		int  getVideoSystem();
		int setVideoSystem(int newSystem, bool remember = true);
		sigc::signal<void> OnAfterChangeResolution;
		sigc::signal<void> OnBeforeChangeResolution;
};


#endif //__osd_helpers__
