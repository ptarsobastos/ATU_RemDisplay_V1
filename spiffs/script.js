document.addEventListener("DOMContentLoaded", function () {
    let ws;
    let wifiSsidTouched = false;
    const DEBUG_WS_RX = false;

    const oledCanvas = document.getElementById('oledCanvas');
    const oledDecodedEl = document.getElementById('oledDecoded');
    const mirrorResetBtn = document.getElementById('mirrorReset');
    const oledCtx = oledCanvas ? oledCanvas.getContext('2d') : null;
    const OLED_W = 128;
    const OLED_H = 32;
    // UI-side orientation fix for mirrored OLED bitmap.
    // Set to false if backend mapping is adjusted later.
    const OLED_ROTATE_180 = true;
    let lastOledHash = null;

    function decodeOledPixels(payload) {
        if (!payload || payload.w !== OLED_W || payload.h !== OLED_H || !payload.bits_hex) {
            return { kind: 'invalid' };
        }
        if (payload.hash != null && payload.hash === lastOledHash) {
            return { kind: 'unchanged' };
        }

        const hex = String(payload.bits_hex);
        const expectedHexLen = (OLED_W * OLED_H / 8) * 2;
        if (hex.length < expectedHexLen) {
            return { kind: 'invalid' };
        }

        const pixels = new Uint8Array(OLED_W * OLED_H);
        let bitIndex = 0;

        for (let i = 0; i < expectedHexLen; i += 2) {
            const byte = parseInt(hex.slice(i, i + 2), 16);
            if (Number.isNaN(byte)) {
                return { kind: 'invalid' };
            }
            for (let b = 0; b < 8; b++) {
                const on = (byte & (0x80 >> b)) !== 0;
                let px = bitIndex % OLED_W;
                let py = (bitIndex / OLED_W) | 0;
                if (OLED_ROTATE_180) {
                    px = (OLED_W - 1) - px;
                    py = (OLED_H - 1) - py;
                }
                pixels[py * OLED_W + px] = on ? 1 : 0;
                bitIndex++;
            }
        }

        lastOledHash = payload.hash ?? lastOledHash;
        return { kind: 'ok', pixels };
    }

    function renderOledPixels(pixels) {
        if (!oledCtx || !pixels) return;

        const img = oledCtx.createImageData(OLED_W, OLED_H);
        for (let py = 0; py < OLED_H; py++) {
            for (let px = 0; px < OLED_W; px++) {
                const on = pixels[py * OLED_W + px] === 1;
                const p = (py * OLED_W + px) * 4;
                img.data[p + 0] = on ? 26 : 200;
                img.data[p + 1] = on ? 34 : 220;
                img.data[p + 2] = on ? 24 : 42;
                img.data[p + 3] = 255;
            }
        }
        oledCtx.putImageData(img, 0, 0);
    }

    function setConnectionError(message) {
        if (!oledDecodedEl) return;
        oledDecodedEl.textContent = message || '';
    }

    function initWebSocket() {
        ws = new WebSocket('ws://' + window.location.host + '/ws');

        ws.onopen = function () {
            console.log("WebSocket connection opened to " + ws.url);
            setConnectionError('');
        };

        // Display data update.
        ws.onmessage = function (event) {
            if (DEBUG_WS_RX) console.log("WS RX: " + event.data);
            const data = JSON.parse(event.data);
            // OLED frame comes in `data.oled`.
            const frame = decodeOledPixels(data?.oled);
            if (frame.kind === 'invalid') {
                setConnectionError('Invalid frame.');
                return;
            }
            if (frame.kind === 'unchanged') {
                return;
            }
            renderOledPixels(frame.pixels);
            setConnectionError('');
        };

        ws.onclose = function () {
            console.log("WebSocket connection closed");
            setConnectionError('WebSocket offline.');
            // Try reconnecting after 1 second.
            setTimeout(initWebSocket, 1000);
        };

        ws.onerror = function (error) {
            console.log("WebSocket error: " + error);
            setConnectionError('WebSocket offline.');
        };
    }

    initWebSocket();

    function sendCommand(command) {
        console.log("Sending command: " + command);
        if (ws && ws.readyState === WebSocket.OPEN) {
            ws.send(command);
        }
    }

    if (mirrorResetBtn) {
        mirrorResetBtn.addEventListener('click', async () => {
            mirrorResetBtn.disabled = true;
            setConnectionError('Resetting OLED mirror...');
            try {
                const res = await fetch('/api/mirror/reset', { method: 'POST' });
                const data = await res.json();
                if (!data.ok) throw new Error('mirror_reset_failed');
                lastOledHash = null;
                renderOledPixels(new Uint8Array(OLED_W * OLED_H));
                setConnectionError('OLED mirror reset.');
            } catch (err) {
                setConnectionError('OLED mirror reset failed.');
            } finally {
                mirrorResetBtn.disabled = false;
            }
        });
    }

    // Force "released" if the page loses focus, preventing a stuck visual button state.
    const releaseFns = new Set();
    function registerRelease(fn) {
        releaseFns.add(fn);
        return () => releaseFns.delete(fn);
    }
    function releaseAll() {
        for (const fn of releaseFns) fn();
    }
    document.addEventListener('visibilitychange', () => {
        if (document.visibilityState !== 'visible') releaseAll();
    });
    window.addEventListener('blur', releaseAll);

    function setupButton(buttonId, action) {
        const button = document.getElementById(buttonId);
        if (!button) return;

        let isPressed = false;

        function setPressedVisual(pressed) {
            button.classList.toggle('is-pressed', pressed);
        }

        function press() {
            if (isPressed) return;
            isPressed = true;
            setPressedVisual(true);
            sendCommand(JSON.stringify({ action: action, state: 'pressed' }));
        }

        function release() {
            if (!isPressed) return;
            isPressed = false;
            setPressedVisual(false);
            sendCommand(JSON.stringify({ action: action, state: 'released' }));
        }

        registerRelease(release);

        // Prefer Pointer Events (Android + desktop) to avoid duplicate mouse/touch events.
        if (window.PointerEvent) {
            button.addEventListener('pointerdown', (e) => {
                if (e.pointerType === 'mouse' && e.button !== 0) return;
                press();
                try { button.setPointerCapture(e.pointerId); } catch { }
            });
            button.addEventListener('pointerup', release);
            button.addEventListener('pointercancel', release);
        } else {
            // Fallback for older browsers.
            button.addEventListener('mousedown', (e) => {
                if (e.button !== 0) return;
                press();
            });
            button.addEventListener('mouseup', release);
            button.addEventListener('mouseleave', release);

            button.addEventListener('touchstart', (e) => {
                e.preventDefault();
                press();
            }, { passive: false });
            button.addEventListener('touchend', (e) => {
                e.preventDefault();
                release();
            }, { passive: false });
            button.addEventListener('touchcancel', release);
        }
    }

    // Front panel buttons (left->right): TUNE, BYPASS, AUTO.
    // Current firmware mapping:
    // - TUNE   -> toggle2
    // - AUTO   -> toggle1
    // - BYPASS -> toggle3
    setupButton('btnTune', 'toggle2');
    setupButton('btnBypass', 'toggle3');
    setupButton('btnAuto', 'toggle1');

    // Wi-Fi settings (configured via web UI, persisted in NVS)
    const wifiStatusEl = document.getElementById('wifiStatus');
    const wifiForm = document.getElementById('wifiForm');
    const wifiSsidEl = document.getElementById('wifiSsid');
    const wifiPassEl = document.getElementById('wifiPass');
    const wifiShowPassEl = document.getElementById('wifiShowPass');
    const wifiMsgEl = document.getElementById('wifiMsg');
    const wifiForgetBtn = document.getElementById('wifiForget');

    if (wifiSsidEl) {
        wifiSsidEl.addEventListener('input', () => {
            wifiSsidTouched = true;
        });
    }

    if (wifiShowPassEl && wifiPassEl) {
        wifiShowPassEl.addEventListener('change', () => {
            wifiPassEl.type = wifiShowPassEl.checked ? 'text' : 'password';
        });
    }

    async function refreshWifiStatus() {
        if (!wifiStatusEl) return;
        try {
            const res = await fetch('/api/wifi', { cache: 'no-store' });
            const data = await res.json();
            if (!data.ok) throw new Error('status_not_ok');

            const ssidText = data.ssid ? `SSID: ${data.ssid}` : 'SSID: (not set)';
            const ipText = data.ip ? `IP: ${data.ip}` : 'IP: (unknown)';
            const rssiText = data.has_rssi ? `RSSI: ${Number(data.rssi_dbm)} dBm` : 'RSSI: (n/a)';
            const connText = data.connected ? 'Connected' : 'Not connected';
            const modeText = data.mode ? `Mode: ${String(data.mode).toUpperCase()}` : 'Mode: (unknown)';

            wifiStatusEl.textContent = `${modeText} | ${connText} | ${ssidText} | ${ipText} | ${rssiText}`;

            if (wifiSsidEl && !wifiSsidTouched && data.ssid) {
                wifiSsidEl.value = data.ssid;
            }
        } catch (e) {
            wifiStatusEl.textContent = 'Wi-Fi status unavailable.';
        }
    }

    if (wifiForm) {
        wifiForm.addEventListener('submit', async (e) => {
            e.preventDefault();
            if (wifiMsgEl) wifiMsgEl.textContent = 'Saving Wi-Fi settings...';

            const ssid = (wifiSsidEl?.value || '').trim();
            const password = wifiPassEl?.value || '';

            try {
                const body = new URLSearchParams({ ssid, password }).toString();
                const res = await fetch('/api/wifi', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                    body,
                });
                const data = await res.json();
                if (!data.ok) throw new Error('save_failed');
                if (wifiMsgEl) wifiMsgEl.textContent = 'Saved. Device is restarting...';
            } catch (err) {
                if (wifiMsgEl) wifiMsgEl.textContent = 'Failed to save Wi-Fi settings.';
            }
        });
    }

    if (wifiForgetBtn) {
        wifiForgetBtn.addEventListener('click', async () => {
            if (wifiMsgEl) wifiMsgEl.textContent = 'Forgetting Wi-Fi settings...';
            try {
                const res = await fetch('/api/wifi/forget', { method: 'POST' });
                const data = await res.json();
                if (!data.ok) throw new Error('forget_failed');
                if (wifiMsgEl) wifiMsgEl.textContent = 'Forgotten. Device is restarting...';
            } catch (err) {
                if (wifiMsgEl) wifiMsgEl.textContent = 'Failed to forget Wi-Fi settings.';
            }
        });
    }

    refreshWifiStatus();
    setInterval(refreshWifiStatus, 5000);
});
