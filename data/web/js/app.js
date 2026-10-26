import { api, all, session, may, key, osdUrl, ApiError, pairOnTv, httpsUrl } from "./api.js";
import { loadLayout, drawRemote, KEYBOARD } from "./remote.js";

const $ = (sel, root = document) => root.querySelector(sel);

const ICONS = {
	remote: '<rect x="7" y="2" width="10" height="20" rx="3"/><circle cx="12" cy="9" r="2.2"/><path d="M10 15h.01M14 15h.01M10 18h.01M14 18h.01"/>',
	channels: '<rect x="3" y="5" width="18" height="12" rx="2"/><path d="M8 21h8M12 17v4"/>',
	epg: '<rect x="3" y="4" width="18" height="17" rx="2"/><path d="M3 9h18M8 2v4M16 2v4M7 13h4M7 17h7"/>',
	timers: '<circle cx="12" cy="13" r="8"/><path d="M12 9v4l2.5 2.5M9 2h6"/>',
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
	await api("timers", { method: "POST", body });
	toast(`Recording set: ${ev.title}`);
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

function eventDialog(ch, ev) {
	const d = h("dialog", {},
		h("h2", {}, ev.title),
		h("p", { class: "dim small" }, `${ch.name} - ${day(ev.start)} ${hm(ev.start)}-${hm(ev.start + ev.duration)}`),
		ev.description ? h("p", {}, ev.description) : null,
		h("div", { class: "buttons" },
			h("button", { class: "btn", onclick: () => d.close() }, "Close"),
			onBox ? null : h("button", { class: "btn", onclick: () => { stream(ch.id).catch(fail); d.close(); } }, "Stream"),
			may("write") ? h("button", { class: "btn", onclick: () => { zap(ch).catch(fail); d.close(); } }, "Zap") : null,
			may("write") ? h("button", { class: "btn primary", onclick: () => { record(ch.id, ev).catch(fail); d.close(); } }, "Record") : null));
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
		const [channels, grid] = await Promise.all([
			all(`channels?bouquet=${id}&limit=500`),
			all(`epg/grid?bouquet=${id}&from=${start}&to=${start + hours * 3600}&limit=100`),
		]);
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
				line.append(h("div", { class: `event${running ? " running" : ""}`, style: `left:${left}px;width:${right - left - 2}px`,
					title: `${hm(ev.start)} ${ev.title}`, onclick: () => eventDialog(ch, ev) }, ev.title));
			});
			grid_el.append(h("div", { class: "line" }, h("div", { class: "rowh", title: ch.name }, ch.name), line));
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
/* frame and routing                                                   */

const VIEWS = {
	remote: ["Remote", viewRemote, "read"],
	channels: ["Channels", viewChannels, "read"],
	epg: ["Guide", viewEpg, "read"],
	timers: ["Timers", viewTimers, "read"],
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
