/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * toonbus.js: the ReToon bus in the browser, with the calls of libtoonbus
 * (toonbus.h). Over the websocket /ws of the web interface, which lighttpd
 * passes on to "toon-bus serve" (serve.c): one JSON object per line.
 *
 *   bus.start()                       connect; again by itself when it drops
 *   bus.connected()                   true once every state has come in
 *   bus.json(name)                    retoon/<name> as an object, or null
 *   bus.value(name, key)              one value of it, or undefined
 *   bus.age(name)                     seconds since its "time", or -1
 *   bus.on(topic, fn)                 fn(topic, data) for each message
 *                                     (MQTT wildcards + and #); "bus" for
 *                                     connected/not connected: fn(up)
 *   bus.send(topic, payload)          a command; false when not connected
 */
const bus = (() => {
  const BASE = 'retoon/';
  const states = {}, handlers = [], statusFns = [];
  let ws = null, up = false, buf = '', retry = 1000, started = false;

  const matches = (sub, topic) => {
    const s = sub.split('/'), t = topic.split('/');
    for (let i = 0; i < s.length; i++) {
      if (s[i] === '#') return true;
      if (i >= t.length || (s[i] !== '+' && s[i] !== t[i])) return false;
    }
    return s.length === t.length;
  };

  const setUp = v => {
    if (up === v) return;
    up = v;
    for (const fn of statusFns) try { fn(up); } catch (e) { console.error(e); }
  };

  function message(m) {
    if (m.ready) { retry = 1000; setUp(true); return; }
    if (m.error) { console.warn('toonbus:', m.error, m.topic || ''); return; }
    if (typeof m.topic !== 'string') return;
    const name = m.topic.startsWith(BASE) ? m.topic.slice(BASE.length) : null;
    if (name && !name.includes('/')) {
      if (m.data === null) delete states[name]; else states[name] = m.data;
    }
    for (const [sub, fn] of handlers)
      if (matches(sub, m.topic)) try { fn(m.topic, m.data); } catch (e) { console.error(e); }
  }

  function later() {
    setTimeout(connect, retry);
    retry = Math.min(retry * 2, 30000);
  }

  async function connect() {
    if (!window.WebSocket) return;
    let token;
    try {
      const r = await fetch(new URL('cgi-bin/api/ws-token', location.href), { cache: 'no-store' });
      token = (await r.json()).token;
    } catch (e) { later(); return; }
    if (!token) { later(); return; }
    const url = new URL('ws', location.href);
    url.protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
    try { ws = new WebSocket(url); } catch (e) { later(); return; }
    ws.onopen = () => ws.send(JSON.stringify({ auth: token }) + '\n');
    ws.onmessage = ev => {
      buf += ev.data;
      let i;
      while ((i = buf.indexOf('\n')) >= 0) {
        const line = buf.slice(0, i);
        buf = buf.slice(i + 1);
        if (line) try { message(JSON.parse(line)); } catch (e) { /* a broken line */ }
      }
    };
    ws.onclose = () => {
      ws = null; buf = '';
      setUp(false);
      later();
    };
  }

  return {
    start() { if (!started) { started = true; connect(); } },
    connected: () => up,
    json: name => states[name] ?? null,
    value: (name, key) => (states[name] || {})[key],
    age(name) {
      const t = (states[name] || {}).time;
      return typeof t === 'number' ? Math.max(0, Math.round(Date.now() / 1000 - t)) : -1;
    },
    on(topic, fn) {
      if (topic === 'bus') statusFns.push(fn); else handlers.push([topic, fn]);
    },
    send(topic, payload) {
      if (!ws || !up) return false;
      ws.send(JSON.stringify({ topic, payload: String(payload) }) + '\n');
      return true;
    },
  };
})();
