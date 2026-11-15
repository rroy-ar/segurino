(function () {
  "use strict";

  const configWarning = document.getElementById("config-warning");
  const loginView = document.getElementById("login-view");
  const mainContent = document.getElementById("main-content");

  if (!window.FIREBASE_CONFIG || window.FIREBASE_CONFIG.apiKey === "YOUR_FIREBASE_WEB_API_KEY") {
    configWarning.classList.remove("hidden");
    if (!window.FIREBASE_CONFIG) {
      loginView.classList.add("hidden");
      return;
    }
  }

  firebase.initializeApp(window.FIREBASE_CONFIG);
  const database = firebase.database();
  const auth = firebase.auth();

  // --- Database References ---
  const systemStatusRef = database.ref("systemStatus/status");
  const pinsRef = database.ref("pins");
  const eventsRef = database.ref("events").orderByChild("epoch").limitToLast(50);

  // --- DOM Elements ---
  const authForm = document.getElementById("auth-form");
  const loginEmailInput = document.getElementById("login-email");
  const loginPasswordInput = document.getElementById("login-password");
  const btnLogin = document.getElementById("btn-login");
  const btnRegister = document.getElementById("btn-register");
  const errorMessage = document.getElementById("error-message");

  const userEmailDisplay = document.getElementById("user-email");
  const btnToggleSound = document.getElementById("btn-toggle-sound");
  const btnLogout = document.getElementById("btn-logout");
  const statusDisplay = document.getElementById("status-display");
  const btnArm = document.getElementById("btn-arm");
  const btnDisarm = document.getElementById("btn-disarm");

  const pinForm = document.getElementById("pin-form");
  const newPinInput = document.getElementById("new-pin-input");
  const newPinLabel = document.getElementById("new-pin-label");
  const pinFeedback = document.getElementById("pin-feedback");
  const pinsList = document.getElementById("pins-list");
  const eventsList = document.getElementById("events-list");

  let currentUser = null;
  let soundEnabled = true;
  let initialEventsLoaded = false;
  const knownEventKeys = new Set();
  const revealedPins = new Set();
  let currentPinsCache = {};

  // --- Web Audio API Alert Siren (zero external dependencies) ---
  function playAlertTone() {
    if (!soundEnabled) {
      return;
    }
    const AudioCtx = window.AudioContext || window.webkitAudioContext;
    if (!AudioCtx) {
      return;
    }
    try {
      const ctx = new AudioCtx();
      const now = ctx.currentTime;
      [880, 660, 880].forEach((freq, idx) => {
        const osc = ctx.createOscillator();
        const gain = ctx.createGain();
        osc.type = "square";
        osc.frequency.setValueAtTime(freq, now + idx * 0.18);
        gain.gain.setValueAtTime(0.08, now + idx * 0.18);
        gain.gain.exponentialRampToValueAtTime(0.001, now + idx * 0.18 + 0.16);
        osc.connect(gain);
        gain.connect(ctx.destination);
        osc.start(now + idx * 0.18);
        osc.stop(now + idx * 0.18 + 0.16);
      });
    } catch (err) {
      console.warn("Unable to play browser alert tone:", err);
    }
  }

  btnToggleSound.addEventListener("click", () => {
    soundEnabled = !soundEnabled;
    btnToggleSound.textContent = soundEnabled ? "Sound: On" : "Sound: Off";
  });

  // --- Authentication ---
  auth.onAuthStateChanged((user) => {
    errorMessage.textContent = "";
    if (user) {
      currentUser = user;
      loginView.classList.add("hidden");
      mainContent.classList.remove("hidden");
      userEmailDisplay.textContent = user.email || "Authenticated User";
      attachDatabaseListeners();
    } else {
      currentUser = null;
      loginView.classList.remove("hidden");
      mainContent.classList.add("hidden");
      detachDatabaseListeners();
    }
  });

  authForm.addEventListener("submit", async (e) => {
    e.preventDefault();
    const email = loginEmailInput.value.trim();
    const password = loginPasswordInput.value;

    if (!email || !password) {
      errorMessage.textContent = "Please enter both email and password.";
      return;
    }

    setAuthLoading(true);
    try {
      await auth.signInWithEmailAndPassword(email, password);
    } catch (_err) {
      errorMessage.textContent = "Invalid email or password.";
    } finally {
      setAuthLoading(false);
    }
  });

  btnRegister.addEventListener("click", async () => {
    const email = loginEmailInput.value.trim();
    const password = loginPasswordInput.value;

    if (!email || password.length < 6) {
      errorMessage.textContent = "Provide a valid email and a password of at least 6 characters.";
      return;
    }

    setAuthLoading(true);
    try {
      await auth.createUserWithEmailAndPassword(email, password);
    } catch (err) {
      errorMessage.textContent = err.message || "Registration failed.";
    } finally {
      setAuthLoading(false);
    }
  });

  btnLogout.addEventListener("click", () => auth.signOut());

  function setAuthLoading(isLoading) {
    btnLogin.disabled = isLoading;
    btnRegister.disabled = isLoading;
  }

  // --- Database Listeners & UI Renderers ---
  const updateStatusUI = (snapshot) => {
    const status = snapshot.val();
    if (status === "ARMED") {
      statusDisplay.textContent = "System Armed";
      statusDisplay.className = "status-banner status-armed";
      btnArm.disabled = true;
      btnDisarm.disabled = false;
    } else if (status === "DISARMED") {
      statusDisplay.textContent = "System Disarmed";
      statusDisplay.className = "status-banner status-disarmed";
      btnArm.disabled = false;
      btnDisarm.disabled = true;
    } else {
      statusDisplay.textContent = "Status Unknown";
      statusDisplay.className = "status-banner status-loading";
      btnArm.disabled = false;
      btnDisarm.disabled = false;
    }
  };

  const updatePinsUI = (snapshot) => {
    currentPinsCache = snapshot.val() || {};
    renderPins();
  };

  function renderPins() {
    pinsList.textContent = "";
    if (!currentUser) {
      return;
    }

    const userPins = Object.entries(currentPinsCache).filter(
      ([, pinObj]) => pinObj && pinObj.owner === currentUser.uid
    );

    if (userPins.length === 0) {
      const emptyLi = document.createElement("li");
      emptyLi.className = "empty-state";
      emptyLi.textContent = "No keypad PINs configured yet.";
      pinsList.appendChild(emptyLi);
      return;
    }

    userPins.forEach(([key, pinObj]) => {
      const li = document.createElement("li");
      const meta = document.createElement("div");
      meta.className = "pin-meta";

      const isRevealed = revealedPins.has(key);
      const pinValueSpan = document.createElement("span");
      pinValueSpan.className = "pin-value";
      pinValueSpan.textContent = isRevealed ? pinObj.value : "•".repeat(String(pinObj.value).length);
      meta.appendChild(pinValueSpan);

      if (pinObj.label) {
        const labelSpan = document.createElement("span");
        labelSpan.className = "pin-label";
        labelSpan.textContent = pinObj.label;
        meta.appendChild(labelSpan);
      }

      const actions = document.createElement("div");
      actions.className = "pin-actions";

      const toggleBtn = document.createElement("button");
      toggleBtn.type = "button";
      toggleBtn.className = "icon-btn";
      toggleBtn.textContent = isRevealed ? "Hide" : "Show";
      toggleBtn.addEventListener("click", () => {
        if (revealedPins.has(key)) {
          revealedPins.delete(key);
        } else {
          revealedPins.add(key);
        }
        renderPins();
      });

      const deleteBtn = document.createElement("button");
      deleteBtn.type = "button";
      deleteBtn.className = "delete-pin-btn";
      deleteBtn.setAttribute("aria-label", "Delete PIN");
      deleteBtn.textContent = "×";
      deleteBtn.addEventListener("click", () => {
        if (window.confirm("Are you sure you want to delete this PIN?")) {
          pinsRef.child(key).remove();
        }
      });

      actions.appendChild(toggleBtn);
      actions.appendChild(deleteBtn);
      li.appendChild(meta);
      li.appendChild(actions);
      pinsList.appendChild(li);
    });
  }

  function parseEventTimeMs(event) {
    if (typeof event.epoch === "number") {
      return event.epoch * 1000;
    }
    if (event.timestamp) {
      const normalized = String(event.timestamp).includes("T")
        ? event.timestamp
        : String(event.timestamp).replace(" ", "T");
      const parsed = Date.parse(normalized);
      if (!Number.isNaN(parsed)) {
        return parsed;
      }
    }
    return 0;
  }

  function formatEventTimestamp(event) {
    const ms = parseEventTimeMs(event);
    if (ms > 0) {
      return new Date(ms).toLocaleString();
    }
    return event.timestamp || "Unknown time";
  }

  const updateEventsUI = (snapshot) => {
    eventsList.textContent = "";
    const events = snapshot.val();

    if (!events) {
      const emptyLi = document.createElement("li");
      emptyLi.className = "empty-state";
      emptyLi.textContent = "No intrusion events recorded.";
      eventsList.appendChild(emptyLi);
      initialEventsLoaded = true;
      return;
    }

    const sortedEvents = Object.entries(events)
      .map(([id, data]) => ({ id, ...data }))
      .sort((a, b) => parseEventTimeMs(b) - parseEventTimeMs(a));

    sortedEvents.forEach((event) => {
      knownEventKeys.add(event.id);

      const li = document.createElement("li");
      const details = document.createElement("div");
      details.className = "event-details";

      const typeSpan = document.createElement("span");
      typeSpan.className = "event-type";
      typeSpan.textContent = `Intrusion: ${event.type || "UNKNOWN"}`;

      const timeSpan = document.createElement("span");
      timeSpan.className = "event-time";
      timeSpan.textContent = formatEventTimestamp(event);

      details.appendChild(typeSpan);
      details.appendChild(timeSpan);
      li.appendChild(details);
      eventsList.appendChild(li);
    });

    initialEventsLoaded = true;
  };

  const handleNewEventChild = (snapshot) => {
    if (initialEventsLoaded && !knownEventKeys.has(snapshot.key)) {
      knownEventKeys.add(snapshot.key);
      playAlertTone();
    }
  };

  function attachDatabaseListeners() {
    detachDatabaseListeners();
    initialEventsLoaded = false;
    knownEventKeys.clear();

    systemStatusRef.on("value", updateStatusUI);
    pinsRef.on("value", updatePinsUI);
    eventsRef.on("value", updateEventsUI);
    eventsRef.on("child_added", handleNewEventChild);
  }

  function detachDatabaseListeners() {
    systemStatusRef.off("value", updateStatusUI);
    pinsRef.off("value", updatePinsUI);
    eventsRef.off("value", updateEventsUI);
    eventsRef.off("child_added", handleNewEventChild);
  }

  // --- Controls ---
  btnArm.addEventListener("click", () => {
    systemStatusRef.set("ARMED");
  });

  btnDisarm.addEventListener("click", () => {
    systemStatusRef.set("DISARMED");
  });

  pinForm.addEventListener("submit", async (e) => {
    e.preventDefault();
    if (!currentUser) {
      return;
    }

    const pinValue = newPinInput.value.trim();
    const label = newPinLabel.value.trim();
    pinFeedback.textContent = "";

    if (!/^\d{4,8}$/.test(pinValue)) {
      pinFeedback.textContent = "Please enter a numeric PIN between 4 and 8 digits.";
      return;
    }

    const isDuplicate = Object.values(currentPinsCache).some(
      (entry) => entry && entry.value === pinValue
    );
    if (isDuplicate) {
      pinFeedback.textContent = "That PIN is already registered.";
      return;
    }

    try {
      await pinsRef.push({
        value: pinValue,
        owner: currentUser.uid,
        label: label || "Keypad PIN",
        createdAt: new Date().toISOString()
      });
      newPinInput.value = "";
      newPinLabel.value = "";
    } catch (err) {
      pinFeedback.textContent = err.message || "Failed to add PIN.";
    }
  });
})();
