import { createContext } from 'preact';
import { signal } from '@preact/signals';
import uuidv4 from '../utils/uuid.js';

function randomId() {
  return Math.random()
    .toString(36)
    .replace(/[^a-z]+/g, '')
    .substr(2, 10);
}

export default class ApiService {
  socket = null;
  listeners = {};
  reconnectAttempts = 0;
  // Capped at 5s, not 30s: the backoff exists to avoid hammering a device that
  // is off, but a 30s cap means a machine that just rebooted (OTA, watchdog)
  // leaves the UI dead long enough that every button looks broken.
  maxReconnectDelay = 5000;
  baseReconnectDelay = 1000; // Start with 1 second delay
  reconnectTimeout = null;
  isConnecting = false;

  constructor() {
    console.log('Established websocket connection');
    this.connect();
    // Coming back to the tab, or regaining network, is the strongest signal that
    // a retry is worth making now rather than after the remaining backoff.
    if (typeof window !== 'undefined') {
      const retryNow = () => {
        if (document.visibilityState === 'visible' && !this.isSocketOpen()) {
          this.reconnectNow();
        }
      };
      window.addEventListener('online', retryNow);
      document.addEventListener('visibilitychange', retryNow);
    }
  }

  isSocketOpen() {
    return !!this.socket && this.socket.readyState === WebSocket.OPEN;
  }

  // Drop any pending backoff and reconnect immediately, resetting the escalation
  // so a user-visible retry doesn't inherit a long delay from earlier failures.
  reconnectNow() {
    if (this.reconnectTimeout) {
      clearTimeout(this.reconnectTimeout);
      this.reconnectTimeout = null;
    }
    this.reconnectAttempts = 0;
    this.connect();
  }

  async connect() {
    if (this.isConnecting) return;
    this.isConnecting = true;

    try {
      if (this.socket) {
        this.socket.close();
      }

      const apiHost = window.location.host;
      const wsProtocol = window.location.protocol === 'https:' ? 'wss://' : 'ws://';
      this.socket = new WebSocket(`${wsProtocol}${apiHost}/ws`);

      this.socket.addEventListener('message', this._onMessage.bind(this));
      this.socket.addEventListener('close', this._onClose.bind(this));
      this.socket.addEventListener('error', this._onError.bind(this));
      this.socket.addEventListener('open', this._onOpen.bind(this));
    } catch (error) {
      console.error('WebSocket connection error:', error);
      this._scheduleReconnect();
    } finally {
      this.isConnecting = false;
    }
  }

  _onOpen() {
    console.log('WebSocket connected successfully');
    this.reconnectAttempts = 0;
    // A reconnect may already be queued from the close that preceded this open;
    // leaving it armed would tear down the socket we just established.
    if (this.reconnectTimeout) {
      clearTimeout(this.reconnectTimeout);
      this.reconnectTimeout = null;
    }
    machine.value = {
      ...machine.value,
      connected: true,
    };
  }

  _onClose(event) {
    // connect() closes any previous socket, and that close fires asynchronously
    // with this handler still bound to it. Without this guard the dead socket
    // schedules a reconnect that closes the live one, and the two take turns
    // forever.
    if (event?.target && event.target !== this.socket) {
      return;
    }
    console.log('WebSocket connection closed');
    machine.value = {
      ...machine.value,
      connected: false,
    };
    this._scheduleReconnect();
  }

  _onError(error) {
    console.error('WebSocket error:', error);
    if (this.socket) {
      this.socket.close();
    }
  }

  _scheduleReconnect() {
    if (this.reconnectTimeout) {
      clearTimeout(this.reconnectTimeout);
    }

    // Calculate delay with exponential backoff
    const delay = Math.min(
      this.baseReconnectDelay * Math.pow(2, this.reconnectAttempts),
      this.maxReconnectDelay,
    );

    console.log(`Scheduling reconnect attempt ${this.reconnectAttempts + 1} in ${delay}ms`);

    this.reconnectTimeout = setTimeout(() => {
      this.reconnectAttempts++;
      this.connect();
    }, delay);
  }

  _onMessage(event) {
    let message;
    try {
      message = JSON.parse(event.data);
    } catch {
      return; // Discard malformed messages to avoid crashing the WS handler.
    }
    const listeners = Object.values(this.listeners[message.tp] || {});
    if (message.tp === 'evt:status') {
      this._onStatus(message);
    }
    for (const listener of listeners) {
      listener(message);
    }
  }

  send(event) {
    if (this.isSocketOpen()) {
      this.socket.send(JSON.stringify(event));
      return;
    }
    // Someone is present and waiting, so this is the best possible moment to
    // retry rather than sitting out the remaining backoff. Still throws, so the
    // failure isn't swallowed.
    this.reconnectNow();
    throw new Error('WebSocket is not connected');
  }

  async request(data = {}) {
    if (!this.isSocketOpen()) {
      this.reconnectNow();
      throw new Error('WebSocket is not connected');
    }

    const returnType = `res:${data.tp.substring(4)}`;
    const rid = uuidv4();
    const message = { ...data, rid };
    return new Promise((resolve, reject) => {
      let timeoutId;

      // Create a listener for the response with matching rid
      const listenerId = this.on(returnType, response => {
        if (response.rid === rid) {
          // Clean up the listener and cancel the timeout to free the closure.
          clearTimeout(timeoutId);
          this.off(returnType, listenerId);
          resolve(response);
        }
      });

      // Send the request
      this.send(message);

      // Timeout: reject if no matching response arrives within 30 seconds
      timeoutId = setTimeout(() => {
        this.off(returnType, listenerId);
        reject(new Error(`Request ${data.tp} timed out`));
      }, 30000); // 30 second timeout
    });
  }

  on(type, listener) {
    const id = randomId();
    if (!this.listeners[type]) {
      this.listeners[type] = {};
    }
    this.listeners[type][id] = listener;
    return id;
  }

  off(type, id) {
    delete this.listeners[type][id];
  }

  _onStatus(message) {
    const newStatus = {
      currentTemperature: message.ct,
      targetTemperature: message.tt,
      currentPressure: message.pr,
      targetPressure: message.pt,
      targetWeight: message.tw || 0,
      activeTargetWeight: (message?.process?.a && message.tw) || 0,
      currentFlow: message.fl,
      mode: message.m,
      selectedProfile: message.p,
      selectedProfileId: message.puid,
      brewTarget: !!message.bt,
      brewTargetDuration: message.btd || 0,
      volumetricAvailable: message.bta || false,
      grindTargetDuration: message.gtd || 0,
      grindTargetVolume: message.gtv || 0,
      grindTarget: message.gt || 0,
      grindActive: message.gact || false,
      grinderModel: message.grm || 0,
      grindLevel: message.gl || 0,
      doseWeight: message.dw || 0,
      currentWeight: message.cw || 0,
      bluetoothConnected: message.bc || false,
      process: message.process || null,
      timestamp: new Date(),
      rssi: message.rssi || 0,
      lat: message.lat || 0,
      tofDistance: message.tof || 0,
    };
    const historyEntry = { ...newStatus };
    delete historyEntry.process;
    const newValue = {
      ...machine.value,
      connected: true,
      status: {
        ...machine.value.status,
        ...newStatus,
      },
      capabilities: {
        ...machine.value.capabilities,
        dimming: message.cd,
        pressure: message.cp,
        ledControl: message.led,
      },
      history: [...machine.value.history, historyEntry],
    };
    newValue.history = newValue.history.slice(-600);
    machine.value = newValue;
  }
}

export const ApiServiceContext = createContext(null);

export const machine = signal({
  connected: false,
  status: {
    currentTemperature: 0,
    targetTemperature: 0,
    mode: 0,
    selectedProfile: '',
    selectedProfileId: null,
    brewTargetDuration: 0,
    brewTargetVolume: 0,
    grindTargetDuration: 0,
    grindTargetVolume: 0,
    grindTarget: 0,
    grindActive: false,
    grinderModel: 0,
    grindLevel: 0,
    doseWeight: 0,
    process: null,
  },
  capabilities: {
    pressure: false,
    dimming: false,
  },
  history: [],
});
