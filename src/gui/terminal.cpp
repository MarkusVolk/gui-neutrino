/*
	Terminal for plugins that run a text user interface, drawn on the OSD

	The program runs on a pseudo terminal, libtsm keeps the screen and turns
	keys into what the program reads. On generic hardware the keyboard comes
	from libstb-hal as typed characters, the remote control works everywhere.

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
	along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <linux/input.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <vector>

#include <global.h>
#include <neutrino.h>
#include <driver/abstime.h>
#include <driver/fontrenderer.h>
#include <driver/framebuffer.h>
#include <driver/neutrinofonts.h>
#include <driver/rcinput.h>
#include <driver/textkeyboard.h>
#include <gui/infoviewer.h>
#include <gui/terminal.h>
#include <gui/widget/keyboard_input.h>
#include <gui/widget/remote_ui.h>


extern std::string font_file_monospace;

#define TERM_ROWS 40

/* the colours of foot */
static uint8_t palette[TSM_COLOR_NUM][3] = {
	[TSM_COLOR_BLACK]		= { 0x24, 0x24, 0x24 },
	[TSM_COLOR_RED]			= { 0xf6, 0x2b, 0x5a },
	[TSM_COLOR_GREEN]		= { 0x47, 0xb4, 0x13 },
	[TSM_COLOR_YELLOW]		= { 0xe3, 0xc4, 0x01 },
	[TSM_COLOR_BLUE]		= { 0x24, 0xac, 0xd4 },
	[TSM_COLOR_MAGENTA]		= { 0xf2, 0xaf, 0xfd },
	[TSM_COLOR_CYAN]		= { 0x13, 0xc2, 0x99 },
	[TSM_COLOR_LIGHT_GREY]		= { 0xe6, 0xe6, 0xe6 },
	[TSM_COLOR_DARK_GREY]		= { 0x61, 0x61, 0x61 },
	[TSM_COLOR_LIGHT_RED]		= { 0xff, 0x4d, 0x51 },
	[TSM_COLOR_LIGHT_GREEN]		= { 0x35, 0xd4, 0x50 },
	[TSM_COLOR_LIGHT_YELLOW]	= { 0xe9, 0xe8, 0x36 },
	[TSM_COLOR_LIGHT_BLUE]		= { 0x5d, 0xc5, 0xf8 },
	[TSM_COLOR_LIGHT_MAGENTA]	= { 0xfe, 0xab, 0xf2 },
	[TSM_COLOR_LIGHT_CYAN]		= { 0x24, 0xdf, 0xc4 },
	[TSM_COLOR_WHITE]		= { 0xff, 0xff, 0xff },
	[TSM_COLOR_FOREGROUND]		= { 0xff, 0xff, 0xff },
	[TSM_COLOR_BACKGROUND]		= { 0x24, 0x24, 0x24 },
};

static fb_pixel_t pixel(uint8_t r, uint8_t g, uint8_t b)
{
	return 0xff000000 | (r << 16) | (g << 8) | b;
}

CTerminal::CTerminal(const std::string &Program)
{
	program = Program;
	frameBuffer = CFrameBuffer::getInstance();
	fontRenderer = NULL;
	font = NULL;
	screen = NULL;
	vte = NULL;
	pid = -1;
	master = -1;
	layoutShown = 0;
	escDown = escLast = 0;
	x = y = cols = rows = cellWidth = cellHeight = 0;
	age = 0;
	cursorX = cursorY = 0;
	leftX = leftY = 0;
	dirty = true;
	scrolled = false;
}

CTerminal::~CTerminal()
{
	if (vte)
		tsm_vte_unref(vte);
	if (screen)
		tsm_screen_unref(screen);
	delete font;
	delete fontRenderer;
}

bool CTerminal::setupFont()
{
	std::string ttf = CNeutrinoFonts::getInstance()->getShellTTF();
	if (ttf.empty())
		ttf = font_file_monospace;

	x = frameBuffer->getScreenX();
	y = frameBuffer->getScreenY();
	int width = frameBuffer->getScreenWidth();
	int height = frameBuffer->getScreenHeight();

	/* the line height of a font is more than its size, so the size is
	 * found in two steps */
	int size = height / TERM_ROWS;
	for (int i = 0; i < 2; i++)
	{
		delete font;
		font = NULL;
		delete fontRenderer;
		fontRenderer = new FBFontRenderClass();
		const char *style = fontRenderer->AddFont(ttf.c_str());
		if (!style)
			return false;
		font = fontRenderer->getFont(fontRenderer->getFamily(ttf.c_str()).c_str(), style, size);
		if (!font)
			return false;
		cellHeight = font->getHeight();
		if (cellHeight <= 0)
			return false;
		size = size * (height / TERM_ROWS) / cellHeight;
	}
	cellWidth = font->getRenderWidth("M");
	if (cellWidth <= 0)
		return false;
	cols = width / cellWidth;
	rows = height / cellHeight;
	/* centre the cells in the visible area */
	x += (width - cols * cellWidth) / 2;
	y += (height - rows * cellHeight) / 2;
	printf("[terminal] %s: %dx%d cells of %dx%d\n", ttf.c_str(), cols, rows, cellWidth, cellHeight);
	return true;
}

bool CTerminal::start()
{
	std::string home;
	const char *h = getenv("HOME");
	if (h && *h)
		home = h;
	else
	{
		struct passwd *pw = getpwuid(getuid());
		home = pw ? pw->pw_dir : "/";
	}

	/* everything the child needs is made before fork(), the child only
	 * calls what is safe in a copy of a threaded process */
	std::vector<std::string> env;
	/* the keyboard types UTF-8; in a locale with another character set,
	 * such as the C locale neutrino runs in, programs would not take
	 * umlauts and the like */
	const char *ctype = getenv("LC_ALL");
	if (!ctype || !*ctype)
		ctype = getenv("LC_CTYPE");
	if (!ctype || !*ctype)
		ctype = getenv("LANG");
	bool utf8 = ctype && (strcasestr(ctype, "UTF-8") || strcasestr(ctype, "utf8"));
	for (char **e = environ; *e; e++)
		if (strncmp(*e, "TERM=", 5) && strncmp(*e, "COLORTERM=", 10) &&
		    strncmp(*e, "HOME=", 5) && strncmp(*e, "LINES=", 6) && strncmp(*e, "COLUMNS=", 8) &&
		    (utf8 || (strncmp(*e, "LC_ALL=", 7) && strncmp(*e, "LC_CTYPE=", 9))))
			env.push_back(*e);
	if (!utf8)
		env.push_back("LC_CTYPE=C.UTF-8");
	env.push_back("TERM=xterm-256color");
	env.push_back("COLORTERM=truecolor");
	env.push_back("HOME=" + home);
	std::vector<char *> envp;
	for (size_t i = 0; i < env.size(); i++)
		envp.push_back((char *)env[i].c_str());
	envp.push_back(NULL);
	const char *argv[] = { "/bin/sh", program.c_str(), NULL };

	struct winsize ws;
	memset(&ws, 0, sizeof(ws));
	ws.ws_row = rows;
	ws.ws_col = cols;
	ws.ws_xpixel = cols * cellWidth;
	ws.ws_ypixel = rows * cellHeight;

	pid = forkpty(&master, NULL, NULL, &ws);
	if (pid < 0)
	{
		printf("[terminal] forkpty: %m\n");
		return false;
	}
	if (pid == 0)
	{
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = SIG_DFL;
		for (int i = 1; i < NSIG; i++)
			sigaction(i, &sa, NULL);
		close_range(3, ~0U, 0);
		if (chdir(home.c_str()))
			(void)!chdir("/");
		execve(argv[0], (char * const *)argv, envp.data());
		_exit(127);
	}
	fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
	fcntl(master, F_SETFD, FD_CLOEXEC);
	return true;
}

/* closing the pseudo terminal hangs up the program; one that ignores
 * that gets a second to end before it is killed */
void CTerminal::stop(int *status)
{
	if (master >= 0)
		close(master);
	master = -1;
	if (pid <= 0)
		return;
	kill(pid, SIGHUP);
	for (int i = 0; i < 20; i++)
	{
		pid_t r = waitpid(pid, status, WNOHANG);
		if (r == pid || (r < 0 && errno != EINTR))
		{
			pid = -1;
			return;
		}
		usleep(50000);
	}
	kill(pid, SIGKILL);
	waitpid(pid, status, 0);
	pid = -1;
}

/* false once the program has closed its side */
bool CTerminal::readOutput()
{
	char buf[16384];
	for (;;)
	{
		ssize_t n = read(master, buf, sizeof(buf));
		if (n > 0)
		{
			tsm_vte_input(vte, buf, n);
			dirty = true;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			return true;
		return false;
	}
}

/* false for the menu key, which ends the terminal as on the remote control */
bool CTerminal::readKeyboard()
{
	struct text_key k;
	while (keyboard.read(k))
	{
		if (k.keysym == XKB_KEY_Menu)
			return false;
		if (k.keysym == XKB_KEY_Escape && !k.mods)
		{
			/* held for a second it ends; a tap goes to the program at once */
			int64_t now = time_monotonic_ms();
			if (escDown && now - escLast < 600)
			{
				escLast = now;
				if (now - escDown >= 1000)
					return false;
				continue;
			}
			escDown = escLast = now;
		}
		if ((k.mods & TEXT_KEY_SHIFT) && (k.keysym == XKB_KEY_Prior || k.keysym == XKB_KEY_Next))
		{
			scrollBack(k.keysym == XKB_KEY_Prior);
			continue;
		}
		if ((k.mods & TEXT_KEY_SHIFT) && (k.keysym == XKB_KEY_Up || k.keysym == XKB_KEY_Down))
		{
			scrollBack(k.keysym == XKB_KEY_Up, rows / 2);
			continue;
		}
		scrollBottom();
		unsigned int mods = 0;
		if (k.mods & TEXT_KEY_SHIFT)
			mods |= TSM_SHIFT_MASK;
		if (k.mods & TEXT_KEY_CTRL)
			mods |= TSM_CONTROL_MASK;
		if (k.mods & TEXT_KEY_ALT)
			mods |= TSM_ALT_MASK;
		if (k.mods & TEXT_KEY_LOGO)
			mods |= TSM_LOGO_MASK;
		tsm_vte_handle_keyboard(vte, k.keysym, k.unicode && k.unicode < 0x80 ? k.unicode : TSM_VTE_INVALID,
					mods, k.unicode ? k.unicode : TSM_VTE_INVALID);
	}
	std::string name = keyboard.switchedLayout();
	if (!name.empty())
	{
		layoutName = name;
		layoutShown = time_monotonic_ms() + 1500;
	}
	return true;
}

/* Shift+Page Up/Down, Shift+Up/Down and rewind/forward page through the
 * lines that went out at the top, a whole page or the given lines */
void CTerminal::scrollBack(bool up, unsigned int lines)
{
	if (lines)
		up ? tsm_screen_sb_up(screen, lines) : tsm_screen_sb_down(screen, lines);
	else if (up)
		tsm_screen_sb_page_up(screen, 1);
	else
		tsm_screen_sb_page_down(screen, 1);
	scrolled = true;
	repaint();
}

/* any other key goes back to what the program shows now */
void CTerminal::scrollBottom()
{
	if (!scrolled)
		return;
	tsm_screen_sb_reset(screen);
	scrolled = false;
	repaint();
}

/* text typed on the on-screen keyboard goes to the program as it is */
void CTerminal::typeText()
{
	std::string text;
	keyboard.close();
	CKeyboardInput input(std::string("Terminal"), &text, 255, NULL, NULL, std::string("OK sends the text to the program"), std::string(""));
	/* a shell, maybe asking for a password: only the viewer types here */
	CRemoteUi::ViewerOnly viewer;
	input.exec(NULL, "");
	keyboard.open();
	g_RCInput->clearRCMsg();
	repaint();
	if (!text.empty())
		write_cb(vte, text.data(), text.size(), this);
}

/* the remote control: the cursor keys, OK, exit and the digits are what a
 * text user interface needs most, red opens the on-screen keyboard, green
 * is Ctrl+C and yellow Tab, rewind and forward page through the scroll-back. A keyboard that neutrino reads as a remote
 * control types lower case letters. False for the menu key, which ends;
 * exit or Escape held for a second ends as well, see exec(). */
bool CTerminal::rcKey(uint32_t msg)
{
	uint32_t sym = XKB_KEY_NoSymbol;
	uint32_t ch = TSM_VTE_INVALID;
	unsigned int mods = 0;
	switch (msg)
	{
		case CRCInput::RC_setup:
			return false;
		case CRCInput::RC_rewind:
		case CRCInput::RC_forward:
			scrollBack(msg == CRCInput::RC_rewind);
			return true;
		case CRCInput::RC_red:
			scrollBottom();
			typeText();
			return true;
		case CRCInput::RC_green:
			ch = 'c';
			mods = TSM_CONTROL_MASK;
			break;
		case CRCInput::RC_yellow:
		case KEY_TAB:			sym = XKB_KEY_Tab; break;
		case KEY_ENTER:			sym = XKB_KEY_Return; break;
		case KEY_BACKSPACE:		sym = XKB_KEY_BackSpace; break;
		case KEY_ESC:			sym = XKB_KEY_Escape; break;
		case CRCInput::RC_up:		sym = XKB_KEY_Up; break;
		case CRCInput::RC_down:		sym = XKB_KEY_Down; break;
		case CRCInput::RC_left:		sym = XKB_KEY_Left; break;
		case CRCInput::RC_right:	sym = XKB_KEY_Right; break;
		case CRCInput::RC_page_up:	sym = XKB_KEY_Prior; break;
		case CRCInput::RC_page_down:	sym = XKB_KEY_Next; break;
		case CRCInput::RC_ok:		sym = XKB_KEY_Return; break;
		case CRCInput::RC_home:
		case CRCInput::RC_back:		sym = XKB_KEY_Escape; break;
		default:
			if (msg >= CRCInput::RC_0 && msg <= CRCInput::RC_9)
				ch = msg == CRCInput::RC_0 ? '0' : '1' + (msg - CRCInput::RC_1);
			else
			{
				const char *u = CRCInput::getUnicodeValue(msg);
				if (*u)
					ch = tolower((unsigned char)*u);
			}
			break;
	}
	scrollBottom();
	if (ch != TSM_VTE_INVALID)
		tsm_vte_handle_keyboard(vte, ch, ch, mods, ch);
	else if (sym != XKB_KEY_NoSymbol)
		tsm_vte_handle_keyboard(vte, sym, TSM_VTE_INVALID, 0, TSM_VTE_INVALID);
	return true;
}

void CTerminal::write_cb(struct tsm_vte *, const char *u8, size_t len, void *data)
{
	CTerminal *t = (CTerminal *)data;
	while (len > 0 && t->master >= 0)
	{
		ssize_t n = write(t->master, u8, len);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN)
		{
			usleep(1000);
			continue;
		}
		if (n <= 0)
			return;
		u8 += n;
		len -= n;
	}
}

int CTerminal::draw_cb(struct tsm_screen *, uint64_t, const uint32_t *ch, size_t len,
		       unsigned int width, unsigned int posx, unsigned int posy,
		       const struct tsm_screen_attr *attr, tsm_age_t cellAge, void *data)
{
	return ((CTerminal *)data)->drawCell(ch, len, width, posx, posy, attr, cellAge);
}

static int utf8(uint32_t c, char *out)
{
	if (c < 0x80)
	{
		out[0] = c;
		return 1;
	}
	if (c < 0x800)
	{
		out[0] = 0xc0 | (c >> 6);
		out[1] = 0x80 | (c & 0x3f);
		return 2;
	}
	if (c < 0x10000)
	{
		out[0] = 0xe0 | (c >> 12);
		out[1] = 0x80 | ((c >> 6) & 0x3f);
		out[2] = 0x80 | (c & 0x3f);
		return 3;
	}
	out[0] = 0xf0 | (c >> 18);
	out[1] = 0x80 | ((c >> 12) & 0x3f);
	out[2] = 0x80 | ((c >> 6) & 0x3f);
	out[3] = 0x80 | (c & 0x3f);
	return 4;
}

/* colour between bg and fg, t from 0 to 1 */
static fb_pixel_t mix(fb_pixel_t bg, fb_pixel_t fg, float t)
{
	fb_pixel_t r = 0xff000000;
	for (int shift = 0; shift < 24; shift += 8)
	{
		int b = (bg >> shift) & 0xff, f = (fg >> shift) & 0xff;
		r |= (fb_pixel_t)(b + (f - b) * t + 0.5f) << shift;
	}
	return r;
}

/* lines, blocks and the powerline separators fill the cell edge to edge,
 * which glyphs of a font do not, so they are drawn here */
bool CTerminal::drawShape(uint32_t c, int px, int py, int w, uint32_t fg, uint32_t bg)
{
	int h = cellHeight;
	switch (c)
	{
		case 0xe0b0: case 0xe0b2: case 0xe0b4: case 0xe0b6:
		{
			bool right = c == 0xe0b0 || c == 0xe0b4;
			bool round = c == 0xe0b4 || c == 0xe0b6;
			for (int r = 0; r < h; r++)
			{
				float dy = (r + 0.5f - h / 2.0f) / (h / 2.0f);
				float e = round ? w * sqrtf(fmaxf(0.0f, 1.0f - dy * dy)) : w * (1.0f - fabsf(dy));
				int full = (int)e;
				if (full > w)
					full = w;
				int x0 = right ? px : px + w - full;
				if (full > 0)
					frameBuffer->paintBoxRel(x0, py + r, full, 1, fg);
				if (full < w && e > full)
					frameBuffer->paintBoxRel(right ? px + full : x0 - 1, py + r, 1, 1, mix(bg, fg, e - full));
			}
			return true;
		}
		case 0x2580:
			frameBuffer->paintBoxRel(px, py, w, h / 2, fg);
			return true;
		case 0x2588:
			frameBuffer->paintBoxRel(px, py, w, h, fg);
			return true;
		case 0x2590:
			frameBuffer->paintBoxRel(px + w / 2, py, w - w / 2, h, fg);
			return true;
		case 0x2591: case 0x2592: case 0x2593:
			frameBuffer->paintBoxRel(px, py, w, h, mix(bg, fg, (c - 0x2590) / 4.0f));
			return true;
		case 0x2594:
			frameBuffer->paintBoxRel(px, py, w, h / 8 > 0 ? h / 8 : 1, fg);
			return true;
		case 0x2595:
			frameBuffer->paintBoxRel(px + w - (w / 8 > 0 ? w / 8 : 1), py, w / 8 > 0 ? w / 8 : 1, h, fg);
			return true;
		default:
			break;
	}

	/* lower eighths, ▁ to ▇, and left eighths, ▉ to ▏, as graphs and bars use them */
	if (c >= 0x2581 && c <= 0x2587)
	{
		int part = h * (int)(c - 0x2580) / 8;
		frameBuffer->paintBoxRel(px, py + h - part, w, part, fg);
		return true;
	}
	if (c >= 0x2589 && c <= 0x258f)
	{
		int part = w * (int)(0x2590 - c) / 8;
		frameBuffer->paintBoxRel(px, py, part > 0 ? part : 1, h, fg);
		return true;
	}

	/* quadrants, ▖ to ▟: upper left, upper right, lower left, lower right */
	static const unsigned char quadrants[] = { 4, 8, 1, 13, 9, 7, 11, 2, 6, 14 };
	if (c >= 0x2596 && c <= 0x259f)
	{
		int q = quadrants[c - 0x2596];
		int hw = w / 2, hh = h / 2;
		if (q & 1)
			frameBuffer->paintBoxRel(px, py, hw, hh, fg);
		if (q & 2)
			frameBuffer->paintBoxRel(px + hw, py, w - hw, hh, fg);
		if (q & 4)
			frameBuffer->paintBoxRel(px, py + hh, hw, h - hh, fg);
		if (q & 8)
			frameBuffer->paintBoxRel(px + hw, py + hh, w - hw, h - hh, fg);
		return true;
	}

	/* braille, two columns of four dots, as btop draws its graphs with them */
	if (c >= 0x2800 && c <= 0x28ff)
	{
		static const int dot_col[8] = { 0, 0, 0, 1, 1, 1, 0, 1 };
		static const int dot_row[8] = { 0, 1, 2, 0, 1, 2, 3, 3 };
		int bits = c - 0x2800;
		for (int i = 0; i < 8; i++)
		{
			if (!(bits & (1 << i)))
				continue;
			int x0 = px + w * dot_col[i] / 2;
			int x1 = px + w * (dot_col[i] + 1) / 2;
			int y0 = py + h * dot_row[i] / 4;
			int y1 = py + h * (dot_row[i] + 1) / 4;
			frameBuffer->paintBoxRel(x0, y0, x1 - x0, y1 - y0, fg);
		}
		return true;
	}

	enum { U = 1, D = 2, L = 4, R = 8, HEAVY = 16 };
	int parts;
	switch (c)
	{
		case 0x2500: parts = L | R; break;
		case 0x2501: parts = L | R | HEAVY; break;
		case 0x2502: parts = U | D; break;
		case 0x2503: parts = U | D | HEAVY; break;
		case 0x250c: case 0x256d: parts = D | R; break;
		case 0x2510: case 0x256e: parts = D | L; break;
		case 0x2514: case 0x2570: parts = U | R; break;
		case 0x2518: case 0x256f: parts = U | L; break;
		case 0x251c: parts = U | D | R; break;
		case 0x2524: parts = U | D | L; break;
		case 0x252c: parts = L | R | D; break;
		case 0x2534: parts = L | R | U; break;
		case 0x253c: parts = U | D | L | R; break;
		default: return false;
	}
	int t = w / 10 > 1 ? w / 10 : 1;
	if (parts & HEAVY)
		t *= 2;
	int cx = px + (w - t) / 2;
	int cy = py + (h - t) / 2;
	if (parts & U)
		frameBuffer->paintBoxRel(cx, py, t, cy - py + t, fg);
	if (parts & D)
		frameBuffer->paintBoxRel(cx, cy, t, py + h - cy, fg);
	if (parts & L)
		frameBuffer->paintBoxRel(px, cy, cx - px + t, t, fg);
	if (parts & R)
		frameBuffer->paintBoxRel(cx, cy, px + w - cx, t, fg);
	return true;
}

int CTerminal::drawCell(const uint32_t *ch, size_t len, unsigned int width,
			unsigned int posx, unsigned int posy,
			const struct tsm_screen_attr *attr, tsm_age_t cellAge)
{
	bool cursor = posx == cursorX && posy == cursorY &&
		!(tsm_screen_get_flags(screen) & TSM_SCREEN_HIDE_CURSOR);
	if (cellAge && age && cellAge <= age && !cursor && !(posx == leftX && posy == leftY))
		return 0;
	if (!width)
		return 0;

	fb_pixel_t fg = pixel(attr->fr, attr->fg, attr->fb);
	fb_pixel_t bg = pixel(attr->br, attr->bg, attr->bb);
	/* libtsm has inverted the cell under the cursor already */
	if (attr->inverse)
	{
		fb_pixel_t t = fg;
		fg = bg;
		bg = t;
	}
	int px = x + posx * cellWidth;
	int py = y + posy * cellHeight;
	int w = width * cellWidth;
	frameBuffer->paintBoxRel(px, py, w, cellHeight, bg);
	if (len == 1 && drawShape(ch[0], px, py, w, fg, bg))
		;
	else if (len && ch[0] != ' ')
	{
		char buf[4 * 8 + 1];
		int n = 0;
		for (size_t i = 0; i < len && i < 8; i++)
			n += utf8(ch[i], buf + n);
		buf[n] = 0;
		font->RenderString(px, py + cellHeight, w, buf, fg, 0, Font::IS_UTF8 | Font::FULLBG);
	}
	if (attr->underline)
		frameBuffer->paintBoxRel(px, py + cellHeight - 2, w, 1, fg);
	return 0;
}

/* the layout Alt+Shift switched to, in the top right corner for a moment */
void CTerminal::drawLayout()
{
	std::string name = layoutName;
	int w = font->getRenderWidth(name.c_str()) + 2 * cellWidth;
	int h = cellHeight * 3 / 2;
	int bx = x + cols * cellWidth - w - cellWidth;
	int by = y + cellHeight;
	frameBuffer->paintBoxRel(bx, by, w, h, pixel(palette[TSM_COLOR_BLUE][0], palette[TSM_COLOR_BLUE][1], palette[TSM_COLOR_BLUE][2]), h / 2);
	font->RenderString(bx + cellWidth, by + (h + cellHeight) / 2, w - cellWidth, name.c_str(),
			   pixel(palette[TSM_COLOR_BACKGROUND][0], palette[TSM_COLOR_BACKGROUND][1], palette[TSM_COLOR_BACKGROUND][2]),
			   0, Font::IS_UTF8 | Font::FULLBG);
	frameBuffer->blit();
}

/* everything again, over whatever neutrino drew meanwhile */
void CTerminal::repaint()
{
	frameBuffer->paintBoxRel(0, 0, frameBuffer->getScreenWidth(true), frameBuffer->getScreenHeight(true),
				 pixel(palette[TSM_COLOR_BACKGROUND][0], palette[TSM_COLOR_BACKGROUND][1], palette[TSM_COLOR_BACKGROUND][2]));
	age = 0;
	dirty = true;
}

void CTerminal::draw()
{
	/* the cell the cursor leaves has to be drawn again as well */
	leftX = cursorX;
	leftY = cursorY;
	cursorX = tsm_screen_get_cursor_x(screen);
	cursorY = tsm_screen_get_cursor_y(screen);
	age = tsm_screen_draw(screen, draw_cb, this);
	frameBuffer->blit();
	dirty = false;
}

void CTerminal::waitForKey()
{
	neutrino_msg_t msg;
	neutrino_msg_data_t data;
	int64_t end = time_monotonic_ms() + 30000;
	g_RCInput->clearRCMsg();
	while (time_monotonic_ms() < end)
	{
		g_RCInput->getMsg_ms(&msg, &data, 100);
		if (msg <= CRCInput::RC_MaxRC && !(msg & CRCInput::RC_Release))
			return;
		if (msg > CRCInput::RC_MaxRC && msg != CRCInput::RC_timeout)
			CNeutrinoApp::getInstance()->handleMsg(msg, data);
		struct text_key k;
		if (keyboard.read(k))
			return;
	}
}

int CTerminal::exec()
{
	int status = -1;
	if (!setupFont())
	{
		printf("[terminal] no usable font\n");
		return -1;
	}
	if (tsm_screen_new(&screen, NULL, NULL) < 0 ||
	    tsm_screen_resize(screen, cols, rows) < 0 ||
	    tsm_vte_new(&vte, screen, write_cb, this, NULL, NULL) < 0)
	{
		printf("[terminal] libtsm failed\n");
		return -1;
	}
	tsm_screen_set_max_sb(screen, 2000);
	tsm_vte_set_backspace_sends_delete(vte, true);
	tsm_vte_set_custom_palette(vte, palette);
	tsm_vte_set_palette(vte, "custom");
	if (!start())
		return -1;
	keyboard.open();
	if (g_InfoViewer)
		g_InfoViewer->killTitle();
	g_RCInput->clearRCMsg();
	repaint();

	bool running = true;
	/* exit, and Escape on a keyboard, is Escape when tapped and ends the
	 * terminal when held for a second, so it goes to the program only when
	 * it is let go; a press that neither repeats nor is let go, as from the
	 * web interface, is a tap */
	int64_t exitDown = 0, exitLast = 0;
	neutrino_msg_t exitKey = CRCInput::RC_home;
	CRemoteView remote_view("Terminal", [] { return std::string("A shell that only the viewer types into."); }, true);
	while (running)
	{
		if (!readOutput())
			break;
		if (!readKeyboard())
		{
			running = false;
			break;
		}
		if (dirty)
			draw();
		if (layoutShown)
		{
			if (time_monotonic_ms() < layoutShown)
				drawLayout();
			else
			{
				layoutShown = 0;
				repaint();
			}
		}

		neutrino_msg_t msg;
		neutrino_msg_data_t data;
		g_RCInput->getMsg_ms(&msg, &data, 20);
		if (exitDown && time_monotonic_ms() - exitLast > 600)
		{
			exitDown = 0;
			rcKey(exitKey);
		}
		if (msg == CRCInput::RC_timeout)
			continue;
		if (msg <= CRCInput::RC_MaxRC)
		{
			neutrino_msg_t key = msg & ~(CRCInput::RC_Repeat | CRCInput::RC_Release);
			if (key == CRCInput::RC_home || key == CRCInput::RC_back || key == KEY_ESC)
			{
				int64_t now = time_monotonic_ms();
				if (msg & CRCInput::RC_Release)
				{
					if (exitDown)
						rcKey(key);
					exitDown = 0;
				}
				else if (msg & CRCInput::RC_Repeat)
				{
					exitLast = now;
					if (exitDown && now - exitDown >= 1000)
						running = false;
				}
				else
				{
					exitDown = exitLast = now;
					exitKey = key;
				}
			}
			else if (!(msg & CRCInput::RC_Release))
				running = rcKey(key);
		}
		else if (msg == NeutrinoMessages::EVT_START_PLUGIN)
		{
			/* a plugin started from the web interface would run in here */
			delete[] (unsigned char *)data;
		}
		else
		{
			if (CNeutrinoApp::getInstance()->handleMsg(msg, data) & messages_return::cancel_all)
				running = false;
			else if (msg != NeutrinoMessages::EVT_TIMER)
				repaint();
		}
	}

	bool ended = running;
	if (ended)
	{
		/* the program ended by itself: it may have said why */
		int r = waitpid(pid, &status, 0);
		pid = -1;
		if (r > 0 && !(WIFEXITED(status) && WEXITSTATUS(status) == 0))
		{
			char line[80];
			snprintf(line, sizeof(line), "\r\n[%s %d]", WIFEXITED(status) ? "exit" : "signal",
				 WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
			tsm_vte_input(vte, line, strlen(line));
			draw();
			waitForKey();
		}
	}
	keyboard.close();
	stop(&status);
	g_RCInput->clearRCMsg();
	frameBuffer->paintBackground();
	frameBuffer->blit();
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
