(() => {
  "use strict";

  const WS_URL = `ws://${location.hostname}/ws`;
  const VIDEO_BYTES = 5760;
  const PACKET_TYPE_FRAME = 0x01;

  // Peanut-GB shade: 0 = lightest … 3 = darkest (grayscale DMG look)
  const PALETTE = [
    [224, 224, 224], // white / light
    [160, 160, 160], // light gray
    [80, 80, 80],    // dark gray
    [8, 8, 8],       // black
  ];

  // Keyboard → bitmask bits (Spec §4.B.1)
  const KEY_BITS = {
    ArrowRight: 0,
    ArrowLeft: 1,
    ArrowUp: 2,
    ArrowDown: 3,
    KeyZ: 4, // A
    KeyX: 5, // B
    ShiftLeft: 6,
    ShiftRight: 6,
    Enter: 7,
    KeyA: 4,
    KeyB: 5,
    KeyQ: 6,
    KeyW: 7,
  };

  const canvas = document.getElementById("screen");
  const ctx = canvas.getContext("2d", { alpha: false });
  const imageData = ctx.createImageData(160, 144);
  const pixels = imageData.data;
  const statusEl = document.getElementById("status");
  const liveDot = document.querySelector(".dot");
  const romNameEl = document.getElementById("romName");
  const romFileEl = document.getElementById("romFile");
  const romListEl = document.getElementById("romList");
  const storageText = document.getElementById("storageText");
  const storageBar = document.getElementById("storageBar");
  const storageDetail = document.getElementById("storageDetail");
  const uploadBar = document.getElementById("uploadBar");
  const uploadMsg = document.getElementById("uploadMsg");
  const cartBtn = document.querySelector("label.cart-btn[for='romFile']");
  const settingsBtn = document.getElementById("settingsBtn");
  const settingsClose = document.getElementById("settingsClose");
  const settingsPanel = document.getElementById("settingsPanel");
  const settingsBackdrop = document.getElementById("settingsBackdrop");
  const bufferRange = document.getElementById("bufferRange");
  const bufferValue = document.getElementById("bufferValue");
  const hapticToggle = document.getElementById("hapticToggle");
  const saveInfo = document.getElementById("saveInfo");
  const saveMsg = document.getElementById("saveMsg");
  const saveDownload = document.getElementById("saveDownload");
  const saveDelete = document.getElementById("saveDelete");
  const saveFileEl = document.getElementById("saveFile");
  const rebootBtn = document.getElementById("rebootBtn");
  const rebootMsg = document.getElementById("rebootMsg");
  const linkOwnMac = document.getElementById("linkOwnMac");
  const linkCopyMac = document.getElementById("linkCopyMac");
  const linkPeerMac = document.getElementById("linkPeerMac");
  const linkStart = document.getElementById("linkStart");
  const linkStop = document.getElementById("linkStop");
  const linkStats = document.getElementById("linkStats");
  const linkMsg = document.getElementById("linkMsg");

  let ws = null;
  let joypad = 0;
  let audioCtx = null;
  let nextAudioTime = 0;
  let maxRomBytes = 2 * 1024 * 1024;
  let uploading = false;
  let settingsOpen = false;
  let promptedForRom = false;
  let hasSave = false;
  let hapticsEnabled = true;
  let linkPoll = null;
  let filmBadPacing = false;
  const AUDIO_SAMPLE_RATE = 32768;
  const BUFFER_KEY = "espgb.bufferMs";
  const HAPTIC_KEY = "espgb.haptics";
  const LINK_PEER_KEY = "espgb.linkPeer";
  const HAPTIC_MS = 12; // short key-click style pulse

  // Jitter buffer (tunable in Settings; default 48 ms)
  // Present at a fixed DMG rate — never speed up / slow down to "chase" the queue.
  const FRAME_MS = 1000 / 59.7275;
  let BUFFER_TARGET_MS = 48;
  let BUFFER_START = 3;
  let BUFFER_MAX = 9;
  let AUDIO_TARGET_AHEAD = 0.08;
  let AUDIO_MAX_AHEAD = 0.2;

  function applyBufferMs(ms) {
    BUFFER_TARGET_MS = Math.max(16, Math.min(120, Math.round(ms / 8) * 8));
    BUFFER_START = Math.max(1, Math.round(BUFFER_TARGET_MS / FRAME_MS));
    BUFFER_MAX = BUFFER_START + 6;
    // Audio schedules on packet arrival (not present), so keep a healthier lead
    AUDIO_TARGET_AHEAD = Math.max(0.1, (BUFFER_TARGET_MS + 40) / 1000);
    AUDIO_MAX_AHEAD = Math.max(0.22, AUDIO_TARGET_AHEAD * 2.5);
    if (bufferRange) bufferRange.value = String(BUFFER_TARGET_MS);
    if (bufferValue) {
      const frames = BUFFER_START;
      bufferValue.textContent = `${BUFFER_TARGET_MS} ms (~${frames}f)`;
    }
    try {
      localStorage.setItem(BUFFER_KEY, String(BUFFER_TARGET_MS));
    } catch (_) {
      /* private mode */
    }
  }

  function loadBufferSetting() {
    let ms = 48;
    try {
      const stored = localStorage.getItem(BUFFER_KEY);
      if (stored) ms = Number(stored) || 48;
    } catch (_) {
      /* ignore */
    }
    applyBufferMs(ms);
  }

  /** @type {{ video: Uint8Array, audio: Uint8Array }[]} */
  const frameQueue = [];
  let playbackArmed = false;
  let nextPresentMs = 0;
  let lastFrameVideo = null;

  function setSettingsOpen(open) {
    settingsOpen = open;
    settingsPanel.hidden = !open;
    settingsBackdrop.hidden = !open;
    settingsBtn.setAttribute("aria-expanded", open ? "true" : "false");
    document.body.style.overflow = open ? "hidden" : "";
    if (open) {
      refreshLink();
      if (!linkPoll) {
        linkPoll = setInterval(refreshLink, 400);
      }
    } else if (linkPoll) {
      clearInterval(linkPoll);
      linkPoll = null;
    }
  }

  function setStatus(text, live) {
    statusEl.textContent = text;
    liveDot.classList.toggle("live", !!live);
  }

  function sendJoypad() {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(new Uint8Array([joypad]));
    }
  }

  function loadHapticSetting() {
    try {
      const stored = localStorage.getItem(HAPTIC_KEY);
      if (stored !== null) hapticsEnabled = stored === "1";
    } catch (_) {
      /* ignore */
    }
    if (hapticToggle) hapticToggle.checked = hapticsEnabled;
  }

  function setHapticsEnabled(on) {
    hapticsEnabled = !!on;
    try {
      localStorage.setItem(HAPTIC_KEY, hapticsEnabled ? "1" : "0");
    } catch (_) {
      /* ignore */
    }
  }

  function hapticTap() {
    if (!hapticsEnabled || !navigator.vibrate) return;
    try {
      navigator.vibrate(HAPTIC_MS);
    } catch (_) {
      /* unsupported / denied */
    }
  }

  function setBit(bit, pressed) {
    const mask = 1 << bit;
    const next = pressed ? (joypad | mask) : (joypad & ~mask);
    if (next === joypad) return;
    if (pressed) hapticTap();
    joypad = next;
    sendJoypad();
  }

  function bindButtons() {
    // Kill iOS/Android long-press callout / haptic on the play surface
    document.addEventListener("contextmenu", (e) => e.preventDefault());

    document.querySelectorAll(".btn[data-bit]").forEach((btn) => {
      const bit = Number(btn.dataset.bit);
      let held = false;

      const down = (e) => {
        e.preventDefault();
        if (held) return;
        held = true;
        if (e.pointerId != null && btn.setPointerCapture) {
          try { btn.setPointerCapture(e.pointerId); } catch (_) { /* ignore */ }
        }
        btn.classList.add("pressed");
        setBit(bit, true);
        ensureAudio();
      };
      const up = (e) => {
        if (e) e.preventDefault();
        if (!held) return;
        held = false;
        btn.classList.remove("pressed");
        setBit(bit, false);
      };

      // Pointer path (mouse / pen / modern touch)
      btn.addEventListener("pointerdown", down);
      btn.addEventListener("pointerup", up);
      btn.addEventListener("pointercancel", up);
      btn.addEventListener("lostpointercapture", up);
      // Non-passive touch path blocks long-press haptic/callout on mobile Safari
      btn.addEventListener("touchstart", down, { passive: false });
      btn.addEventListener("touchend", up, { passive: false });
      btn.addEventListener("touchcancel", up, { passive: false });
    });
  }

  function bindKeyboard() {
    const downKeys = new Set();

    window.addEventListener("keydown", (e) => {
      const bit = KEY_BITS[e.code];
      if (bit === undefined || downKeys.has(e.code)) return;
      e.preventDefault();
      downKeys.add(e.code);
      setBit(bit, true);
      ensureAudio();
    });

    window.addEventListener("keyup", (e) => {
      const bit = KEY_BITS[e.code];
      if (bit === undefined) return;
      e.preventDefault();
      downKeys.delete(e.code);
      setBit(bit, false);
    });
  }

  function ensureAudio() {
    if (!audioCtx) {
      audioCtx = new (window.AudioContext || window.webkitAudioContext)({
        sampleRate: AUDIO_SAMPLE_RATE,
      });
      nextAudioTime = audioCtx.currentTime + AUDIO_TARGET_AHEAD;
    }
    if (audioCtx.state === "suspended") {
      audioCtx.resume();
    }
  }

  function playPcmU8(pcm, speed) {
    if (!audioCtx || !pcm || pcm.length === 0) return;
    if (!speed || speed <= 0) speed = 1;

    const now = audioCtx.currentTime;
    // Underrun: rebuild a small lead-in instead of scheduling in the past
    if (nextAudioTime < now + 0.02) {
      nextAudioTime = now + AUDIO_TARGET_AHEAD;
    }
    // Overrun: drop this chunk so latency doesn't balloon (honest clock only)
    if (!filmBadPacing && nextAudioTime > now + AUDIO_MAX_AHEAD) {
      return;
    }

    const buffer = audioCtx.createBuffer(1, pcm.length, AUDIO_SAMPLE_RATE);
    const channel = buffer.getChannelData(0);
    for (let i = 0; i < pcm.length; i++) {
      channel[i] = (pcm[i] - 128) / 128;
    }

    const source = audioCtx.createBufferSource();
    source.buffer = buffer;
    source.playbackRate.value = speed;
    source.connect(audioCtx.destination);
    source.start(nextAudioTime);
    nextAudioTime += buffer.duration / speed;
  }

  function unpackVideo(packed) {
    let pi = 0;
    for (let i = 0; i < VIDEO_BYTES; i++) {
      const byte = packed[i];
      for (let shift = 6; shift >= 0; shift -= 2) {
        const color = (byte >> shift) & 0x03;
        const rgb = PALETTE[color];
        pixels[pi++] = rgb[0];
        pixels[pi++] = rgb[1];
        pixels[pi++] = rgb[2];
        pixels[pi++] = 255;
      }
    }
  }

  function presentFrame(video) {
    unpackVideo(video);
    ctx.putImageData(imageData, 0, 0);
    lastFrameVideo = video;
  }

  function enqueueFrame(video, audio) {
    // Honest clock: schedule audio on arrival so video drops don't chop sound.
    // Film sick-pacing: keep PCM with the frame so present-rate can pitch it.
    if (!filmBadPacing && audio && audio.length) {
      playPcmU8(audio);
    }
    frameQueue.push({
      video: new Uint8Array(video),
      audio: filmBadPacing && audio && audio.length ? new Uint8Array(audio) : null,
    });
    const maxQ = filmBadPacing ? 24 : BUFFER_MAX;
    while (frameQueue.length > maxQ) {
      frameQueue.shift();
    }
  }

  function filmBreatheSpeed(timestamp) {
    // Square-ish 2.6s cycle: sprint then crawl. Readable in a 10-second take.
    const cycle = 2600;
    const up = 1.55;
    const down = 0.62;
    const fade = 160;
    const phase = timestamp % cycle;
    if (phase < fade) {
      return down + (up - down) * (phase / fade);
    }
    if (phase < 1150) {
      return up;
    }
    if (phase < 1150 + fade) {
      return up + (down - up) * ((phase - 1150) / fade);
    }
    return down;
  }

  function renderLoopSick(timestamp) {
    if (!playbackArmed) {
      if (frameQueue.length >= 1) {
        playbackArmed = true;
        nextPresentMs = timestamp;
      }
      return;
    }

    if (timestamp < nextPresentMs) {
      return;
    }

    let speed = filmBreatheSpeed(timestamp);
    if (frameQueue.length > 8) {
      speed *= 1.2;
    } else if (frameQueue.length < 2) {
      speed *= 0.85;
    }

    if (frameQueue.length > 0) {
      const frame = frameQueue.shift();
      presentFrame(frame.video);
      if (frame.audio) {
        playPcmU8(frame.audio, speed);
      }
    } else if (lastFrameVideo) {
      presentFrame(lastFrameVideo);
    }

    nextPresentMs += FRAME_MS / speed;
  }

  function renderLoop(timestamp) {
    if (filmBadPacing) {
      renderLoopSick(timestamp);
      requestAnimationFrame(renderLoop);
      return;
    }

    if (!playbackArmed) {
      if (frameQueue.length >= BUFFER_START) {
        playbackArmed = true;
        nextPresentMs = timestamp;
      }
    } else if (frameQueue.length === 0) {
      // Hold last picture until the queue refills — no slow-mo stretch
      playbackArmed = false;
    }

    if (playbackArmed && frameQueue.length > 0 && timestamp >= nextPresentMs) {
      // Too much backlog: drop oldest video (audio already scheduled)
      while (frameQueue.length > BUFFER_START + 1) {
        frameQueue.shift();
      }

      presentFrame(frameQueue.shift().video);
      nextPresentMs += FRAME_MS;

      // After a stall, don't try to "owe" multiple catch-up presents
      if (timestamp - nextPresentMs > FRAME_MS * 2) {
        nextPresentMs = timestamp;
      }
    }

    requestAnimationFrame(renderLoop);
  }

  function handleFrame(buf) {
    const view = new DataView(buf);
    if (view.byteLength < 3 || view.getUint8(0) !== PACKET_TYPE_FRAME) return;

    const audioLen = view.getUint16(1, true);
    const audioStart = 3;
    const videoStart = audioStart + audioLen;
    if (view.byteLength < videoStart + VIDEO_BYTES) return;

    const audio =
      audioLen > 0 ? new Uint8Array(buf, audioStart, audioLen) : new Uint8Array(0);
    const video = new Uint8Array(buf, videoStart, VIDEO_BYTES);
    enqueueFrame(video, audio);
  }

  function formatBytes(n) {
    if (n < 1024) return `${n} B`;
    if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
    return `${(n / (1024 * 1024)).toFixed(2)} MB`;
  }

  function setUploadMsg(text, kind) {
    uploadMsg.textContent = text || "";
    uploadMsg.classList.toggle("error", kind === "error");
    uploadMsg.classList.toggle("ok", kind === "ok");
  }

  function setSaveMsg(text, kind) {
    saveMsg.textContent = text || "";
    saveMsg.classList.toggle("error", kind === "error");
    saveMsg.classList.toggle("ok", kind === "ok");
  }

  function updateStorageMeter(storage) {
    if (!storage) return;
    const total = storage.total || 0;
    const used = storage.used || 0;
    const free = storage.free != null ? storage.free : Math.max(0, total - used);
    const pct = total > 0 ? Math.min(100, Math.round((used / total) * 100)) : 0;
    storageText.textContent = `${pct}% used`;
    storageBar.style.width = `${pct}%`;
    storageBar.classList.toggle("warn", pct >= 70 && pct < 90);
    storageBar.classList.toggle("full", pct >= 90);
    storageDetail.textContent =
      `${formatBytes(used)} used · ${formatBytes(free)} free · ${formatBytes(total)} total`;
  }

  function waitForReboot(onReady) {
    let tries = 0;
    const waitBack = setInterval(async () => {
      tries += 1;
      try {
        const ping = await fetch("/api/status", { cache: "no-store" });
        if (ping.ok) {
          clearInterval(waitBack);
          onReady();
        }
      } catch (_) {
        /* rebooting */
      }
      if (tries > 40) {
        clearInterval(waitBack);
        onReady(new Error("timeout"));
      }
    }, 750);
  }

  async function refreshLibrary() {
    try {
      const res = await fetch("/api/roms", { cache: "no-store" });
      if (!res.ok) return;
      const data = await res.json();
      updateStorageMeter(data.storage);

      romListEl.innerHTML = "";
      const roms = data.roms || [];
      if (roms.length === 0) {
        const li = document.createElement("li");
        li.className = "setting-help";
        li.textContent = "Library empty — add a .gb ROM below.";
        romListEl.appendChild(li);
        return;
      }

      roms
        .slice()
        .sort((a, b) => a.name.localeCompare(b.name))
        .forEach((rom) => {
          const li = document.createElement("li");
          li.className = "rom-item" + (rom.active ? " active" : "");

          const info = document.createElement("div");
          const name = document.createElement("div");
          name.className = "rom-item-name";
          name.textContent = rom.name;
          const meta = document.createElement("div");
          meta.className = "rom-item-meta";
          meta.textContent =
            `${formatBytes(rom.size || 0)}` +
            (rom.hasSave ? " · save" : "") +
            (rom.active ? " · playing" : "");
          info.appendChild(name);
          info.appendChild(meta);

          const actions = document.createElement("div");
          actions.className = "rom-item-actions";

          const playBtn = document.createElement("button");
          playBtn.type = "button";
          playBtn.textContent = rom.active ? "Playing" : "Play";
          playBtn.disabled = !!rom.active;
          playBtn.addEventListener("click", () => selectRom(rom.name));

          const delBtn = document.createElement("button");
          delBtn.type = "button";
          delBtn.className = "danger";
          delBtn.textContent = "Delete";
          delBtn.addEventListener("click", () => deleteRom(rom.name, rom.active));

          actions.appendChild(playBtn);
          actions.appendChild(delBtn);
          li.appendChild(info);
          li.appendChild(actions);
          romListEl.appendChild(li);
        });
    } catch (_) {
      /* ignore */
    }
  }

  async function selectRom(name) {
    setUploadMsg(`Loading ${name}…`);
    setStatus("Switching ROM…", false);
    try {
      const body = new URLSearchParams({ name });
      const res = await fetch("/api/roms/select", {
        method: "POST",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
        body,
      });
      const json = await res.json().catch(() => ({}));
      if (!res.ok || !json.ok) {
        setUploadMsg((json && json.error) || "Could not switch ROM", "error");
        return;
      }
      setUploadMsg("Rebooting into cartridge…", "ok");
      waitForReboot((err) => {
        if (err) {
          setUploadMsg("Reboot timed out — refresh the page.", "error");
          return;
        }
        setUploadMsg("Cartridge ready.", "ok");
        refreshStatus();
        refreshLibrary();
        if (!ws || ws.readyState !== WebSocket.OPEN) connect();
      });
    } catch (_) {
      setUploadMsg("Could not switch ROM", "error");
    }
  }

  async function deleteRom(name, wasActive) {
    if (!confirm(`Delete ${name} from the library?` + (wasActive ? " (also stops play)" : ""))) {
      return;
    }
    try {
      const res = await fetch(`/api/roms?name=${encodeURIComponent(name)}`, {
        method: "DELETE",
      });
      const json = await res.json().catch(() => ({}));
      if (!res.ok || !json.ok) {
        setUploadMsg((json && json.error) || "Delete failed", "error");
        return;
      }
      if (json.rebooting) {
        setUploadMsg("Deleted — rebooting…", "ok");
        waitForReboot(() => {
          refreshStatus();
          refreshLibrary();
          if (!ws || ws.readyState !== WebSocket.OPEN) connect();
        });
      } else {
        setUploadMsg("Deleted from library.", "ok");
        refreshLibrary();
        refreshStatus();
      }
    } catch (_) {
      setUploadMsg("Delete failed", "error");
    }
  }

  async function refreshStatus() {
    try {
      const res = await fetch("/api/status", { cache: "no-store" });
      if (!res.ok) return;
      const info = await res.json();
      if (info.maxRomBytes) maxRomBytes = info.maxRomBytes;
      filmBadPacing = !!info.filmBadPacing;
      updateStorageMeter(info.storage);

      if (info.rom && info.romName) {
        romNameEl.textContent = `${info.romName} (${formatBytes(info.romSize || 0)})`;
      } else if (info.rom) {
        romNameEl.textContent = `ROM (${formatBytes(info.romSize || 0)})`;
      } else {
        romNameEl.textContent = "No ROM loaded — add one to the library";
        if (!promptedForRom && !uploading) {
          promptedForRom = true;
          setSettingsOpen(true);
        }
      }

      hasSave = !!info.save && (info.saveSize || 0) > 0;
      if (!info.battery) {
        saveInfo.textContent = "This cartridge has no battery save RAM.";
      } else if (hasSave) {
        saveInfo.textContent = `Save on device: ${formatBytes(info.saveSize)} (autosaved to flash)`;
      } else {
        saveInfo.textContent = `Battery save ready (${formatBytes(info.batterySize || 0)}) — empty until the game writes.`;
      }
      saveDownload.disabled = !hasSave;
      saveDelete.disabled = !hasSave;

      if (!uploading) {
        if (!info.rom) setStatus("Waiting for ROM", false);
        else if (info.running) setStatus("Linked", ws && ws.readyState === WebSocket.OPEN);
      }

      refreshLibrary();
    } catch (_) {
      /* ignore transient AP blips during reboot */
    }
  }

  function uploadRom(file) {
    if (!file || uploading) return;

    const lower = file.name.toLowerCase();
    if (!lower.endsWith(".gb") && !lower.endsWith(".gbc")) {
      setUploadMsg("Please choose a .gb / .gbc ROM file.", "error");
      return;
    }
    if (file.size > maxRomBytes) {
      setUploadMsg(`ROM too large (max ${formatBytes(maxRomBytes)}).`, "error");
      return;
    }
    if (file.size < 0x150) {
      setUploadMsg("File is too small to be a Game Boy ROM.", "error");
      return;
    }

    uploading = true;
    let uploadFinished = false; // true only after full body reached the ESP
    if (cartBtn) cartBtn.classList.add("busy");
    uploadBar.style.width = "0%";
    setUploadMsg(`Adding ${file.name} (${formatBytes(file.size)})…`);
    setStatus("Uploading ROM…", false);

    const body = new FormData();
    // Keep a clean .gb name for library storage even if source was .gbc
    const storeName = lower.endsWith(".gbc")
      ? file.name.replace(/\.gbc$/i, ".gb")
      : file.name;
    body.append("rom", file, storeName);

    const xhr = new XMLHttpRequest();
    xhr.open("POST", "/api/rom");
    xhr.timeout = 120000; // Pokémon-sized ROMs over SoftAP can be slow
    xhr.responseType = "json";

    xhr.upload.onprogress = (e) => {
      if (!e.lengthComputable) return;
      const pct = Math.round((e.loaded / e.total) * 100);
      uploadBar.style.width = `${pct}%`;
      setUploadMsg(`Uploading… ${pct}% (${formatBytes(e.loaded)} / ${formatBytes(e.total)})`);
    };
    xhr.upload.onload = () => {
      uploadFinished = true;
    };

    xhr.onload = () => {
      const res = xhr.response || {};
      if (xhr.status >= 200 && xhr.status < 300 && res.ok) {
        uploadBar.style.width = "100%";
        setUploadMsg("Added to library — rebooting…", "ok");
        setStatus("Rebooting…", false);
        waitForReboot((err) => {
          uploading = false;
          if (cartBtn) cartBtn.classList.remove("busy");
          if (err) {
            setUploadMsg("Reboot timed out — refresh the page.", "error");
            return;
          }
          setUploadMsg("Cartridge ready.", "ok");
          refreshStatus();
          if (!ws || ws.readyState !== WebSocket.OPEN) connect();
        });
      } else {
        uploading = false;
        if (cartBtn) cartBtn.classList.remove("busy");
        const err = (res && res.error) || `Upload failed (${xhr.status})`;
        setUploadMsg(err, "error");
        setStatus("Upload failed", false);
        refreshLibrary();
      }
    };

    xhr.onerror = () => {
      // Only treat as reboot if the ESP got the whole file; otherwise SoftAP dropped a big upload
      if (!uploadFinished) {
        uploading = false;
        if (cartBtn) cartBtn.classList.remove("busy");
        setUploadMsg(
          "Upload dropped on Wi‑Fi — try again (large ROMs like Pokémon are sensitive). Stay close to the ESP.",
          "error"
        );
        setStatus("Upload failed", false);
        refreshLibrary();
        return;
      }
      setUploadMsg("Device rebooting… reconnecting", "ok");
      setStatus("Rebooting…", false);
      waitForReboot((err) => {
        uploading = false;
        if (cartBtn) cartBtn.classList.remove("busy");
        uploadBar.style.width = "100%";
        if (err) {
          setUploadMsg("Reconnect timed out — refresh the page.", "error");
          return;
        }
        setUploadMsg("Cartridge ready.", "ok");
        refreshStatus();
        if (!ws || ws.readyState !== WebSocket.OPEN) connect();
      });
    };

    xhr.ontimeout = () => {
      uploading = false;
      if (cartBtn) cartBtn.classList.remove("busy");
      setUploadMsg("Upload timed out — try again closer to the ESP32.", "error");
      setStatus("Upload failed", false);
    };

    xhr.send(body);
  }

  function bindRomUpload() {
    romFileEl.addEventListener("change", () => {
      const file = romFileEl.files && romFileEl.files[0];
      romFileEl.value = "";
      if (file) uploadRom(file);
    });
  }

  function bindBufferSetting() {
    bufferRange.addEventListener("input", () => {
      applyBufferMs(Number(bufferRange.value));
    });
  }

  function bindHapticSetting() {
    hapticToggle.addEventListener("change", () => {
      setHapticsEnabled(hapticToggle.checked);
      if (hapticToggle.checked) hapticTap(); // preview
    });
  }

  async function uploadSave(file) {
    if (!file) return;
    setSaveMsg(`Uploading ${file.name}…`);
    try {
      const body = new FormData();
      body.append("save", file, file.name);
      const res = await fetch("/api/save", { method: "POST", body });
      const json = await res.json().catch(() => ({}));
      if (!res.ok || !json.ok) {
        setSaveMsg((json && json.error) || "Save upload failed", "error");
        return;
      }
      setSaveMsg("Save uploaded — rebooting to apply…", "ok");
      setStatus("Rebooting…", false);
      let tries = 0;
      const waitBack = setInterval(async () => {
        tries += 1;
        try {
          const ping = await fetch("/api/status", { cache: "no-store" });
          if (ping.ok) {
            clearInterval(waitBack);
            setSaveMsg("Save loaded.", "ok");
            refreshStatus();
            if (!ws || ws.readyState !== WebSocket.OPEN) connect();
          }
        } catch (_) {
          /* rebooting */
        }
        if (tries > 40) {
          clearInterval(waitBack);
          setSaveMsg("Reboot timed out — refresh the page.", "error");
        }
      }, 750);
    } catch (_) {
      setSaveMsg("Save upload failed", "error");
    }
  }

  function setRebootMsg(text, kind) {
    rebootMsg.textContent = text || "";
    rebootMsg.classList.toggle("error", kind === "error");
    rebootMsg.classList.toggle("ok", kind === "ok");
  }

  function bindSaveControls() {
    saveDownload.addEventListener("click", () => {
      window.location.href = "/api/save";
    });
    saveFileEl.addEventListener("change", () => {
      const file = saveFileEl.files && saveFileEl.files[0];
      saveFileEl.value = "";
      if (file) uploadSave(file);
    });
    saveDelete.addEventListener("click", async () => {
      if (!hasSave) return;
      if (!confirm("Delete save data on the ESP32?")) return;
      try {
        const res = await fetch("/api/save", { method: "DELETE" });
        const json = await res.json().catch(() => ({}));
        if (!res.ok || !json.ok) {
          setSaveMsg((json && json.error) || "Delete failed", "error");
          return;
        }
        setSaveMsg("Save deleted — rebooting…", "ok");
        setStatus("Rebooting…", false);
      } catch (_) {
        setSaveMsg("Delete failed", "error");
      }
    });
  }

  function bindReboot() {
    rebootBtn.addEventListener("click", async () => {
      if (uploading) {
        setRebootMsg("Wait for the ROM upload to finish", "error");
        return;
      }
      if (!confirm("Reboot the ESP32 now?")) return;
      rebootBtn.disabled = true;
      setRebootMsg("Rebooting…", "ok");
      setStatus("Rebooting…", false);
      try {
        const res = await fetch("/api/reboot", { method: "POST" });
        const json = await res.json().catch(() => ({}));
        if (!res.ok || !json.ok) {
          setRebootMsg((json && json.error) || "Reboot failed", "error");
          rebootBtn.disabled = false;
          return;
        }
      } catch (_) {
        // Request may drop as the AP goes down — treat as expected
      }
      waitForReboot((err) => {
        rebootBtn.disabled = false;
        if (err) {
          setRebootMsg("Timed out waiting for reboot — reconnect to ESP-GameBoy", "error");
          return;
        }
        setRebootMsg("Back online", "ok");
        setStatus("Linked", true);
        refreshStatus();
        refreshLibrary();
        connect();
      });
    });
  }

  function selectedLinkRole() {
    const el = document.querySelector('input[name="linkRole"]:checked');
    return (el && el.value) || "echo";
  }

  function renderLinkStats(info) {
    if (!info || !info.ok) {
      linkStats.textContent = "Link radio not ready.";
      return;
    }
    const rtt = (us) => (us ? (us / 1000).toFixed(2) + " ms" : "—");
    if (info.role === "echo") {
      linkStats.textContent =
        `Echo  ch${info.channel}  GPIO send=${info.gpioSend} recv=${info.gpioRecv}\n` +
        `running ${info.running ? "yes" : "no"}\n` +
        `pings in ${info.echoRecv}  pongs out ${info.echoSent}  send fail ${info.sendFail}\n` +
        `ignored ${info.ignored}`;
      return;
    }
    linkStats.textContent =
      `Ping  ch${info.channel}  GPIO send=${info.gpioSend} recv=${info.gpioRecv}\n` +
      `running ${info.running ? "yes" : "no"}  success ${info.successPct}%\n` +
      `sent ${info.sent}  pong ${info.pong}  lost ${info.lost}  send fail ${info.sendFail}\n` +
      `RTT last ${rtt(info.rttUsLast)}  avg ${rtt(info.rttUsAvg)}\n` +
      `min ${rtt(info.rttUsMin)}  max ${rtt(info.rttUsMax)}`;
  }

  async function refreshLink() {
    try {
      const res = await fetch("/api/link", { cache: "no-store" });
      if (!res.ok) return;
      const info = await res.json();
      if (info.mac) linkOwnMac.textContent = info.mac;
      renderLinkStats(info);
    } catch (_) { /* AP down */ }
  }

  async function postLink(fields) {
    const body = new URLSearchParams(fields);
    const res = await fetch("/api/link", {
      method: "POST",
      headers: { "Content-Type": "application/x-www-form-urlencoded" },
      body,
    });
    const json = await res.json().catch(() => ({}));
    if (!res.ok || json.ok === false) {
      throw new Error((json && json.error) || "Link request failed");
    }
    if (json.mac) linkOwnMac.textContent = json.mac;
    renderLinkStats(json);
    return json;
  }

  function bindLinkRadio() {
    try {
      const saved = localStorage.getItem(LINK_PEER_KEY);
      if (saved) linkPeerMac.value = saved;
    } catch (_) { /* ignore */ }

    linkCopyMac.addEventListener("click", async () => {
      const mac = linkOwnMac.textContent.trim();
      if (!mac || mac === "—") return;
      try {
        await navigator.clipboard.writeText(mac);
        linkMsg.textContent = "Copied this board’s MAC";
        linkMsg.className = "upload-msg ok";
      } catch (_) {
        linkMsg.textContent = mac;
        linkMsg.className = "upload-msg";
      }
    });

    linkStart.addEventListener("click", async () => {
      const peer = linkPeerMac.value.trim();
      const role = selectedLinkRole();
      if (!peer) {
        linkMsg.textContent = "Paste the other board’s SoftAP MAC";
        linkMsg.className = "upload-msg error";
        return;
      }
      try {
        localStorage.setItem(LINK_PEER_KEY, peer);
      } catch (_) { /* ignore */ }
      linkStart.classList.add("busy");
      try {
        await postLink({ peer, role, run: "1" });
        linkMsg.textContent = role === "echo"
          ? "Echoing — start Ping on the other board"
          : "Pinging at ~1 kHz — watch RTT vs video on";
        linkMsg.className = "upload-msg ok";
      } catch (err) {
        linkMsg.textContent = err.message || "Start failed";
        linkMsg.className = "upload-msg error";
      }
      linkStart.classList.remove("busy");
    });

    linkStop.addEventListener("click", async () => {
      try {
        await postLink({ run: "0" });
        linkMsg.textContent = "Stopped";
        linkMsg.className = "upload-msg";
      } catch (err) {
        linkMsg.textContent = err.message || "Stop failed";
        linkMsg.className = "upload-msg error";
      }
    });
  }

  function bindSettings() {
    settingsBtn.addEventListener("click", () => {
      const open = !settingsOpen;
      setSettingsOpen(open);
      if (open) refreshLibrary();
    });
    settingsClose.addEventListener("click", () => setSettingsOpen(false));
    settingsBackdrop.addEventListener("click", () => {
      if (!uploading) setSettingsOpen(false);
    });
    window.addEventListener("keydown", (e) => {
      if (e.key === "Escape" && settingsOpen && !uploading) {
        setSettingsOpen(false);
      }
    });
  }

  function connect() {
    if (ws) {
      try { ws.close(); } catch (_) { /* ignore */ }
    }

    setStatus("Connecting…", false);
    ws = new WebSocket(WS_URL);
    ws.binaryType = "arraybuffer";

    ws.onopen = () => {
      frameQueue.length = 0;
      playbackArmed = false;
      setStatus("Linked", true);
      sendJoypad();
      refreshStatus();
    };

    ws.onclose = () => {
      // Never fight a ROM upload for SoftAP bandwidth
      if (uploading) return;
      setStatus("Reconnecting…", false);
      setTimeout(() => {
        if (!uploading) connect();
      }, 1000);
    };

    ws.onerror = () => {
      if (!uploading) setStatus("Socket error", false);
    };

    ws.onmessage = (ev) => {
      if (ev.data instanceof ArrayBuffer) {
        handleFrame(ev.data);
      }
    };
  }

  // Unlock audio on first tap anywhere
  document.body.addEventListener("pointerdown", ensureAudio, { once: true });

  loadBufferSetting();
  loadHapticSetting();
  bindButtons();
  bindKeyboard();
  bindRomUpload();
  bindBufferSetting();
  bindHapticSetting();
  bindSaveControls();
  bindReboot();
  bindLinkRadio();
  bindSettings();
  renderLoop();
  refreshStatus();
  connect();
})();
