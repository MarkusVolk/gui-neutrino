// The remote control, drawn as SVG from remote-layout.json (the format of
// neutrino-remote): rectangles and circles in a 200 x 624 box that the SVG
// scales to whatever space it gets.

const NS = "http://www.w3.org/2000/svg";

// glyphs in a 16 x 16 box, in place of the icon theme names of the layout
const GLYPHS = {
	"system-shutdown-symbolic": '<path class="line" d="M8 2v5"/><path class="line" d="M4.6 4.4a5 5 0 1 0 6.8 0"/>',
	"audio-volume-muted-symbolic": '<path d="M2 6h2.5L8 3v10L4.5 10H2z"/><path class="line" d="M10.5 6l4 4M14.5 6l-4 4"/>',
	"pan-up-symbolic": '<path d="M8 4l5 7H3z"/>',
	"pan-down-symbolic": '<path d="M8 12L3 5h10z"/>',
	"pan-start-symbolic": '<path d="M4 8l7-5v10z"/>',
	"pan-end-symbolic": '<path d="M12 8L5 3v10z"/>',
	"media-seek-backward-symbolic": '<path d="M8 3v10L2 8zM15 3v10L9 8z"/>',
	"media-seek-forward-symbolic": '<path d="M8 3v10l6-5zM1 3v10l6-5z"/>',
	"media-playback-start-symbolic": '<path d="M1.5 3v10L7.5 8z"/><path d="M9.5 3h2v10h-2zM13 3h2v10h-2z"/>',
	"media-playback-stop-symbolic": '<rect x="3" y="3" width="10" height="10" rx="1"/>',
	"media-record-symbolic": '<circle cx="8" cy="8" r="5" fill="#e05252"/>',
};

// held down, these keys repeat
const REPEAT = new Set(["KEY_UP", "KEY_DOWN", "KEY_LEFT", "KEY_RIGHT",
	"KEY_VOLUMEUP", "KEY_VOLUMEDOWN", "KEY_PAGEUP", "KEY_PAGEDOWN"]);

function el(name, attrs = {}) {
	const e = document.createElementNS(NS, name);
	for (const [k, v] of Object.entries(attrs))
		e.setAttribute(k, v);
	return e;
}

function bounds(k) {
	const c = k.coords;
	if (k.shape === "circle")
		return { x: c[0] - c[2], y: c[1] - c[2], w: 2 * c[2], h: 2 * c[2] };
	return { x: c[0], y: c[1], w: c[2] - c[0], h: c[3] - c[1] };
}

export async function loadLayout() {
	const r = await fetch("/remote-layout.json", { cache: "no-cache" });
	return r.json();
}

/* Draws the layout into container; press(name) is called for each key
 * press and for each repeat of a held key. */
export function drawRemote(container, layout, press) {
	const svg = el("svg", {
		class: "remote",
		viewBox: `0 0 ${layout.width} ${layout.height}`,
		preserveAspectRatio: "xMidYMid meet",
		role: "group",
		"aria-label": "Remote control",
	});
	svg.append(el("rect", { class: "body", x: 0, y: 0, width: layout.width, height: layout.height, rx: 24 }));

	for (const k of layout.keys) {
		const b = bounds(k);
		const g = el("g", { class: "key", role: "button", tabindex: "-1", "aria-label": k.title || k.label || k.key });
		const face = k.shape === "circle"
			? el("circle", { class: "face", cx: k.coords[0], cy: k.coords[1], r: k.coords[2] })
			: el("rect", { class: "face", x: b.x, y: b.y, width: b.w, height: b.h, rx: 5 });
		if (k.color)
			face.style.fill = k.color;
		g.append(face);
		const title = el("title");
		title.textContent = k.title || k.label || k.key;
		g.append(title);
		if (k.icon && GLYPHS[k.icon]) {
			const size = Math.max(8, Math.min(b.w, b.h) * 0.55);
			const glyph = el("g", {
				class: "glyph",
				transform: `translate(${b.x + (b.w - size) / 2} ${b.y + (b.h - size) / 2}) scale(${size / 16})`,
			});
			glyph.innerHTML = GLYPHS[k.icon];
			g.append(glyph);
		} else if (k.label) {
			const t = el("text", { x: b.x + b.w / 2, y: b.y + b.h / 2 });
			t.style.fontSize = `${Math.min(b.h * 0.45, 13)}px`;
			t.textContent = k.label;
			g.append(t);
		}
		attach(g, k.key, press);
		svg.append(g);
	}
	container.replaceChildren(svg);
	return svg;
}

function attach(g, name, press) {
	let timer = 0;
	const stop = () => {
		clearTimeout(timer);
		timer = 0;
		g.classList.remove("pressed");
	};
	g.addEventListener("pointerdown", (ev) => {
		ev.preventDefault();
		g.setPointerCapture?.(ev.pointerId);
		g.classList.add("pressed");
		navigator.vibrate?.(12);
		press(name);
		if (REPEAT.has(name)) {
			const again = (delay) => {
				timer = setTimeout(() => {
					press(name);
					again(140);
				}, delay);
			};
			again(450);
		} else
			timer = setTimeout(stop, 160);
	});
	g.addEventListener("pointerup", stop);
	g.addEventListener("pointercancel", stop);
	g.addEventListener("lostpointercapture", stop);
	g.addEventListener("contextmenu", (ev) => ev.preventDefault());
}

// keys of a keyboard that work like the remote control
export const KEYBOARD = {
	ArrowUp: "KEY_UP", ArrowDown: "KEY_DOWN", ArrowLeft: "KEY_LEFT", ArrowRight: "KEY_RIGHT",
	Enter: "KEY_OK", Escape: "KEY_HOME", Backspace: "KEY_HOME", m: "KEY_SETUP", i: "KEY_INFO",
	e: "KEY_EPG", "+": "KEY_VOLUMEUP", "-": "KEY_VOLUMEDOWN", PageUp: "KEY_PAGEUP", PageDown: "KEY_PAGEDOWN",
	r: "KEY_RED", g: "KEY_GREEN", y: "KEY_YELLOW", b: "KEY_BLUE", t: "KEY_TEXT",
	0: "KEY_0", 1: "KEY_1", 2: "KEY_2", 3: "KEY_3", 4: "KEY_4", 5: "KEY_5", 6: "KEY_6", 7: "KEY_7", 8: "KEY_8", 9: "KEY_9",
};
