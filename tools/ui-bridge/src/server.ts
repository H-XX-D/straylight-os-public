/**
 * tools/ui-bridge/src/server.ts
 *
 * Straylight UI Bridge — WebSocket ↔ Unix IPC gateway.
 *
 * Every Straylight service exposes a Unix domain socket at
 * /run/straylight/<service>.sock  using the common IpcServer protocol:
 *   4-byte little-endian length prefix + UTF-8 JSON payload.
 *
 * This daemon:
 *   1. Connects (or re-connects) to each registered socket.
 *   2. Exposes a WebSocket server on ws://localhost:7700
 *   3. Multiplexes messages with an envelope:
 *        { service: string, type: "req"|"res"|"event", id?: string, payload: unknown }
 *
 * TSX components use the hooks in @straylight/ui-hooks to interact.
 */

import { createServer } from "http";
import { WebSocketServer, WebSocket } from "ws";
import * as net from "net";

// ─── Constants ────────────────────────────────────────────────────────────────

const WS_PORT = 7700;
const SOCKET_BASE = process.env.SL_SOCKET_BASE ?? "/run/straylight";
const RECONNECT_MS = 3000;

// ─── Service registry ─────────────────────────────────────────────────────────

/** All Straylight services that have an IPC socket. */
const SERVICES = [
  "core",
  "bus",
  "notify",
  "health",
  "power",
  "network",
  "audio",
  "disk",
  "display",
  "input",
  "hooks",
  "policy",
  "predict",
  "flux",
  "fabric",
  "mesh",
  "sandbox",
  "rewind",
  "replay",
  "snapshot",
  "backup",
  "vault",
  "intent",
  "cron",
  "probe",
  "update",
  "users",
  "log",
  "shield",
  "ghost",
  "alice",
  "agent",
] as const;

export type ServiceName = (typeof SERVICES)[number];

// ─── Envelope ─────────────────────────────────────────────────────────────────

export interface Envelope {
  service: ServiceName;
  type: "req" | "res" | "event" | "error" | "status";
  id?: string;       // correlates req → res
  payload: unknown;
}

// ─── IPC socket wrapper ───────────────────────────────────────────────────────

class ServiceSocket {
  private socket: net.Socket | null = null;
  private recvBuf = Buffer.alloc(0);
  private reconnectTimer: NodeJS.Timeout | null = null;
  private _connected = false;

  constructor(
    readonly name: ServiceName,
    private readonly onMessage: (svc: ServiceName, msg: string) => void,
    private readonly onStatusChange: (svc: ServiceName, up: boolean) => void
  ) {
    this.connect();
  }

  get connected() { return this._connected; }

  private get socketPath() {
    return `${SOCKET_BASE}/${this.name}.sock`;
  }

  private connect() {
    const s = new net.Socket();
    s.connect(this.socketPath);

    s.on("connect", () => {
      this._connected = true;
      this.socket = s;
      this.recvBuf = Buffer.alloc(0);
      console.log(`[bridge] connected to ${this.name}`);
      this.onStatusChange(this.name, true);
    });

    s.on("data", (chunk: Buffer) => {
      this.recvBuf = Buffer.concat([this.recvBuf, chunk]);
      this.drain();
    });

    s.on("close", () => this.handleDisconnect());
    s.on("error", () => this.handleDisconnect());
  }

  private handleDisconnect() {
    if (!this._connected) return;
    this._connected = false;
    this.socket = null;
    console.log(`[bridge] disconnected from ${this.name}, retry in ${RECONNECT_MS}ms`);
    this.onStatusChange(this.name, false);
    this.reconnectTimer = setTimeout(() => this.connect(), RECONNECT_MS);
  }

  /** Parse length-prefixed frames from the receive buffer. */
  private drain() {
    while (this.recvBuf.length >= 4) {
      const len = this.recvBuf.readUInt32LE(0);
      if (this.recvBuf.length < 4 + len) break;
      const msg = this.recvBuf.slice(4, 4 + len).toString("utf8");
      this.recvBuf = this.recvBuf.slice(4 + len);
      this.onMessage(this.name, msg);
    }
  }

  /** Send a length-prefixed message to the service. */
  send(msg: string): boolean {
    if (!this.socket || !this._connected) return false;
    const payload = Buffer.from(msg, "utf8");
    const header = Buffer.alloc(4);
    header.writeUInt32LE(payload.length, 0);
    this.socket.write(Buffer.concat([header, payload]));
    return true;
  }

  destroy() {
    if (this.reconnectTimer) clearTimeout(this.reconnectTimer);
    this.socket?.destroy();
  }
}

// ─── Bridge server ────────────────────────────────────────────────────────────

class UiBridge {
  private sockets = new Map<ServiceName, ServiceSocket>();
  private wsClients = new Set<WebSocket>();
  private wss: WebSocketServer;

  constructor() {
    // HTTP server just for health check at GET /
    const http = createServer((req, res) => {
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(JSON.stringify({
        status: "ok",
        services: Object.fromEntries(
          [...this.sockets.entries()].map(([k, v]) => [k, v.connected])
        ),
      }));
    });

    this.wss = new WebSocketServer({ server: http });

    this.wss.on("connection", (ws) => {
      this.wsClients.add(ws);

      // Send current service status on connect
      this.sockets.forEach((svc, name) => {
        this.sendToWs(ws, {
          service: name,
          type: "status",
          payload: { connected: svc.connected },
        });
      });

      ws.on("message", (raw) => {
        try {
          const env = JSON.parse(raw.toString()) as Envelope;
          this.handleClientMessage(ws, env);
        } catch {
          ws.send(JSON.stringify({ type: "error", payload: "invalid JSON" }));
        }
      });

      ws.on("close", () => this.wsClients.delete(ws));
    });

    // Connect to all services
    for (const name of SERVICES) {
      this.sockets.set(
        name,
        new ServiceSocket(
          name,
          (svc, msg) => this.broadcast({ service: svc, type: "event", payload: JSON.parse(msg) }),
          (svc, up) => this.broadcast({ service: svc, type: "status", payload: { connected: up } })
        )
      );
    }

    http.listen(WS_PORT, "127.0.0.1", () => {
      console.log(`[bridge] WebSocket server listening on ws://127.0.0.1:${WS_PORT}`);
    });
  }

  /** Route a message from a WS client to the correct service socket. */
  private handleClientMessage(ws: WebSocket, env: Envelope) {
    const svc = this.sockets.get(env.service);
    if (!svc) {
      this.sendToWs(ws, { service: env.service, type: "error", id: env.id, payload: "unknown service" });
      return;
    }
    const ok = svc.send(JSON.stringify({ id: env.id, ...env.payload as object }));
    if (!ok) {
      this.sendToWs(ws, {
        service: env.service,
        type: "error",
        id: env.id,
        payload: `${env.service} not connected`,
      });
    }
  }

  private broadcast(env: Envelope) {
    const msg = JSON.stringify(env);
    this.wsClients.forEach((ws) => {
      if (ws.readyState === WebSocket.OPEN) ws.send(msg);
    });
  }

  private sendToWs(ws: WebSocket, env: Envelope) {
    if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(env));
  }
}

// ─── Entry ────────────────────────────────────────────────────────────────────

new UiBridge();

process.on("SIGTERM", () => process.exit(0));
process.on("SIGINT",  () => process.exit(0));
