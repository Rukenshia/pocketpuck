import { describe, expect, test } from "bun:test";
import {
  BridgeCache,
  INITIAL_EMPTY_SETTLE_MS,
  MAX_DATA_AGE_MS,
  THREAD_LIMIT,
  statsResponse,
} from "./pocketpuck_bridge.mjs";

function publicThread(index, working = false) {
  return {
    id: String(index),
    title: `Long title ${index} ${"x".repeat(100)}`,
    project: "pocketpuck",
    working,
    executorConnected: index % 2 === 0,
  };
}

describe("BridgeCache", () => {
  test("settles initial empty data and expires stale snapshots", async () => {
    let now = 100;
    const cache = new BridgeCache(() => now);
    expect((await statsResponse(cache).json()).error).toBe("waiting for Amp");
    expect(statsResponse(cache).status).toBe(503);
    cache.updatePublic({ threads: [], reconnecting: false });
    expect(cache.read()).toBeNull();
    now += INITIAL_EMPTY_SETTLE_MS;
    expect(cache.read().items).toEqual([]);
    now += MAX_DATA_AGE_MS + 1;
    expect(cache.read()).toBeNull();
  });

  test("loaded data replaces initial empty without zero flicker", () => {
    const cache = new BridgeCache();
    cache.updatePublic({ threads: [], reconnecting: false });
    expect(cache.read()).toBeNull();
    cache.updatePublic({ threads: [publicThread(1, true)] });
    expect(cache.read().running).toBe(1);
  });

  test("public fallback is bounded and marks unavailable details", () => {
    const cache = new BridgeCache();
    const threads = Array.from({ length: THREAD_LIMIT + 5 }, (_, index) =>
      publicThread(index, index % 2 === 0),
    );
    cache.updatePublic({ threads, reconnecting: true });
    const value = cache.read();
    expect(value.source).toBe("amp-top");
    expect(value.items).toHaveLength(THREAD_LIMIT);
    expect(value.running + value.idle).toBe(threads.length);
    expect(value.states).toBeNull();
    expect(value.unread).toBeNull();
    expect(value.stale).toBeTrue();
  });

  test("private summaries preserve detailed states and independent unread", () => {
    const cache = new BridgeCache();
    const summaries = [
      ["idle", false],
      ["working", false],
      ["streaming", true],
      ["awaiting_approval", true],
      ["error", false],
      ["future_state", false],
    ].map(([state, unread], index) => ({
      threadId: String(index),
      title: state,
      state,
      hasUnreadMessages: unread,
      executorConnected: index % 2 === 0,
      updatedAt: `2026-08-26T21:02:0${index}Z`,
      workspace: {
        uri: "file:///example/checkouts/pocketpuck",
        displayName: "Friendly Puck",
      },
    }));
    cache.updatePrivate(summaries);
    const value = cache.read();
    expect(value.source).toBe("user-actor");
    expect(value.running).toBe(3);
    expect(value.idle).toBe(1);
    expect(value.unread).toBe(2);
    expect(value.states.unknown).toBe(1);
    expect(value.attention).toEqual({ awaitingApproval: 1, error: 1, unread: 2 });
    expect(value.items[0].state).toBe("awaiting_approval");
    expect(value.items[0].project).toBe("pocketpuck");
    expect(value.items[0].workspaceDisplayName).toBe("Friendly Puck");
  });

  test("switches sources only after a complete snapshot", () => {
    const cache = new BridgeCache();
    cache.updatePublic({ threads: [publicThread(1)] });
    cache.markStale("user-actor");
    expect(cache.read().source).toBe("amp-top");
    cache.updatePrivate([
      {
        threadId: "1",
        state: "idle",
        executorConnected: true,
        hasUnreadMessages: false,
      },
    ]);
    expect(cache.read().source).toBe("user-actor");
    cache.updatePublic({ threads: [], reconnecting: false });
    expect(cache.read().source).toBe("user-actor");
  });

  test("marks retained private data stale during reconnect", () => {
    const cache = new BridgeCache();
    cache.updatePrivate([
      {
        threadId: "1",
        title: "Keep me",
        state: "streaming",
        executorConnected: true,
        hasUnreadMessages: false,
      },
    ]);
    cache.markStale("user-actor");
    const value = cache.read();
    expect(value.reconnecting).toBeTrue();
    expect(value.stale).toBeTrue();
    expect(value.items[0].title).toBe("Keep me");
  });
});
