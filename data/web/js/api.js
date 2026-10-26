// Calls of /api/v1. A session or a paired device signs the browser in by
// cookie; a request that changes something then carries the CSRF token
// that GET /api/v1/session hands out.

const state = { csrf: "", level: "public", authenticated: false, device: "", secure: false, httpsPort: 0 };

export class ApiError extends Error {
	constructor(status, problem) {
		super(problem.detail || problem.title || `HTTP ${status}`);
		this.status = status;
		this.code = (problem.type || "").replace("/errors/", "");
	}
}

export async function api(path, { method = "GET", body } = {}) {
	const init = { method, headers: {}, credentials: "same-origin" };
	if (body !== undefined) {
		init.headers["Content-Type"] = "application/json";
		init.body = JSON.stringify(body);
	}
	if (method !== "GET" && state.csrf)
		init.headers["X-CSRF-Token"] = state.csrf;
	const r = await fetch(`/api/v1/${path}`, init);
	if (r.status === 202 || r.status === 204)
		return null;
	if (!r.ok) {
		let problem = {};
		try { problem = await r.json(); } catch { /* not JSON */ }
		throw new ApiError(r.status, problem);
	}
	return r.json();
}

// every page of a paged list
export async function all(path) {
	const items = [];
	let cursor = "";
	for (let i = 0; i < 50; i++) {
		const sep = path.includes("?") ? "&" : "?";
		const page = await api(cursor ? `${path}${sep}cursor=${encodeURIComponent(cursor)}` : path);
		items.push(...(page.items || []));
		if (!page.next_cursor)
			break;
		cursor = page.next_cursor;
	}
	return items;
}

export async function session() {
	const s = await api("session");
	state.csrf = s.csrf || "";
	state.level = s.level || "public";
	state.authenticated = !!s.authenticated;
	state.device = s.device || "";
	state.secure = !!s.secure;
	state.httpsPort = s.https_port || 0;
	return s;
}

// the same page over HTTPS, where a phone can be paired; empty without HTTPS
export function httpsUrl(path = location.pathname + location.search + location.hash) {
	if (location.protocol === "https:" || !state.httpsPort)
		return "";
	const port = state.httpsPort === 443 ? "" : `:${state.httpsPort}`;
	return `https://${location.hostname.includes(":") ? `[${location.hostname}]` : location.hostname}${port}${path}`;
}

// a name for this device on the TV and in the list of paired devices
export function deviceName() {
	const ua = navigator.userAgent;
	const android = ua.match(/Android [^;)]*;\s*([^;)]+?)(?: Build|\))/);
	if (android && android[1] !== "K")
		return android[1].trim().slice(0, 64);
	if (/iPhone/.test(ua)) return "iPhone";
	if (/iPad/.test(ua)) return "iPad";
	if (/Android/.test(ua)) return "Android phone";
	const browser = /Firefox\//.test(ua) ? "Firefox" : /Edg\//.test(ua) ? "Edge" : /Chrome\//.test(ua) ? "Chrome" : /Safari\//.test(ua) ? "Safari" : "Browser";
	const os = /Windows/.test(ua) ? "Windows" : /Mac OS/.test(ua) ? "macOS" : /Linux/.test(ua) ? "Linux" : "";
	return os ? `${browser} on ${os}` : browser;
}

export async function pairWithCode(code) {
	await api("pair", { method: "POST", body: { code, name: deviceName() } });
	return session();
}

/* asks the TV; onDigits(digits) once the question is shown, resolves once
 * someone said yes there */
export async function pairOnTv(onDigits) {
	const ask = await fetch("/api/v1/pair/request", {
		method: "POST", credentials: "same-origin",
		headers: { "Content-Type": "application/json" },
		body: JSON.stringify({ name: deviceName() }),
	});
	const first = await ask.json().catch(() => ({}));
	if (ask.status !== 202)
		throw new ApiError(ask.status, first);
	onDigits(first.digits);
	for (;;) {
		await new Promise((r) => setTimeout(r, 1500));
		const r = await fetch(`/api/v1/pair/wait/${first.id}`, { credentials: "same-origin" });
		if (r.status === 202)
			continue;
		const body = await r.json().catch(() => ({}));
		if (!r.ok)
			throw new ApiError(r.status, body);
		return session();
	}
}

export function level() {
	return state.level;
}

const order = ["public", "read", "write", "system"];

export function may(needed) {
	return order.indexOf(state.level) >= order.indexOf(needed);
}

export function key(name) {
	return api("osd/remote/key", { method: "POST", body: { name } });
}

export function osdUrl() {
	return `/api/v1/osd/screenshot?osd=true&video=false&crop=true&format=png&at=${Date.now() % 4294967296}`;
}

/* The selection colour of the box's OSD theme becomes the accent of the page,
 * where it stands out enough against the page's own background (contrast 3:1
 * for controls); otherwise the page keeps its own. Backgrounds and text stay
 * the page's, light or dark as the browser asks. */
function luminance(hex) {
	const c = [1, 3, 5].map((i) => parseInt(hex.slice(i, i + 2), 16) / 255)
		.map((v) => v <= 0.03928 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4);
	return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
}

function contrast(a, b) {
	const [x, y] = [luminance(a), luminance(b)].sort((p, q) => q - p);
	return (x + 0.05) / (y + 0.05);
}

function cssHex(name) {
	const v = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
	return /^#[0-9a-f]{6}$/i.test(v) ? v : null;
}

export async function applyTheme() {
	const root = document.documentElement.style;
	let theme = null;
	const apply = () => {
		root.removeProperty("--accent");
		root.removeProperty("--accent-fg");
		root.removeProperty("--radius");
		if (!theme)
			return;
		if (!theme.rounded)
			root.setProperty("--radius", "3px");
		const panel = cssHex("--panel"), bg = cssHex("--bg");
		const accent = theme.accent;
		if (!panel || !bg || !/^#[0-9a-f]{6}$/i.test(accent) || contrast(accent, panel) < 3 || contrast(accent, bg) < 3)
			return;
		let text = theme.accent_text;
		if (!/^#[0-9a-f]{6}$/i.test(text) || contrast(accent, text) < 4.5)
			text = contrast(accent, "#ffffff") >= contrast(accent, "#000000") ? "#ffffff" : "#000000";
		root.setProperty("--accent", accent);
		root.setProperty("--accent-fg", text);
	};
	/* a theme chosen on the box shows without reloading the page */
	const load = async () => {
		try {
			const t = await api("theme");
			if (JSON.stringify(t) === JSON.stringify(theme))
				return;
			theme = t;
		} catch {
			return;
		}
		apply();
	};
	await load();
	matchMedia("(prefers-color-scheme: dark)").addEventListener("change", apply);
	document.addEventListener("visibilitychange", () => { if (!document.hidden) load(); });
	setInterval(() => { if (!document.hidden) load(); }, 30000);
}
