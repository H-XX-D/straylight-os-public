/**
 * tools/ui-bridge/src/hooks/useBridge.ts
 *
 * Core WebSocket connection to the Straylight UI bridge.
 * All per-service hooks use this internally.
 */

import { useEffect, useRef, useCallback, useState } from "react";
import type { ServiceName, Envelope } from "../server";

export { ServiceName, Envelope };

const BRIDGE_URL = typeof window !== "undefined"
  ? (window as any).__SL_BRIDGE_URL__ ?? "ws://127.0.0.1:7700"
  : "ws://127.0.0.1:7700";

// Global singleton WS connection shared across all hook instances
let _ws: WebSocket | null = null;
let _listeners = new Set<(env: Envelope) => void>();
let _connectPromise: Promise<void> | null = null;

function getSocket(): Promise<void> {
  if (_ws && _ws.readyState === WebSocket.OPEN) return Promise.resolve();
  if (_connectPromise) return _connectPromise;

  _connectPromise = new Promise((resolve) => {
    const ws = new WebSocket(BRIDGE_URL);

    ws.onopen = () => {
      _ws = ws;
      _connectPromise = null;
      resolve();
    };

    ws.onmessage = (e) => {
      try {
        const env = JSON.parse(e.data) as Envelope;
        _listeners.forEach((fn) => fn(env));
      } catch { /* ignore malformed */ }
    };

    ws.onclose = () => {
      _ws = null;
      _connectPromise = null;
      // Reconnect after 2s
      setTimeout(() => getSocket(), 2000);
    };

    ws.onerror = () => ws.close();
  });

  return _connectPromise;
}

// Auto-connect on module load (client-side)
if (typeof window !== "undefined") getSocket();

/** Send a request envelope to the bridge. Returns a correlation id. */
export function sendRequest(service: ServiceName, payload: unknown): string {
  const id = `${service}-${Date.now()}-${Math.random().toString(36).slice(2, 7)}`;
  const env: Envelope = { service, type: "req", id, payload };
  const send = () => _ws?.send(JSON.stringify(env));
  if (_ws?.readyState === WebSocket.OPEN) {
    send();
  } else {
    getSocket().then(send);
  }
  return id;
}

/** Subscribe to all envelopes for a given service. Returns unsubscribe fn. */
export function subscribeService(
  service: ServiceName,
  handler: (env: Envelope) => void
): () => void {
  const filter = (env: Envelope) => {
    if (env.service === service) handler(env);
  };
  _listeners.add(filter);
  return () => _listeners.delete(filter);
}

/** Returns connection status for all services. */
export function useBridgeStatus(): Record<string, boolean> {
  const [status, setStatus] = useState<Record<string, boolean>>({});

  useEffect(() => {
    const unsub = (env: Envelope) => {
      if (env.type === "status") {
        setStatus((prev) => ({
          ...prev,
          [env.service]: (env.payload as { connected: boolean }).connected,
        }));
      }
    };
    _listeners.add(unsub);
    getSocket();
    return () => { _listeners.delete(unsub); };
  }, []);

  return status;
}

/** Generic hook: subscribe to events from a service and get latest payload. */
export function useServiceEvent<T = unknown>(
  service: ServiceName,
  eventFilter?: (payload: T) => boolean
): T | null {
  const [data, setData] = useState<T | null>(null);

  useEffect(() => {
    const unsub = subscribeService(service, (env) => {
      if (env.type !== "event") return;
      const payload = env.payload as T;
      if (!eventFilter || eventFilter(payload)) setData(payload);
    });
    return unsub;
  }, [service]);

  return data;
}

/** Generic hook: send a request and wait for a correlated response. */
export function useServiceRequest<TReq, TRes>(service: ServiceName) {
  const [loading, setLoading] = useState(false);
  const [data, setData] = useState<TRes | null>(null);
  const [error, setError] = useState<string | null>(null);
  const pendingRef = useRef<Map<string, (res: TRes | null, err: string | null) => void>>(new Map());

  useEffect(() => {
    const unsub = subscribeService(service, (env) => {
      if ((env.type === "res" || env.type === "error") && env.id) {
        const cb = pendingRef.current.get(env.id);
        if (cb) {
          pendingRef.current.delete(env.id);
          if (env.type === "error") cb(null, String(env.payload));
          else cb(env.payload as TRes, null);
        }
      }
    });
    return unsub;
  }, [service]);

  const request = useCallback((payload: TReq): Promise<TRes> => {
    setLoading(true);
    setError(null);
    return new Promise((resolve, reject) => {
      const id = sendRequest(service, payload);
      pendingRef.current.set(id, (res, err) => {
        setLoading(false);
        if (err) { setError(err); reject(new Error(err)); }
        else { setData(res); resolve(res!); }
      });
      // Timeout after 10s
      setTimeout(() => {
        if (pendingRef.current.has(id)) {
          pendingRef.current.delete(id);
          setLoading(false);
          setError("timeout");
          reject(new Error("timeout"));
        }
      }, 10_000);
    });
  }, [service]);

  return { request, loading, data, error };
}
