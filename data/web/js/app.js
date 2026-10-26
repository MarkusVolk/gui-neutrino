import { api, all, session, may, key, osdUrl, ApiError, pairOnTv, httpsUrl, applyTheme } from "./api.js";
import { loadLayout, drawRemote, KEYBOARD } from "./remote.js";

const $ = (sel, root = document) => root.querySelector(sel);

const ICONS = {
	remote: '<rect x="7" y="2" width="10" height="20" rx="3"/><circle cx="12" cy="9" r="2.2"/><path d="M10 15h.01M14 15h.01M10 18h.01M14 18h.01"/>',
	channels: '<rect x="3" y="5" width="18" height="12" rx="2"/><path d="M8 21h8M12 17v4"/>',
	epg: '<rect x="3" y="4" width="18" height="17" rx="2"/><path d="M3 9h18M8 2v4M16 2v4M7 13h4M7 17h7"/>',
	timers: '<circle cx="12" cy="13" r="8"/><path d="M12 9v4l2.5 2.5M9 2h6"/>',
	mounts: '<rect x="3" y="4" width="18" height="7" rx="2"/><rect x="3" y="13" width="18" height="7" rx="2"/><path d="M7 7.5h.01M7 16.5h.01M11 7.5h6M11 16.5h6"/>',
	box: '<rect x="2" y="7" width="20" height="10" rx="2"/><path d="M6 12h.01M10 12h4"/>',
	record: '<circle cx="12" cy="12" r="6" fill="currentColor" stroke="none"/>',
	stream: '<path d="M8 5v14l11-7z" fill="currentColor" stroke="none"/>',
	trash: '<path d="M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3"/>',
	refresh: '<path d="M20 11a8 8 0 1 0-2.3 5.7M20 4v7h-7"/>',
	phone: '<rect x="7" y="2" width="10" height="20" rx="2"/><path d="M11 18h2"/>',
	camera: '<path d="M4 8h3l1.5-2.5h7L17 8h3a1 1 0 0 1 1 1v9.5a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V9a1 1 0 0 1 1-1z"/><circle cx="12" cy="13.5" r="3.5"/>',
	cloud: '<path d="M7 18h10.5a4 4 0 0 0 .5-7.97A6 6 0 0 0 6.34 9.5 4.25 4.25 0 0 0 7 18z"/>',
	puzzle: '<path d="M10 4a2 2 0 1 1 4 0v2h4v4h-2a2 2 0 1 0 0 4h2v4h-4v-2a2 2 0 1 0-4 0v2H6v-4H4a2 2 0 1 1 0-4h2V6h4z"/>',
};

function icon(name) {
	return `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${ICONS[name] || ICONS.puzzle}</svg>`;
}

function h(tag, attrs = {}, ...children) {
	const e = document.createElement(tag);
	for (const [k, v] of Object.entries(attrs)) {
		if (k.startsWith("on"))
			e.addEventListener(k.slice(2), v);
		else if (k === "html")
			e.innerHTML = v;
		else if (v !== false && v !== undefined && v !== null)
			e.setAttribute(k, v === true ? "" : v);
	}
	for (const c of children.flat())
		if (c !== null && c !== undefined && c !== false)
			e.append(c instanceof Node ? c : document.createTextNode(String(c)));
	return e;
}

let toastTimer = 0;
export function toast(text, error = false) {
	let t = $(".toast");
	if (!t) {
		t = h("div", { class: "toast", role: "status" });
		document.body.append(t);
	}
	t.textContent = text;
	t.classList.toggle("error", error);
	t.hidden = false;
	clearTimeout(toastTimer);
	toastTimer = setTimeout(() => { t.hidden = true; }, 3500);
}

function fail(e) {
	if (e instanceof ApiError && (e.status === 401 || e.status === 403)) {
		location.hash = "#/signin";
		return;
	}
	toast(e.message || String(e), true);
}

function confirmDialog(text, okText = "OK", danger = false) {
	return new Promise((resolve) => {
		const d = h("dialog", {},
			h("p", {}, text),
			h("div", { class: "buttons" },
				h("button", { class: "btn", value: "no", onclick: () => d.close("no") }, "Cancel"),
				h("button", { class: `btn ${danger ? "danger" : "primary"}`, onclick: () => d.close("yes") }, okText)));
		d.addEventListener("close", () => { d.remove(); resolve(d.returnValue === "yes"); });
		document.body.append(d);
		d.showModal();
	});
}

const pad = (n) => String(n).padStart(2, "0");
const hm = (t) => { const d = new Date(t * 1000); return `${pad(d.getHours())}:${pad(d.getMinutes())}`; };
const day = (t) => new Date(t * 1000).toLocaleDateString(undefined, { weekday: "short", day: "numeric", month: "short" });
const now = () => Date.now() / 1000;

/* ------------------------------------------------------------------ */
/* shared data                                                         */

const cache = { bouquets: null, names: new Map() };

async function tvBouquets() {
	if (!cache.bouquets) {
		const items = (await api("bouquets")).items || [];
		cache.bouquets = items.filter((b) => !b.hidden && b.tv_count > 0);
	}
	return cache.bouquets;
}

async function channelName(id) {
	if (!cache.names.has(id)) {
		try {
			cache.names.set(id, (await api(`channels/${id}`)).name);
		} catch {
			cache.names.set(id, id);
		}
	}
	return cache.names.get(id);
}

function bouquetSelect(bouquets, selected, onchange) {
	const s = h("select", { "aria-label": "Bouquet", onchange: () => onchange(s.value) },
		bouquets.map((b) => h("option", { value: b.id, selected: String(b.id) === String(selected) }, b.name)));
	return s;
}

function savedBouquet(bouquets) {
	let id = "";
	try { id = localStorage.getItem("bouquet") || ""; } catch { /* no storage */ }
	return bouquets.some((b) => String(b.id) === id) ? id : String(bouquets[0]?.id ?? "");
}

function saveBouquet(id) {
	try { localStorage.setItem("bouquet", id); } catch { /* no storage */ }
}

async function record(channel, ev) {
	const body = { kind: "record", channel_id: channel, start: ev.start, stop: ev.start + ev.duration,
		title: ev.title, recording_safety: true };
	if (ev.id) {
		body.epg_id = ev.id;
		body.epg_start = ev.start;
	}
	let r;
	try {
		r = await api("timers", { method: "POST", body });
	} catch (e) {
		/* the box sees no tuner for it beside the timers there are, as its own menus ask */
		if (!(e instanceof ApiError && e.code === "timer-conflict"))
			throw e;
		const what = e.message.replace(/^overlaps with /, "");
		if (!await confirmDialog(`"${ev.title}" overlaps with ${what}. The box may not be able to record both. Set the timer anyway?`, "Set anyway"))
			return null;
		r = await api("timers", { method: "POST", body: { ...body, force: true } });
	}
	toast(`Recording set: ${ev.title}`);
	return r;
}

/* the browser runs on the box itself, where a stream would only play
 * what the TV plays already */
const onBox = ["localhost", "127.0.0.1", "[::1]", "::1"].includes(location.hostname);

async function zap(ch) {
	await api("zap", { method: "POST", body: { channel_id: ch.id, stop_playback: true } });
	toast(`Zapped to ${ch.name}`);
}

/* An Android phone hands the stream to a video player app; elsewhere an
 * mpv: link starts mpv, where a handler for it is set up (mpv-url.desktop).
 * The address goes to the clipboard as well, for when nothing opens. */
async function stream(id) {
	const s = await api(`stream/${id}`);
	const m = s.url.match(/^(https?):\/\/(.*)$/);
	if (/Android/i.test(navigator.userAgent) && m) {
		location.href = `intent://${m[2]}#Intent;scheme=${m[1]};action=android.intent.action.VIEW;type=video/*;end`;
		return;
	}
	let copied = false;
	try {
		await navigator.clipboard.writeText(s.url);
		copied = true;
	} catch { /* no clipboard here */ }
	location.href = `mpv:${encodeURIComponent(s.url)}`;
	toast(copied ? "Starting mpv - if nothing opens, the address is in the clipboard" : "Starting mpv");
}

/* ------------------------------------------------------------------ */
/* views                                                               */

let cleanup = null;

async function viewRemote(main) {
	const osd = h("div", { class: "osd" }, h("span", { class: "hint" }, "The OSD of the box shows here after a key"));
	const pad = h("div", { class: "remote" });
	main.append(h("h1", {}, "Remote control"),
		h("div", { class: "remote-view" }, pad,
			h("div", { class: "osd-col" }, osd,
				h("p", { class: "dim small" }, "Keyboard: arrows, Enter = OK, Esc = Exit, 0-9, + / - volume, r g y b colour keys. ",
					h("a", { href: "/remote.html" }, "Remote only"), " fills a phone screen."))));

	/* the box answers a key before it paints, so the OSD is fetched right
	 * away and again while it settles; one request at a time, and a picture
	 * never replaces a newer one */
	let timers = [];
	let busy = false, again = false, asked = 0, shown = 0;
	const shot = () => {
		if (busy) {
			again = true;
			return;
		}
		busy = true;
		again = false;
		const n = ++asked;
		const img = new Image();
		const done = () => {
			busy = false;
			if (again)
				shot();
		};
		img.onload = () => {
			if (n > shown) {
				shown = n;
				const c = document.createElement("canvas");
				c.width = 32; c.height = 18;
				const g = c.getContext("2d");
				g.drawImage(img, 0, 0, 32, 18);
				const empty = !g.getImageData(0, 0, 32, 18).data.some((v, i) => i % 4 === 3 && v > 0);
				osd.replaceChildren(empty ? h("span", { class: "hint" }, "No menu on the screen") : img);
			}
			done();
		};
		img.onerror = done;
		img.src = osdUrl();
	};
	const follow = () => {
		timers.forEach(clearTimeout);
		shot();
		timers = [150, 450, 2000].map((t) => setTimeout(shot, t));
	};
	const press = (name) => key(name).then(follow, fail);

	drawRemote(pad, await loadLayout(), press);
	const onkey = (ev) => {
		if (ev.target.closest("input, textarea, select") || ev.ctrlKey || ev.metaKey || ev.altKey)
			return;
		const name = KEYBOARD[ev.key];
		if (name) {
			ev.preventDefault();
			press(name);
		}
	};
	document.addEventListener("keydown", onkey);
	cleanup = () => {
		document.removeEventListener("keydown", onkey);
		timers.forEach(clearTimeout);
	};
}

async function viewChannels(main) {
	const bouquets = await tvBouquets();
	if (!bouquets.length) {
		main.append(h("div", { class: "empty" }, "No channels"));
		return;
	}
	const list = h("div", { class: "list" });
	let current = "";
	const show = async (id) => {
		saveBouquet(id);
		list.replaceChildren(h("div", { class: "empty" }, "Loading ..."));
		const from = Math.floor(now());
		const [channels, grid, cur] = await Promise.all([
			all(`channels?bouquet=${id}&limit=500`),
			all(`epg/grid?bouquet=${id}&from=${from}&to=${from + 6 * 3600}&limit=100`).catch(() => []),
			api("channels/current").catch(() => null),
		]);
		current = cur?.id || "";
		const events = new Map(grid.map((r) => [r.channel_id, r.events || []]));
		const t = now();
		list.replaceChildren(...channels.map((ch) => {
			const evs = (events.get(ch.id) || []).filter((e) => e.start + e.duration > t);
			const nowEv = evs.find((e) => e.start <= t);
			const nextEv = evs.find((e) => e.start > t);
			const pct = nowEv ? Math.min(100, Math.max(0, (t - nowEv.start) / nowEv.duration * 100)) : 0;
			const logo = h("img", { class: "logo", src: `/api/v1/channels/${ch.id}/logo`, alt: "", loading: "lazy" });
			logo.onerror = () => { logo.style.visibility = "hidden"; };
			const row = h("div", { class: `channel${ch.id === current ? " current" : ""}`, title: nowEv?.description || "" },
				h("span", { class: "num" }, ch.number),
				logo,
				h("div", { class: "info" },
					h("div", { class: "name" }, ch.name),
					nowEv ? h("div", { class: "now" }, `${hm(nowEv.start)} ${nowEv.title}`) : h("div", { class: "now dim" }, "No programme information"),
					/* hidden where there is nothing to show, so that every row is as high */
					h("div", { class: "progress", style: nowEv ? "" : "visibility:hidden" }, h("span", { style: `width:${pct}%` })),
					h("div", { class: "next", style: nextEv ? "" : "visibility:hidden" }, nextEv ? `${hm(nextEv.start)} ${nextEv.title}` : "-")),
				h("div", { class: "actions" },
					may("write") && nowEv ? h("button", { class: "btn icon", title: "Record this programme", html: icon("record"),
						onclick: (ev) => { ev.stopPropagation(); record(ch.id, nowEv).catch(fail); } }) : null,
					onBox ? null : h("button", { class: "btn icon", title: "Watch as stream", html: icon("stream"),
						onclick: (ev) => { ev.stopPropagation(); stream(ch.id).catch(fail); } })));
			row.addEventListener("click", async () => {
				try {
					await api("zap", { method: "POST", body: { channel_id: ch.id, stop_playback: true } });
					list.querySelector(".channel.current")?.classList.remove("current");
					row.classList.add("current");
					toast(`Zapped to ${ch.name}`);
				} catch (e) { fail(e); }
			});
			return row;
		}));
		list.querySelector(".channel.current")?.scrollIntoView({ block: "center" });
	};
	const sel = bouquetSelect(bouquets, savedBouquet(bouquets), (id) => show(id).catch(fail));
	main.append(h("div", { class: "toolbar" }, h("h1", {}, "Channels"), sel,
		h("button", { class: "btn icon", title: "Reload", html: icon("refresh"), onclick: () => show(sel.value).catch(fail) })), list);
	await show(sel.value);
}

const PX_PER_MIN = 4;

/* el is the programme in the guide; el.timerId the recording set for it */
function eventDialog(ch, ev, el) {
	const end = ev.start + ev.duration;
	const running = ev.start <= now() && now() < end;
	const timerId = el?.timerId;
	let timerButton = null;
	if (may("write") && timerId !== undefined)
		timerButton = h("button", { class: "btn danger", onclick: async () => {
			d.close();
			/* a timer to come is set again as fast; a recording stopped is gone */
			if (running && !await confirmDialog(`Stop the recording of "${ev.title}"?`, "Stop recording", true))
				return;
			try {
				await api(`timers/${timerId}`, { method: "DELETE" });
				el.classList.remove("timer");
				delete el.timerId;
				toast(running ? `Recording stopped: ${ev.title}` : `Timer deleted: ${ev.title}`);
			} catch (e) { fail(e); }
		} }, running ? "Stop recording" : "Delete timer");
	else if (may("write"))
		timerButton = h("button", { class: "btn primary", onclick: () => {
			record(ch.id, ev).then((r) => {
				if (el && r?.id !== undefined) {
					el.classList.add("timer");
					el.timerId = r.id;
				}
			}).catch(fail);
			d.close();
		} }, "Record");
	const d = h("dialog", {},
		h("h2", {}, ev.title),
		h("p", { class: "dim small" }, `${ch.name} - ${day(ev.start)} ${hm(ev.start)}-${hm(ev.start + ev.duration)}`),
		ev.description ? h("p", {}, ev.description) : null,
		h("div", { class: "buttons" },
			h("button", { class: "btn", onclick: () => d.close() }, "Close"),
			onBox ? null : h("button", { class: "btn", onclick: () => { stream(ch.id).catch(fail); d.close(); } }, "Stream"),
			may("write") ? h("button", { class: "btn", onclick: () => { zap(ch).catch(fail); d.close(); } }, "Zap") : null,
			timerButton));
	d.addEventListener("close", () => d.remove());
	document.body.append(d);
	d.showModal();
}

async function viewEpg(main) {
	const bouquets = await tvBouquets();
	if (!bouquets.length) {
		main.append(h("div", { class: "empty" }, "No channels"));
		return;
	}
	const box = h("div", { class: "epg" });
	const show = async (id) => {
		saveBouquet(id);
		box.replaceChildren(h("div", { class: "empty" }, "Loading ..."));
		const start = Math.floor(now() / 1800) * 1800;
		/* as many hours as the window shows, and two more to scroll to */
		const hours = Math.min(24, Math.max(8, Math.ceil((box.clientWidth - 150) / (60 * PX_PER_MIN)) + 2));
		const [channels, grid, timers] = await Promise.all([
			all(`channels?bouquet=${id}&limit=500`),
			all(`epg/grid?bouquet=${id}&from=${start}&to=${start + hours * 3600}&limit=100`),
			api("timers").then((r) => r.items || [], () => []),
		]);
		/* a programme a recording is set for: by its event, or covered more than half */
		const records = timers.filter((tm) => tm.kind === "record");
		const recordFor = (ch, ev, end) => records.find((tm) => tm.channel_id === ch.id &&
			((tm.epg_id && tm.epg_id === ev.id) || Math.min(tm.stop, end) - Math.max(tm.start, ev.start) > (end - ev.start) / 2));
		const events = new Map(grid.map((r) => [r.channel_id, r.events || []]));
		const width = hours * 60 * PX_PER_MIN;
		const x = (t) => (t - start) / 60 * PX_PER_MIN;
		const times = h("div", { class: "times", style: `margin-left:150px;width:${width}px` });
		for (let t = start; t < start + hours * 3600; t += 1800)
			times.append(h("span", { style: `left:${x(t) + 4}px` }, hm(t)));
		const grid_el = h("div", { class: "grid", style: `width:${width + 150}px` }, times);
		const t = now();
		for (const ch of channels) {
			const line = h("div", { class: "events", style: `width:${width}px` });
			/* the guide may let one programme run into the next; each
			 * ends where the next one starts, so none covers another */
			const evs = (events.get(ch.id) || []).slice().sort((a, b) => a.start - b.start);
			evs.forEach((ev, i) => {
				const next = evs[i + 1];
				const end = next ? Math.min(ev.start + ev.duration, next.start) : ev.start + ev.duration;
				const left = Math.max(0, x(ev.start));
				const right = Math.min(width, x(end));
				if (right - left < 2)
					return;
				const running = ev.start <= t && t < end;
				/* a click on what runs now plays it (on the box itself: zaps
				 * to it), the right button or a long press opens the choices */
				const direct = running && (!onBox || may("write"));
				const timer = recordFor(ch, ev, end);
				const el = h("div", { class: `event${running ? " running" : ""}${timer ? " timer" : ""}`, style: `left:${left}px;width:${right - left - 2}px`,
					title: `${hm(ev.start)} ${ev.title}${direct ? (onBox ? "\nClick: zap - right click: more" : "\nClick: stream - right click: more") : ""}`,
					onclick: () => {
						if (!direct)
							eventDialog(ch, ev, el);
						else if (onBox)
							zap(ch).catch(fail);
						else
							stream(ch.id).catch(fail);
					},
					oncontextmenu: (e) => { e.preventDefault(); eventDialog(ch, ev, el); } }, ev.title);
				if (timer)
					el.timerId = timer.id;
				line.append(el);
			});
			/* the channel logo, the name where there is none */
			const head = h("div", { class: "rowh logo-head", title: ch.name });
			const logo = h("img", { src: `/api/v1/channels/${ch.id}/logo`, alt: ch.name, loading: "lazy" });
			logo.onerror = () => { head.classList.remove("logo-head"); head.replaceChildren(ch.name); };
			head.append(logo);
			grid_el.append(h("div", { class: "line" }, head, line));
		}
		grid_el.append(h("div", { class: "nowline", style: `left:${150 + x(t)}px` }));
		box.replaceChildren(grid_el);
	};
	const sel = bouquetSelect(bouquets, savedBouquet(bouquets), (id) => show(id).catch(fail));
	main.append(h("div", { class: "toolbar" }, h("h1", {}, "Programme guide"), sel), box);
	await show(sel.value);
}

const TIMER_KINDS = { record: "Recording", zapto: "Zap", standby: "Standby", shutdown: "Shutdown",
	remind: "Reminder", sleeptimer: "Sleep timer", exec_plugin: "Plugin", next_program: "Next programme" };

async function viewTimers(main) {
	const list = h("div", { class: "list" });
	main.append(h("div", { class: "toolbar" }, h("h1", {}, "Timers")), list);
	const show = async () => {
		const items = ((await api("timers")).items || []).sort((a, b) => a.start - b.start);
		if (!items.length) {
			list.replaceChildren(h("div", { class: "empty" }, "No timers"));
			return;
		}
		const rows = [];
		for (const t of items) {
			const channel = t.channel_id && t.channel_id !== "0" ? await channelName(t.channel_id) : "";
			rows.push(h("div", { class: "card timer" },
				h("div", {},
					h("div", {}, h("strong", {}, t.title || TIMER_KINDS[t.kind] || t.kind)),
					h("div", { class: "dim small" },
						`${TIMER_KINDS[t.kind] || t.kind} - ${day(t.start)} ${hm(t.start)}${t.stop ? `-${hm(t.stop)}` : ""}${channel ? ` - ${channel}` : ""}`)),
				may("write") ? h("button", { class: "btn icon danger", title: "Delete", html: icon("trash"),
					onclick: async () => {
						if (!await confirmDialog(`Delete the timer "${t.title || TIMER_KINDS[t.kind] || t.kind}"?`, "Delete", true))
							return;
						try {
							await api(`timers/${t.id}`, { method: "DELETE" });
							await show();
						} catch (e) { fail(e); }
					} }) : null));
		}
		list.replaceChildren(...rows);
	};
	await show();
}

function size(bytes) {
	const units = ["B", "KB", "MB", "GB", "TB"];
	let i = 0;
	while (bytes >= 1024 && i < units.length - 1) { bytes /= 1024; i++; }
	return `${bytes.toFixed(i ? 1 : 0)} ${units[i]}`;
}

function uptime(s) {
	const d = Math.floor(s / 86400), hh = Math.floor(s % 86400 / 3600), mm = Math.floor(s % 3600 / 60);
	return d ? `${d} d ${hh} h` : `${hh} h ${mm} min`;
}

async function viewBox(main) {
	const [info, standby] = await Promise.all([api("system/info"), api("system/standby").catch(() => null)]);
	const cards = h("div", { class: "cards" });
	main.append(h("h1", {}, "Box"), cards);

	cards.append(h("div", { class: "card" }, h("h2", {}, info.hostname || "Box"),
		h("dl", { class: "facts" },
			h("dt", {}, "Model"), h("dd", {}, `${info.vendor} ${info.model}`.trim()),
			h("dt", {}, "Image"), h("dd", {}, info.image_version || "-"),
			h("dt", {}, "Kernel"), h("dd", {}, info.kernel || "-"),
			h("dt", {}, "Running"), h("dd", {}, uptime(info.uptime || 0)),
			h("dt", {}, "Memory"), h("dd", {}, `${size(info.memory_free)} free of ${size(info.memory_total)}`),
			h("dt", {}, "Storage"), h("dd", {}, `${size(info.root_free)} free of ${size(info.root_total)}`))));

	if (may("write")) {
		const text = h("textarea", { maxlength: 1024, placeholder: "Text for the TV screen", "aria-label": "Message" });
		const popup = h("input", { type: "checkbox" });
		cards.append(h("div", { class: "card" }, h("h2", {}, "Message"),
			h("form", { onsubmit: async (ev) => {
				ev.preventDefault();
				if (!text.value.trim())
					return;
				try {
					await api("system/message", { method: "POST", body: { text: text.value, popup: popup.checked } });
					text.value = "";
					toast("Message sent");
				} catch (e) { fail(e); }
			} }, text,
				h("div", { class: "row", style: "margin-top:10px" },
					h("label", { class: "check" }, popup, "Wait for OK"),
					h("span", { style: "flex:1" }),
					h("button", { class: "btn primary" }, "Send")))));
	}

	if (may("system")) {
		const power = async (action, question) => {
			if (!await confirmDialog(question, "Yes", true))
				return;
			try {
				await api("system/power", { method: "POST", body: { action } });
				toast("Done");
			} catch (e) { fail(e); }
		};
		let asleep = !!standby?.on;
		const sb = h("button", { class: "btn", onclick: async () => {
			try {
				await api("system/standby", { method: "POST", body: { on: !asleep } });
				asleep = !asleep;
				sb.textContent = asleep ? "Wake up" : "Standby";
			} catch (e) { fail(e); }
		} }, asleep ? "Wake up" : "Standby");
		cards.append(h("div", { class: "card" }, h("h2", {}, "Power"),
			h("div", { class: "row" }, sb,
				h("button", { class: "btn", onclick: () => power("restart", "Restart Neutrino?") }, "Restart Neutrino"),
				h("button", { class: "btn danger", onclick: () => power("reboot", "Reboot the box?") }, "Reboot"),
				h("button", { class: "btn danger", onclick: () => power("shutdown", "Switch the box off?") }, "Shut down"))));
	}

	const s = await session();
	const account = h("div", { class: "card" }, h("h2", {}, "This browser"),
		h("p", { class: "dim small" }, `Access: ${s.level}${s.authenticated ? ` as ${s.user}` : s.device ? ` as paired device "${s.device}"` : " from the home network"}`));
	const buttons = h("div", { class: "row" });
	if (s.authenticated)
		buttons.append(h("button", { class: "btn", onclick: async () => {
			try { await api("logout", { method: "POST" }); } catch { /* gone anyway */ }
			location.hash = "#/signin";
		} }, "Sign out"));
	else if (s.device)
		buttons.append(h("button", { class: "btn", onclick: async () => {
			if (!await confirmDialog("Unpair this browser? It needs to be paired again to work as a remote control.", "Unpair", true))
				return;
			try { await api("pair/forget", { method: "POST" }); } catch (e) { fail(e); return; }
			await session();
			location.hash = "#/signin";
		} }, "Unpair"));
	else if (s.secure)
		buttons.append(h("a", { class: "btn", href: "#/signin" }, "Pair with the TV"));
	else if (httpsUrl())
		buttons.append(h("a", { class: "btn", href: httpsUrl("/#/signin") }, "Switch to HTTPS"));
	buttons.append(h("a", { class: "btn", href: "/api/v1/system/ca", title: "Install it on a phone to trust the box without a warning" }, "Certificate of the box"));
	account.append(buttons);
	cards.append(account);

	if (may("system")) {
		cards.append(await devicesCard());
		const mcp = await api("mcp").catch(() => null);
		if (mcp?.available)
			cards.append(mcpCard(mcp));
	}
}

/* the MCP server of the assistant, for claude.ai and other MCP clients */
function mcpCard(state) {
	const card = h("div", { class: "card" });
	const show = (st) => {
		const enabled = h("input", { type: "checkbox", checked: st.enabled });
		enabled.addEventListener("change", () => change({ enabled: enabled.checked }));
		const url = h("input", { type: "text", value: st.public_url, placeholder: "https://...", "aria-label": "Public address", style: "flex:1;min-width:0" });
		const fresh = st.new_token ? h("div", { class: "row" },
			h("code", { style: "overflow-wrap:anywhere" }, st.new_token),
			h("button", { class: "btn", onclick: async () => {
				try { await navigator.clipboard.writeText(st.new_token); toast("Token copied"); } catch { /* shown above */ }
			} }, "Copy")) : null;
		const now = Date.now() / 1000;
		const clients = (st.clients || []).filter((c) => c.signed_in_until > now);
		card.replaceChildren(h("div", {}, h("h2", {}, "MCP server"),
			h("label", { class: "check" }, enabled, "Open for MCP clients such as claude.ai"),
			h("dl", { class: "facts", style: "margin-top:10px" },
				h("dt", {}, "Address"), h("dd", {}, st.address || "-"),
				h("dt", {}, "Token"), h("dd", {}, st.token || "-")),
			fresh ? h("p", { class: "small" }, "The new token, shown only now:") : null, fresh,
			h("div", { class: "row", style: "margin-top:10px" },
				h("button", { class: "btn", onclick: async () => {
					if (await confirmDialog("A new token ends the old one; clients that use it need the new one.", "New token", true))
						change({ new_token: true });
				} }, "New token")),
			h("p", { class: "dim small" }, "Public address: where clients from the internet reach the box (a tunnel such as Tailscale Funnel). Everything but MCP stays closed there."),
			h("div", { class: "row" }, url, h("button", { class: "btn", onclick: () => change({ public_url: url.value.trim() }) }, "Save")),
			h("h2", { style: "margin-top:16px" }, "Signed-in clients"),
			clients.length ? h("div", { class: "list" }, clients.map((c) => h("div", { class: "timer" },
				h("div", {}, h("div", {}, c.name || c.id), h("div", { class: "dim small" }, `${c.host} - until ${day(c.signed_in_until)}`)),
				h("button", { class: "btn", onclick: async () => {
					if (!await confirmDialog(`Sign "${c.name || c.id}" out?`, "Sign out", true))
						return;
					try {
						await api(`mcp/clients/${encodeURIComponent(c.id)}`, { method: "DELETE" });
						show(await api("mcp"));
					} catch (e) { fail(e); }
				} }, "Sign out")))) : h("p", { class: "dim small" }, "No client is signed in."),
			clients.length > 1 ? h("button", { class: "btn danger", style: "margin-top:8px", onclick: async () => {
				if (!await confirmDialog("Sign every client out?", "Sign out all", true))
					return;
				try {
					await api("mcp/clients", { method: "DELETE" });
					show(await api("mcp"));
				} catch (e) { fail(e); }
			} }, "Sign out all") : null));
	};
	const change = async (body) => {
		try {
			show(await api("mcp", { method: "POST", body }));
			toast("Saved");
		} catch (e) { fail(e); }
	};
	show(state);
	return card;
}

async function devicesCard() {
	const card = h("div", { class: "card" }, h("h2", {}, "Paired devices"));
	const list = h("div", { class: "list" });
	card.append(list);
	const show = async () => {
		const items = (await api("pair/devices")).items || [];
		if (!items.length) {
			list.replaceChildren(h("p", { class: "dim small" }, "No device is paired. Pair a phone with the code on the TV: Network > Web server > Pair a phone."));
			return;
		}
		list.replaceChildren(...items.sort((a, b) => b.last_used - a.last_used).map((d) =>
			h("div", { class: "timer" },
				h("div", {}, h("div", {}, d.name), h("div", { class: "dim small" }, `last used ${day(d.last_used)} ${hm(d.last_used)}`)),
				h("button", { class: "btn icon danger", title: "Remove", html: icon("trash"), onclick: async () => {
					if (!await confirmDialog(`Remove "${d.name}"?`, "Remove", true))
						return;
					try {
						await api(`pair/devices/${d.id}`, { method: "DELETE" });
						await show();
					} catch (e) { fail(e); }
				} }))));
	};
	await show();
	return card;
}

function pairCard(s) {
	const card = h("div", { class: "card signin" }, h("h1", {}, "Pair with the TV"));
	if (!s.secure) {
		const url = httpsUrl("/#/signin");
		card.append(h("p", { class: "dim" }, "Pairing works over HTTPS. The browser warns once about the certificate of the box; ",
			h("a", { href: "/api/v1/system/ca" }, "install it"), " to avoid that."),
			url ? h("a", { class: "btn primary", href: url }, "Switch to HTTPS") : h("p", { class: "error small" }, "HTTPS is off on this box."));
		return card;
	}
	const out = h("p", { class: "dim" }, "On the TV: Network > Web server > Pair a phone shows a code to scan. Or ask the TV from here:");
	const ask = h("button", { class: "btn primary", onclick: async () => {
		ask.disabled = true;
		try {
			await pairOnTv((digits) => { out.textContent = `Allow this browser on the TV. The TV shows the code ${digits}.`; });
			location.hash = "#/remote";
		} catch (e) {
			out.textContent = e.message || "The TV did not allow it.";
			ask.disabled = false;
		}
	} }, "Ask the TV");
	card.append(out, ask);
	return card;
}

async function viewSignIn(main) {
	const s = await session().catch(() => ({ level: "public" }));
	main.append(pairCard(s));
	const user = h("input", { type: "text", autocomplete: "username", value: "root", "aria-label": "Name" });
	const pass = h("input", { type: "password", autocomplete: "current-password", "aria-label": "Password", placeholder: "Password" });
	const msg = h("p", { class: "error small" });
	main.append(h("div", { class: "card signin" },
		h("h1", {}, "Sign in with a password"),
		s.level !== "public" ? h("p", { class: "dim" }, `This browser already has ${s.level} access. `, h("a", { href: "#/remote" }, "Continue")) : null,
		h("form", { onsubmit: async (ev) => {
			ev.preventDefault();
			msg.textContent = "";
			try {
				await api("login", { method: "POST", body: { user: user.value, password: pass.value } });
				await session();
				location.hash = "#/remote";
			} catch (e) {
				msg.textContent = e.code === "authentication-off"
					? "Signing in with a password is off on this box; the home network has access without one."
					: e.message;
			}
		} }, user, pass, h("button", { class: "btn primary" }, "Sign in"), msg)));
}

/* ------------------------------------------------------------------ */
/* drives and network shares                                           */

const SHARE_TYPES = { smb: "SMB", nfs: "NFS", ftp: "FTP" };

function usage(d) {
	if (!d.size)
		return null;
	const used = Math.round(100 * (d.size - d.free) / d.size);
	return h("div", {},
		h("div", { class: "progress" }, h("span", { style: `width:${used}%` })),
		h("div", { class: "dim small" }, `${size(d.free)} free of ${size(d.size)}`));
}

function holds(dir, path) {
	if (!dir || !path)
		return false;
	return path === dir || path.startsWith(dir.endsWith("/") ? dir : `${dir}/`);
}

function shareLabel(s) {
	return s.type === "nfs" ? `${s.host}:${s.share}` : `//${s.host}/${s.share}`;
}

function localDirFor(share) {
	return `/media/net/${share.replace(/^\/+/, "").replace(/[^\w.-]+/g, "_") || "share"}`;
}

/* asks for what a share entry holds; resolves to the body, or null on cancel */
function shareDialog(title, init, isNew) {
	return new Promise((resolve) => {
		const type = h("select", { "aria-label": "Type" },
			h("option", { value: "smb" }, "SMB (Windows, Fritz!Box, most NAS)"),
			h("option", { value: "nfs" }, "NFS (Linux, NAS)"));
		type.value = init.type || "smb";
		const field = (label, input) => h("label", { class: "field" }, h("span", { class: "dim small" }, label), input);
		const shareName = h("span", { class: "dim small" });
		const host = h("input", { type: "text", required: true, value: init.host || "", placeholder: "192.168.178.1 or nas" });
		const share = h("input", { type: "text", required: true, value: init.share || "" });
		const dir = h("input", { type: "text", required: true, value: init.local_dir || "" });
		const user = h("input", { type: "text", value: init.username || "", autocomplete: "off" });
		const pass = h("input", { type: "password", value: init.password || "", autocomplete: "new-password",
			placeholder: init.has_password ? "unchanged" : "" });
		const options = h("input", { type: "text", value: init.options || "", placeholder: "rw" });
		const auto = h("input", { type: "checkbox" });
		auto.checked = init.automount !== false;
		const record = h("input", { type: "checkbox" });
		const login = h("div", { class: "fields" }, field("User", user), field("Password", pass));
		const sync = () => {
			login.hidden = type.value === "nfs";
			share.placeholder = type.value === "nfs" ? "/export/path" : "share name";
			shareName.textContent = type.value === "nfs" ? "Export" : "Share";
		};
		type.addEventListener("change", sync);
		sync();
		if (!init.local_dir)
			share.addEventListener("input", () => { dir.value = localDirFor(share.value); });

		const form = h("form", { method: "dialog", class: "fields" },
			h("h2", {}, title),
			field("Type", type), field("Server", host), h("label", { class: "field" }, shareName, share),
			login, field("Mount at", dir), field("Options", options),
			h("label", { class: "check" }, auto, "Mount when the box starts"),
			isNew ? h("label", { class: "check" }, record, "Record here") : null,
			h("div", { class: "buttons" },
				h("button", { class: "btn", type: "button", onclick: () => d.close() }, "Cancel"),
				h("button", { class: "btn primary", value: "ok" }, isNew ? "Add and mount" : "Save")));
		const d = h("dialog", {}, form);
		d.addEventListener("close", () => {
			d.remove();
			if (d.returnValue !== "ok")
				return resolve(null);
			const body = { type: type.value, host: host.value.trim(), share: share.value.trim(),
				local_dir: dir.value.trim(), automount: auto.checked, options: options.value.trim() };
			if (type.value === "smb") {
				body.username = user.value.trim();
				if (pass.value || !init.has_password || !body.username)
					body.password = pass.value;
			}
			resolve({ body, record: record.checked });
		});
		document.body.append(d);
		d.showModal();
	});
}

function loginDialog(host) {
	return new Promise((resolve) => {
		const user = h("input", { type: "text", required: true, autocomplete: "off", "aria-label": "User" });
		const pass = h("input", { type: "password", autocomplete: "new-password", "aria-label": "Password" });
		const d = h("dialog", {}, h("form", { method: "dialog", class: "fields" },
			h("h2", {}, `Sign in to ${host}`),
			h("label", { class: "field" }, h("span", { class: "dim small" }, "User"), user),
			h("label", { class: "field" }, h("span", { class: "dim small" }, "Password"), pass),
			h("div", { class: "buttons" },
				h("button", { class: "btn", type: "button", onclick: () => d.close() }, "Cancel"),
				h("button", { class: "btn primary", value: "ok" }, "List shares"))));
		d.addEventListener("close", () => {
			d.remove();
			resolve(d.returnValue === "ok" ? { username: user.value.trim(), password: pass.value } : null);
		});
		document.body.append(d);
		d.showModal();
	});
}

async function viewMounts(main) {
	const drives = h("div", { class: "cards" });
	const shares = h("div", { class: "list" });
	const found = h("div", { class: "list" });
	let state = null;

	const run = async (btn, fn, done) => {
		if (btn)
			btn.disabled = true;
		try {
			const result = await fn();
			if (done)
				toast(typeof done === "function" ? done(result) : done);
			await show();
		} catch (e) {
			fail(e);
		} finally {
			if (btn)
				btn.disabled = false;
		}
	};
	const recordHere = (path) => api("mounts/record", { method: "POST", body: { path } });
	const recordButton = (dir, target, writable) => {
		if (!may("system") || !dir || !writable || holds(dir, state.record_dir))
			return null;
		return h("button", { class: "btn", onclick: (ev) => run(ev.currentTarget,
			() => recordHere(target), "Recordings go here") }, "Record here");
	};
	/* unmounting the record target moves the recordings; the box says where */
	const unmounted = (what) => (r) => r?.record_dir ? `${what} unmounted, recordings go to ${r.record_dir} again` : `${what} unmounted`;
	const marker = (dir) => holds(dir, state.record_dir)
		? h("span", { class: "badge", title: state.record_dir, html: `${icon("record")}Recordings` }) : null;

	const addShare = async (init) => {
		const r = await shareDialog("Add a network share", init, true);
		if (!r)
			return;
		await run(null, async () => {
			const s = await api("mounts/shares", { method: "POST", body: r.body });
			try {
				await api(`mounts/shares/${s.nr}/mount`, { method: "POST" });
			} catch (e) {
				/* an entry that never mounted is not kept */
				await api(`mounts/shares/${s.nr}`, { method: "DELETE" }).catch(() => {});
				throw e;
			}
			if (r.record)
				await recordHere(s.local_dir);
		}, "Share mounted");
	};

	const renderDrives = () => {
		if (!state.drives.length) {
			drives.replaceChildren(h("div", { class: "card empty" }, "No drives"));
			return;
		}
		drives.replaceChildren(...state.drives.map((d) => {
			const buttons = h("div", { class: "row", style: "margin-top:10px" });
			if (state.can_mount && may("write"))
				buttons.append(d.mounted
					? h("button", { class: "btn", onclick: (ev) => run(ev.currentTarget,
						() => api(`mounts/drives/${d.name}/umount`, { method: "POST" }), unmounted("Drive")) }, "Unmount")
					: h("button", { class: "btn", onclick: (ev) => run(ev.currentTarget,
						() => api(`mounts/drives/${d.name}/mount`, { method: "POST" }), "Drive mounted") }, "Mount"));
			if (d.mounted && d.mount_point) {
				const b = recordButton(d.mount_point, `${d.mount_point}/movies`, d.writable);
				if (b)
					buttons.append(b);
			}
			return h("div", { class: "card" },
				h("div", { class: "row" }, h("h2", { style: "margin:0;flex:1" }, d.label || d.name), marker(d.mount_point)),
				h("div", { class: "dim small" }, [d.name, d.fs, d.mounted ? d.mount_point || "mounted" : "not mounted"].filter(Boolean).join(" · ")),
				usage(d), buttons.childElementCount ? buttons : null);
		}));
	};

	const renderShares = () => {
		if (!state.shares.length) {
			shares.replaceChildren(h("div", { class: "cards" },
				h("div", { class: "card empty" }, state.can_mount ? "No network shares" : "No network shares mounted")));
			return;
		}
		shares.replaceChildren(...state.shares.map((s) => {
			const buttons = h("div", { class: "row" });
			if (!s.managed) {
				buttons.append(recordButton(s.local_dir, s.local_dir, s.writable) || "");
				return h("div", { class: "card share" },
					h("div", {},
						h("div", { class: "row" }, h("strong", {}, shareLabel(s)),
							h("span", { class: "badge on" }, "Mounted by the desktop"), marker(s.local_dir)),
						h("div", { class: "dim small" }, `${SHARE_TYPES[s.type] || s.type} · ${s.local_dir}`),
						usage(s)),
					buttons);
			}
			if (may("write"))
				buttons.append(s.mounted
					? h("button", { class: "btn", onclick: (ev) => run(ev.currentTarget,
						() => api(`mounts/shares/${s.nr}/umount`, { method: "POST" }), unmounted("Share")) }, "Unmount")
					: h("button", { class: "btn", onclick: (ev) => run(ev.currentTarget,
						() => api(`mounts/shares/${s.nr}/mount`, { method: "POST" }), "Share mounted") }, "Mount"));
			if (may("system")) {
				if (s.mounted)
					buttons.append(recordButton(s.local_dir, s.local_dir, s.writable) || "");
				if (!s.mounted)
					buttons.append(h("button", { class: "btn", onclick: async () => {
						const r = await shareDialog("Change the share", s, false);
						if (r)
							await run(null, () => api(`mounts/shares/${s.nr}`, { method: "POST", body: r.body }), "Saved");
					} }, "Edit"));
				buttons.append(h("button", { class: "btn icon danger", title: "Remove", html: icon("trash"),
					onclick: async (ev) => {
						if (await confirmDialog(`Remove ${shareLabel(s)}?`, "Remove", true))
							await run(ev.currentTarget, () => api(`mounts/shares/${s.nr}`, { method: "DELETE" }),
								(r) => r?.record_dir ? `Share removed, recordings go to ${r.record_dir} again` : "Share removed");
					} }));
			}
			return h("div", { class: "card share" },
				h("div", {},
					h("div", { class: "row" }, h("strong", {}, shareLabel(s)),
						h("span", { class: `badge ${s.mounted ? "on" : ""}` }, s.mounted ? "Mounted" : "Not mounted"),
						h("span", { class: "badge" }, s.automount ? "Mounts at boot" : "Manual mount"),
						marker(s.local_dir)),
					h("div", { class: "dim small" }, `${SHARE_TYPES[s.type] || s.type} · ${s.local_dir}`),
					s.mounted ? usage(s) : null),
				buttons);
		}));
	};

	const show = async () => {
		state = await api("mounts");
		renderDrives();
		renderShares();
	};

	const browse = async (host, type, row) => {
		const label = host.name || host.ip;
		const out = h("div", { class: "list" }, h("div", { class: "dim small" }, "Asking the server..."));
		row.querySelector(".shares")?.remove();
		out.classList.add("shares");
		row.append(out);
		let creds = {};
		for (;;) {
			try {
				const items = (await api("mounts/browse", { method: "POST", body: { host: host.ip, type, ...creds } })).items || [];
				if (!items.length) {
					out.replaceChildren(h("div", { class: "dim small" }, "The server lists no shares"));
					return;
				}
				out.replaceChildren(...items.map((it) => h("div", { class: "row" },
					h("span", { style: "flex:1" }, h("strong", {}, it.name), it.comment ? h("span", { class: "dim small" }, ` ${it.comment}`) : null),
					h("button", { class: "btn", onclick: () => addShare({ type, host: host.ip, share: it.name,
						local_dir: localDirFor(it.name), ...creds }) }, "Add"))));
				return;
			} catch (e) {
				if (e instanceof ApiError && e.code === "login-needed") {
					const c = await loginDialog(label);
					if (!c) {
						out.remove();
						return;
					}
					creds = c;
					continue;
				}
				if (e instanceof ApiError && e.code === "no-share-list") {
					out.replaceChildren(h("div", { class: "row" },
						h("span", { class: "dim small", style: "flex:1" }, e.message),
						h("button", { class: "btn", onclick: () => addShare({ type, host: host.ip }) }, "Add by name")));
					return;
				}
				out.remove();
				fail(e);
				return;
			}
		}
	};

	const search = async (btn) => {
		btn.disabled = true;
		found.replaceChildren(h("div", { class: "empty" }, "Searching the network..."));
		try {
			const hosts = (await api("mounts/scan", { method: "POST" })).items || [];
			if (!hosts.length) {
				found.replaceChildren(h("div", { class: "empty" }, "No SMB or NFS server answered"));
				return;
			}
			found.replaceChildren(h("h2", { style: "margin-top:20px" }, "Servers in the network"), ...hosts.map((host) => {
				const row = h("div", { class: "card" });
				row.append(h("div", { class: "row" },
					h("span", { style: "flex:1" }, h("strong", {}, host.name || host.ip), host.name ? h("span", { class: "dim small" }, ` ${host.ip}`) : null),
					host.smb ? h("button", { class: "btn", onclick: () => browse(host, "smb", row) }, "SMB shares") : null,
					host.nfs ? h("button", { class: "btn", onclick: () => browse(host, "nfs", row) }, "NFS exports") : null));
				return row;
			}));
		} catch (e) {
			found.replaceChildren();
			fail(e);
		} finally {
			btn.disabled = false;
		}
	};

	await show();
	/* a Neutrino without root (a desktop) shows what the desktop mounted and mounts nothing itself */
	const manage = state.can_mount && may("system");
	main.append(
		h("div", { class: "toolbar" }, h("h1", {}, "Mounts"),
			h("button", { class: "btn icon", title: "Refresh", html: icon("refresh"), onclick: () => show().catch(fail) })),
		h("h2", {}, "Drives"), drives,
		h("div", { class: "toolbar", style: "margin-top:24px" }, h("h2", { style: "margin:0;flex:1" }, "Network shares"),
			manage ? h("button", { class: "btn", onclick: () => addShare({}) }, "Add") : null,
			manage ? h("button", { class: "btn primary", onclick: (ev) => search(ev.currentTarget) }, "Find shares") : null),
		state.can_mount ? null : h("p", { class: "dim small" },
			"Drives and shares are mounted by the desktop (file manager or fstab). Once mounted they show up here and can take the recordings."),
		shares, found);

	/* a mount changes behind the page (boot, menu, another browser) */
	const refresh = () => { if (!document.hidden) show().catch(() => {}); };
	const timer = setInterval(refresh, 10000);
	document.addEventListener("visibilitychange", refresh);
	cleanup = () => {
		clearInterval(timer);
		document.removeEventListener("visibilitychange", refresh);
	};
}

/* ------------------------------------------------------------------ */
/* frame and routing                                                   */

const VIEWS = {
	remote: ["Remote", viewRemote, "read"],
	channels: ["Channels", viewChannels, "read"],
	epg: ["Guide", viewEpg, "read"],
	timers: ["Timers", viewTimers, "read"],
	mounts: ["Mounts", viewMounts, "read"],
	box: ["Box", viewBox, "read"],
	signin: ["Sign in", viewSignIn, "public"],
};

/* the picture on the TV with the OSD over it, saved as a PNG file */
async function screenshot(btn) {
	btn.disabled = true;
	try {
		const r = await fetch("/api/v1/osd/screenshot?osd=true&video=true&format=png", { credentials: "same-origin" });
		if (!r.ok) {
			let problem = {};
			try { problem = await r.json(); } catch { /* not JSON */ }
			throw new ApiError(r.status, problem);
		}
		const url = URL.createObjectURL(await r.blob());
		const now = new Date();
		const p = (n) => String(n).padStart(2, "0");
		const a = h("a", { href: url, download: `neutrino-${now.getFullYear()}${p(now.getMonth() + 1)}${p(now.getDate())}-${p(now.getHours())}${p(now.getMinutes())}${p(now.getSeconds())}.png` });
		document.body.append(a);
		a.click();
		a.remove();
		setTimeout(() => URL.revokeObjectURL(url), 10000);
		toast("Screenshot saved");
	} catch (e) {
		fail(e);
	} finally {
		btn.disabled = false;
	}
}

function frame() {
	const nav = h("nav", {}, Object.entries(VIEWS).filter(([k]) => k !== "signin").map(([k, [title]]) =>
		h("a", { href: `#/${k}`, "data-view": k, html: `${icon(k)}<span>${title}</span>` })));
	const app = h("div", { class: "app" },
		h("header", { class: "top" },
			h("div", { class: "brand" }, h("img", { src: "/icons/icon.svg", alt: "" }), "Neutrino"),
			h("span", { class: "box", id: "boxname" }),
			h("span", { class: "spacer" }),
			h("button", { class: "btn icon", title: "Screenshot of the TV", html: icon("camera"), onclick: (ev) => screenshot(ev.currentTarget) }),
			h("a", { class: "btn icon", href: "/remote.html", title: "Remote only (phone)", html: icon("phone") })),
		nav,
		h("main", { id: "main" }));
	document.body.replaceChildren(app);
}

/* A page a plugin adds (ext/<id>/page.js): its default export gets the
 * main element and what it needs to run the command of the extension. */
const extensions = [];

async function viewExtension(main, id) {
	const ext = extensions.find((e) => e.id === id);
	if (!ext) {
		main.append(h("div", { class: "empty" }, "No such page"));
		return;
	}
	const mod = await import(`/ext/${id}/page.js`);
	await mod.default(main, {
		title: ext.title,
		run: async (args) => api(`ext/${id}/run`, { method: "POST", body: { args } }),
		h, icon, toast, fail, confirmDialog,
	});
}

async function loadExtensions() {
	if (!may("read"))
		return;
	try {
		extensions.push(...((await api("ext")).items || []).filter((e) => may(e.level)));
	} catch { /* an older box without extensions */ }
	const box = document.querySelector('nav a[data-view="box"]');
	for (const e of extensions)
		box.before(h("a", { href: `#/ext/${e.id}`, "data-view": `ext/${e.id}`, html: `${icon(e.icon)}<span></span>` }));
	for (const e of extensions)
		document.querySelector(`nav a[data-view="ext/${e.id}"] span`).textContent = e.title;
}

async function route() {
	cleanup?.();
	cleanup = null;
	const ext = (location.hash.match(/^#\/ext\/([\w-]+)/) || [])[1];
	if (ext) {
		for (const a of document.querySelectorAll("nav a"))
			a.classList.toggle("active", a.dataset.view === `ext/${ext}`);
		const main = $("#main");
		main.replaceChildren();
		main.classList.remove("fill");
		try {
			await viewExtension(main, ext);
		} catch (e) {
			fail(e);
		}
		return;
	}
	const name = (location.hash.match(/^#\/(\w+)/) || [])[1] || "remote";
	const [, view, needed] = VIEWS[name] || VIEWS.remote;
	for (const a of document.querySelectorAll("nav a"))
		a.classList.toggle("active", a.dataset.view === name);
	const main = $("#main");
	main.replaceChildren();
	main.classList.toggle("fill", name === "remote");
	if (!may(needed) && name !== "signin") {
		location.hash = "#/signin";
		return;
	}
	try {
		await view(main);
	} catch (e) {
		fail(e);
	}
}

async function start() {
	frame();
	applyTheme();
	try {
		await session();
	} catch (e) {
		toast(e.message, true);
	}
	api("system/info").then((i) => { $("#boxname").textContent = i.hostname || ""; document.title = `${i.hostname || "Neutrino"} - Neutrino`; }, () => {});
	await loadExtensions();
	window.addEventListener("hashchange", route);
	route();
}

start();
