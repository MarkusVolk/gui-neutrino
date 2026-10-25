// The remote control alone, filling the screen of a phone. Opened from the
// code the TV shows, the address carries a one-time code that pairs the
// phone first.

import { session, may, key, ApiError, pairWithCode, pairOnTv, httpsUrl } from "./api.js";
import { loadLayout, drawRemote } from "./remote.js";

const msg = document.getElementById("msg");
const status = document.getElementById("status");

function show(text, ...actions) {
	msg.replaceChildren(document.createTextNode(text));
	for (const a of actions) {
		const b = document.createElement(a.href ? "a" : "button");
		b.className = "btn primary";
		b.textContent = a.text;
		if (a.href)
			b.href = a.href;
		else
			b.addEventListener("click", a.run);
		msg.append(document.createElement("br"), b);
	}
	msg.hidden = false;
}

let statusTimer = 0;
function note(text) {
	status.textContent = text;
	clearTimeout(statusTimer);
	statusTimer = setTimeout(() => { status.textContent = ""; }, 2500);
}

let wakeLock = null;
async function stayAwake() {
	try {
		if ("wakeLock" in navigator && !wakeLock) {
			wakeLock = await navigator.wakeLock.request("screen");
			wakeLock.addEventListener("release", () => { wakeLock = null; });
		}
	} catch { /* not offered here */ }
}

function fullScreen() {
	const d = document.documentElement;
	if (!document.fullscreenElement && d.requestFullscreen)
		d.requestFullscreen({ navigationUI: "hide" }).catch(() => {});
	stayAwake();
}

function noAccess() {
	const secure = httpsUrl();
	if (secure)
		show("This phone has no access to the box. Pair it over HTTPS.", { text: "Go to HTTPS", href: secure });
	else if (location.protocol === "https:")
		show("This phone has no access to the box. Show the code on the TV (Network > Web server > Pair a phone) and scan it, or ask the TV here.",
			{ text: "Ask the TV", run: askTv });
	else
		show("This phone has no access to the box.", { text: "Sign in", href: "/#/signin" });
}

async function askTv() {
	try {
		await pairOnTv((digits) => show(`Allow this phone on the TV. The TV shows the code ${digits}.`));
		ready();
	} catch (e) {
		show(e.message || "The TV did not allow it.", { text: "Ask again", run: askTv });
	}
}

async function press(name) {
	try {
		await key(name);
	} catch (e) {
		if (e instanceof ApiError && (e.status === 401 || e.status === 403))
			noAccess();
		else
			note(e.message || "No answer from the box");
	}
}

async function ready() {
	msg.hidden = true;
	if (!may("write")) {
		noAccess();
		return;
	}
	drawRemote(document.getElementById("remote"), await loadLayout(), press);
}

async function start() {
	document.getElementById("full").addEventListener("click", fullScreen);
	document.addEventListener("pointerdown", fullScreen, { once: true });
	document.addEventListener("visibilitychange", () => { if (document.visibilityState === "visible" && document.fullscreenElement) stayAwake(); });
	try {
		await session();
	} catch {
		show("The box does not answer.");
		return;
	}
	const code = new URLSearchParams(location.search).get("pair");
	if (code) {
		history.replaceState(null, "", location.pathname);
		try {
			await pairWithCode(code);
			note("Paired");
		} catch (e) {
			/* the home network may let it in anyway, but it is not paired */
			show(`Not paired: ${e.message || "pairing did not work."}`,
				{ text: "Ask the TV", run: askTv }, { text: "Use without pairing", run: ready });
			return;
		}
	}
	ready();
}

start();
