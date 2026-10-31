/*
 * OIPF/HbbTV objects for neutrino-hbbtv. Injected at document start into
 * every frame. The methods live on the element prototypes so they work no
 * matter when an application inserts its <object> or <video> elements.
 *
 * Video is played by the box: WebKit never gets the source, the element
 * stays transparent over the hardware picture, and its state comes from
 * Neutrino through the launcher ("position", "ended", "error").
 *
 * License: GPL-2.0-or-later
 */
(function () {
	'use strict';
	if (window.__neutrinoHbbtv)
		return;
	window.__neutrinoHbbtv = true;

	/* shown only when the launcher runs with NEUTRINO_HBBTV_DEBUG */
	function trace(name, args) {
		console.log('oipf: ' + name + '(' + Array.prototype.map.call(args, function (a) {
			try { return JSON.stringify(a); } catch (e) { return String(a); }
		}).join(', ') + ')');
	}

	function traced(obj) {
		for (var k in obj) {
			if (typeof obj[k] === 'function')
				obj[k] = (function (name, fn) {
					return function () { trace(name, arguments); return fn.apply(this, arguments); };
				})(k, obj[k]);
		}
		return obj;
	}

	var bridge = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.neutrino;

	function send(line) {
		if (bridge)
			bridge.postMessage({ cmd: 'media', line: line });
	}

	var channel = window.__neutrinoChannel || {
		ccid: 'ccid:0', name: '', onid: 0, tsid: 0, sid: 0,
		channelType: 0, idType: 15, dsd: '', majorChannel: 0
	};

	var keys = {
		VK_LEFT: 37, VK_UP: 38, VK_RIGHT: 39, VK_DOWN: 40, VK_ENTER: 13,
		VK_BACK: 461, VK_RED: 403, VK_GREEN: 404, VK_YELLOW: 405, VK_BLUE: 406,
		VK_PLAY: 415, VK_PAUSE: 19, VK_PLAY_PAUSE: 402, VK_STOP: 413,
		VK_FAST_FWD: 417, VK_REWIND: 412, VK_TRACK_NEXT: 425, VK_TRACK_PREV: 424,
		VK_INFO: 457, VK_TELETEXT: 459,
		VK_0: 48, VK_1: 49, VK_2: 50, VK_3: 51, VK_4: 52,
		VK_5: 53, VK_6: 54, VK_7: 55, VK_8: 56, VK_9: 57
	};
	window.KeyEvent = window.KeyEvent || {};
	for (var k in keys) {
		window[k] = keys[k];
		window.KeyEvent[k] = keys[k];
	}

	var keyset = traced({
		RED: 0x1, GREEN: 0x2, YELLOW: 0x4, BLUE: 0x8, NAVIGATION: 0x10,
		VCR: 0x20, SCROLL: 0x40, INFO: 0x80, NUMERIC: 0x100, ALPHA: 0x200, OTHER: 0x400,
		value: 0x1f,
		setValue: function (v) { this.value = v; return v; },
		getMaximumValue: function () { return 0x1ff; }
	});

	var application = traced({
		privateData: { keyset: keyset, currentChannel: channel },
		visible: true,
		show: function () { this.visible = true; document.documentElement.style.visibility = ''; return true; },
		hide: function () { this.visible = false; document.documentElement.style.visibility = 'hidden'; return true; },
		activate: function () {},
		deactivate: function () {},
		createApplication: function (uri) { window.location.href = uri; return application; },
		destroyApplication: function () { if (bridge) bridge.postMessage({ cmd: 'exit' }); },
		getOwnerApplication: function () { return application; }
	});

	var configuration = {
		preferredAudioLanguage: 'deu,eng',
		preferredSubtitleLanguage: 'deu,eng',
		preferredUILanguage: 'deu,eng',
		countryId: 'DEU',
		regionId: 0
	};

	var localSystem = { deviceID: 'neutrino', modelName: 'Neutrino', vendorName: 'Neutrino' };

	/* the live picture; Neutrino stops and resumes it around the videos itself */
	var broadcast = traced({
		playState: 2,
		fullScreen: true,
		bindToCurrentChannel: function () { this.playState = 2; return channel; },
		setChannel: function () {},
		prevChannel: function () {},
		nextChannel: function () {},
		release: function () {},
		stop: function () {},
		setFullScreen: function (f) { this.fullScreen = !!f; },
		getChannelConfig: function () {
			return { channelList: { length: 1, item: function () { return channel; },
				getChannelByTriplet: function () { return channel; } } };
		}
	});

	/* --- playback through Neutrino --- */

	var states = new WeakMap();
	var current = null;
	var lastWindow = '';

	function state(el) {
		var s = states.get(el);
		if (!s) {
			s = { src: '', started: false, paused: true, ended: false, time: 0, duration: NaN,
			      readyState: 0, playState: 0, speed: 0, fullScreen: false };
			states.set(el, s);
		}
		return s;
	}

	function windowOf(el) {
		var r = el.getBoundingClientRect();
		var w = window.innerWidth, h = window.innerHeight;
		if (r.width <= 2 || r.height <= 2 || state(el).fullScreen ||
		    (r.left <= 0 && r.top <= 0 && r.right >= w && r.bottom >= h))
			return 'full';
		return Math.round(r.left) + ' ' + Math.round(r.top) + ' ' + Math.round(r.width) + ' ' + Math.round(r.height);
	}

	function followWindow() {
		if (!current)
			return;
		var win = windowOf(current);
		if (win !== lastWindow) {
			lastWindow = win;
			send('window ' + win);
		}
	}
	setInterval(followWindow, 500);

	function start(el, url) {
		if (current && current !== el)
			stopCurrent();
		var s = state(el);
		current = el;
		s.started = true;
		s.paused = false;
		s.ended = false;
		s.readyState = 0;
		lastWindow = windowOf(el);
		send('window ' + lastWindow);
		send('play ' + url);
		if (s.time > 0)
			send('seek ' + Math.round(s.time * 1000));
	}

	function stopCurrent() {
		var el = current;
		if (!el)
			return;
		var s = state(el);
		s.started = false;
		s.paused = true;
		current = null;
		lastWindow = '';
		send('stop');
		if (el instanceof HTMLObjectElement)
			avState(el, 0);
	}

	/* OIPF A/V control object: <object type="video/mp4" data="..."> */

	function avState(el, st) {
		var s = state(el);
		if (s.playState === st)
			return;
		s.playState = st;
		trace('PlayStateChange', [st]);
		if (typeof el.onPlayStateChange === 'function') {
			try { el.onPlayStateChange(st); } catch (e) { console.log(e); }
		}
		var ev = new Event('PlayStateChange');
		ev.state = st;
		el.dispatchEvent(ev);
	}

	function isBroadcast(el) {
		return /^video\/broadcast/i.test(el.getAttribute('type') || '');
	}

	var av = traced({
		play: function (speed) {
			var s = state(this);
			speed = speed === undefined ? 1 : Number(speed);
			s.speed = speed;
			if (this === current && s.started) {
				if (speed === 0 && !s.paused) {
					send('pause');
					s.paused = true;
					avState(this, 2);
				} else if (speed !== 0 && s.paused) {
					send('resume');
					s.paused = false;
					avState(this, 1);
				}
				return true;
			}
			if (speed === 0)
				return true;
			var url = this.getAttribute('data');
			if (!url)
				return false;
			start(this, new URL(url, document.baseURI).href);
			avState(this, 3);
			return true;
		},
		stop: function () {
			if (this === current)
				stopCurrent();
			else
				avState(this, 0);
			return true;
		},
		seek: function (ms) {
			var s = state(this);
			s.time = Number(ms) / 1000;
			if (this === current && s.started)
				send('seek ' + Math.round(Number(ms)));
			return true;
		}
	});

	var objectMethods = {
		getOwnerApplication: function () { return application; },
		play: function (speed) { return isBroadcast(this) ? false : av.play.call(this, speed); },
		stop: function () { return isBroadcast(this) ? broadcast.stop() : av.stop.call(this); },
		seek: function (ms) { return isBroadcast(this) ? false : av.seek.call(this, ms); },
		setFullScreen: function (f) {
			if (isBroadcast(this))
				return broadcast.setFullScreen(f);
			state(this).fullScreen = !!f;
			followWindow();
		},
		bindToCurrentChannel: function () { return broadcast.bindToCurrentChannel(); },
		setChannel: function (c) { return broadcast.setChannel(c); },
		prevChannel: function () { return broadcast.prevChannel(); },
		nextChannel: function () { return broadcast.nextChannel(); },
		release: function () { return isBroadcast(this) ? broadcast.release() : av.stop.call(this); },
		getChannelConfig: function () { return broadcast.getChannelConfig(); },
		selectComponent: function () {},
		unselectComponent: function () {},
		getComponents: function () { return []; }
	};
	var objectProperties = {
		configuration: function () { return configuration; },
		localSystem: function () { return localSystem; },
		currentChannel: function () { return channel; },
		playState: function () { return isBroadcast(this) ? broadcast.playState : state(this).playState; },
		fullScreen: function () { return isBroadcast(this) ? broadcast.fullScreen : state(this).fullScreen; },
		playPosition: function () { return Math.round(state(this).time * 1000); },
		playTime: function () { var d = state(this).duration; return isNaN(d) ? 0 : Math.round(d * 1000); },
		speed: function () { return state(this).speed; },
		error: function () { return state(this).playState === 6 ? 0 : undefined; }
	};
	var op = HTMLObjectElement.prototype;
	for (var m in objectMethods)
		Object.defineProperty(op, m, { value: objectMethods[m], configurable: true, writable: true });
	for (var p in objectProperties)
		Object.defineProperty(op, p, { get: objectProperties[p], configurable: true });

	window.oipfObjectFactory = {
		isObjectSupported: function (t) {
			return /^(application\/oipf|video\/broadcast|video\/mp4|video\/mpeg|application\/dash\+xml)/i.test(t);
		},
		createApplicationManagerObject: function () { return { getOwnerApplication: function () { return application; } }; },
		createConfigurationObject: function () { return { configuration: configuration, localSystem: localSystem }; },
		createVideoBroadcastObject: function () {
			var o = document.createElement('object');
			o.setAttribute('type', 'video/broadcast');
			return o;
		},
		createVideoMpegObject: function () {
			var o = document.createElement('object');
			o.setAttribute('type', 'video/mp4');
			return o;
		}
	};

	/* HTML5 <video> and <audio> */

	function fire(el, type) {
		el.dispatchEvent(new Event(type));
	}

	function sourceOf(el) {
		var s = state(el);
		if (s.src)
			return s.src;
		var source = el.querySelector('source[src]');
		return source ? source.src : '';
	}

	/* WebKit's own media events would tell of a load that never happens */
	['loadstart', 'progress', 'suspend', 'abort', 'error', 'emptied', 'stalled', 'loadedmetadata',
	 'loadeddata', 'canplay', 'canplaythrough', 'playing', 'waiting', 'seeking', 'seeked', 'ended',
	 'durationchange', 'timeupdate', 'play', 'pause', 'ratechange'].forEach(function (type) {
		window.addEventListener(type, function (e) {
			if (e.isTrusted && e.target instanceof HTMLMediaElement)
				e.stopImmediatePropagation();
		}, true);
	});

	var mp = HTMLMediaElement.prototype;
	var mediaMethods = {
		play: function () {
			var s = state(this), src = sourceOf(this);
			if (!src)
				return Promise.reject(new DOMException('no source', 'NotSupportedError'));
			if (this === current && s.started) {
				if (s.paused)
					send('resume');
			} else
				start(this, src);
			s.paused = false;
			fire(this, 'play');
			return Promise.resolve();
		},
		pause: function () {
			var s = state(this);
			if (this === current && !s.paused)
				send('pause');
			s.paused = true;
			fire(this, 'pause');
		},
		load: function () {
			if (this === current)
				stopCurrent();
			var s = state(this);
			s.readyState = 0;
			s.time = 0;
			s.duration = NaN;
			s.ended = false;
			if (this.autoplay && sourceOf(this))
				this.play();
		},
		canPlayType: function (t) {
			return /mp4|mpegurl|dash|mp2t|mpeg/i.test(t) ? 'probably' : '';
		}
	};
	for (var mm in mediaMethods)
		Object.defineProperty(mp, mm, { value: mediaMethods[mm], configurable: true, writable: true });

	Object.defineProperty(mp, 'src', {
		get: function () { return state(this).src; },
		set: function (v) {
			var s = state(this);
			if (this === current)
				stopCurrent();
			s.src = v ? new URL(v, document.baseURI).href : '';
			s.time = 0;
			s.duration = NaN;
			s.readyState = 0;
			if (s.src && this.autoplay)
				this.play();
		},
		configurable: true
	});
	Object.defineProperty(mp, 'currentTime', {
		get: function () { return state(this).time; },
		set: function (v) {
			var s = state(this);
			s.time = Number(v) || 0;
			if (this === current && s.started) {
				fire(this, 'seeking');
				send('seek ' + Math.round(s.time * 1000));
				fire(this, 'seeked');
			}
		},
		configurable: true
	});
	var mediaProperties = {
		currentSrc: function () { return sourceOf(this); },
		duration: function () { return state(this).duration; },
		paused: function () { return state(this).paused; },
		ended: function () { return state(this).ended; },
		readyState: function () { return state(this).readyState; },
		networkState: function () { return state(this).started ? 2 : 1; },
		error: function () { return null; }
	};
	for (var mq in mediaProperties)
		Object.defineProperty(mp, mq, { get: mediaProperties[mq], configurable: true });

	var setAttribute = Element.prototype.setAttribute;
	Element.prototype.setAttribute = function (name, value) {
		if (this instanceof HTMLMediaElement && String(name).toLowerCase() === 'src') {
			this.src = value;
			return;
		}
		return setAttribute.call(this, name, value);
	};

	/* --- what Neutrino reports back --- */

	window.__neutrinoMedia = function (line) {
		var el = current;
		if (!el)
			return;
		var s = state(el);
		var p = line.split(' ');
		var object = el instanceof HTMLObjectElement;
		if (p[0] === 'position') {
			var duration = Number(p[2]) / 1000;
			var first = s.readyState < 4;
			s.time = Number(p[1]) / 1000;
			if (first || (duration && duration !== s.duration))
				s.duration = duration || NaN;
			s.readyState = 4;
			if (object) {
				/* a report sent before a pause arrived must not undo it */
				if (p[3] === 'playing' && !s.paused && s.playState !== 1)
					avState(el, 1);
				return;
			}
			if (first) {
				['durationchange', 'loadedmetadata', 'loadeddata', 'canplay', 'canplaythrough', 'playing']
					.forEach(function (t) { fire(el, t); });
			}
			fire(el, 'timeupdate');
		} else if (p[0] === 'ended' || p[0] === 'error') {
			s.started = false;
			s.paused = true;
			current = null;
			lastWindow = '';
			if (object)
				avState(el, p[0] === 'ended' ? 5 : 6);
			else if (p[0] === 'ended') {
				s.ended = true;
				fire(el, 'pause');
				fire(el, 'ended');
			} else
				fire(el, 'error');
		}
	};

	/* an application that ends or removes its player stops the video */
	window.addEventListener('pagehide', stopCurrent);
	new MutationObserver(function () {
		if (current && !current.isConnected)
			stopCurrent();
	}).observe(document, { childList: true, subtree: true });
})();
