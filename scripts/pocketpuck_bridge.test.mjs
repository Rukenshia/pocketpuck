import { describe, expect, test } from "bun:test";
import {
  BridgeCache,
  INITIAL_EMPTY_SETTLE_MS,
  MAX_DATA_AGE_MS,
  THREAD_LIMIT,
  connectPrivateOnce,
  followAmpCycle,
  resolveAmpApiKey,
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
    expect(value.needsAttention).toBe(2);
    expect(value.headline).toEqual({
      working: 2,
      needsAttention: 2,
      idle: 1,
    });
    expect(value.states.unknown).toBe(1);
    expect(value.attention).toEqual({ awaitingApproval: 1, error: 1 });
    expect(value.items[0].state).toBe("awaiting_approval");
    expect(value.items[0].project).toBe("pocketpuck");
    expect(value.items[0].workspaceDisplayName).toBe("Friendly Puck");
  });

  test("maps the live confirmation indicator to awaiting approval", () => {
    const cache = new BridgeCache();
    cache.updatePrivate([
      {
        threadId: "T-01a04318-5512-770b-9179-59251371964a",
        title: "PocketPuck approval testing",
        state: "tool_use",
        executorConnected: true,
        indicator: {
          kind: "action-required",
          description: "PocketPuck approval test",
          observedAt: "Thu, 27 Aug 2026 12:10:58 GMT",
        },
        hasUnreadMessages: false,
        updatedAt: "2026-08-27T12:10:58.128Z",
        workspace: {
          uri: "file:///home/user/workspace/repo",
          displayName: "repo",
        },
      },
    ]);

    const value = cache.read();
    expect(value.needsAttention).toBe(1);
    expect(value.working).toBe(0);
    expect(value.running).toBe(1);
    expect(value.states.tool_use).toBe(0);
    expect(value.states.awaiting_approval).toBe(1);
    expect(value.attention).toEqual({ awaitingApproval: 1, error: 0 });
    expect(value.items[0].state).toBe("awaiting_approval");
  });

  test("does not infer attention from idle, unread, executor, or active states", () => {
    const cache = new BridgeCache();
    cache.updatePrivate([
      {
        threadId: "idle",
        state: "idle",
        executorConnected: false,
        hasUnreadMessages: false,
      },
      {
        threadId: "unread",
        state: "idle",
        executorConnected: false,
        hasUnreadMessages: true,
      },
      {
        threadId: "attached",
        state: "idle",
        executorConnected: true,
        hasUnreadMessages: false,
      },
      {
        threadId: "tool",
        state: "tool_use",
        executorConnected: true,
        hasUnreadMessages: false,
      },
      {
        threadId: "working-indicator",
        state: "working",
        indicator: { kind: "in-progress" },
        executorConnected: true,
        hasUnreadMessages: false,
      },
    ]);

    const value = cache.read();
    expect(value.needsAttention).toBe(0);
    expect(value.unread).toBe(1);
    expect(value.executorConnected).toBe(3);
    expect(value.states.idle).toBe(3);
    expect(value.states.tool_use).toBe(1);
    expect(value.states.working).toBe(1);
  });

  test("preserves errors when an action-required indicator is also present", () => {
    const cache = new BridgeCache();
    cache.updatePrivate([
      {
        threadId: "error",
        state: "error",
        indicator: { kind: "action-required" },
      },
    ]);

    const value = cache.read();
    expect(value.states.error).toBe(1);
    expect(value.states.awaiting_approval).toBe(0);
    expect(value.attention).toEqual({ awaitingApproval: 0, error: 1 });
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
    let now = 100;
    const cache = new BridgeCache(() => now);
    cache.updatePrivate([
      {
        threadId: "1",
        title: "Keep me",
        state: "streaming",
        executorConnected: true,
        hasUnreadMessages: false,
      },
    ]);
    const updatedAt = cache.read().updatedAt;
    now += MAX_DATA_AGE_MS;
    cache.markStale("user-actor");
    const value = cache.read();
    expect(value.reconnecting).toBeTrue();
    expect(value.stale).toBeTrue();
    expect(value.items[0].title).toBe("Keep me");
    expect(value.updatedAt).toBe(updatedAt);
    now += 1;
    expect(cache.read()).toBeNull();
  });
});

describe("private actor liveness", () => {
  test("periodically replaces the event map with an authoritative baseline", async () => {
    let actionCount = 0;
    let disposed = false;
    const connection = {
      ready: Promise.resolve(),
      on: () => () => {},
      onStatusChange: () => () => {},
      onError: () => () => {},
      action: async () => {
        actionCount += 1;
        if (actionCount === 1) {
          return [{ threadId: "1", title: "Old", state: "idle" }];
        }
        if (actionCount === 2) {
          return [{ threadId: "1", title: "Fresh", state: "working" }];
        }
        throw new Error("stop fixture");
      },
      dispose: async () => {
        disposed = true;
      },
    };
    const createActorClient = () => ({
      userActor: {
        get: () => ({ connect: () => connection }),
      },
    });
    const cache = new BridgeCache();

    await expect(
      connectPrivateOnce(
        cache,
        { apiKey: "fixture" },
        { userId: "user", wsToken: "token", poolName: "pool" },
        { createActorClient, resyncIntervalMs: 5 },
      ),
    ).rejects.toThrow("stop fixture");

    expect(actionCount).toBe(3);
    expect(cache.read().items[0].title).toBe("Fresh");
    expect(disposed).toBeTrue();
  });
});

describe("source orchestration", () => {
  test("starts public fallback when private discovery fails", async () => {
    const calls = [];
    const logs = [];
    await followAmpCycle(new BridgeCache(), "fixture-amp", {
      resolveApiKey: async () => {
        throw new Error("DO_NOT_LOG_THIS_SECRET");
      },
      privateSource: async () => calls.push("private"),
      publicSource: async (_cache, command, duration) =>
        calls.push(["public", command, duration]),
      logger: {
        log: (message) => logs.push(message),
        error: (message) => logs.push(message),
      },
    });
    expect(calls).toEqual([
      ["public", "fixture-amp", 300_000],
    ]);
    expect(logs.join(" ")).not.toContain("DO_NOT_LOG_THIS_SECRET");
  });
});

describe("production API key resolution", () => {
  test("prefers AMP_API_KEY without reading the secret store", async () => {
    expect(
      await resolveAmpApiKey(
        { AMP_API_KEY: "override" },
        { lstat: () => { throw new Error("unexpected read"); } },
      ),
    ).toBe("override");
  });

  test("reads the exact production key from XDG data home", async () => {
    const metadata = {
      uid: 501,
      mode: 0o100600,
      isFile: () => true,
      isSymbolicLink: () => false,
    };
    let requestedPath;
    const apiKey = await resolveAmpApiKey(
      { XDG_DATA_HOME: "/portable/data" },
      {
        lstat: async (target) => {
          requestedPath = target;
          return metadata;
        },
        stat: async () => metadata,
        currentUid: () => 501,
        readText: async () =>
          JSON.stringify({ "apiKey@https://ampcode.com/": "stored" }),
      },
    );
    expect(requestedPath).toBe("/portable/data/amp/secrets.json");
    expect(apiKey).toBe("stored");
  });

  test("rejects unsafe or malformed stores", async () => {
    const metadata = {
      uid: 501,
      mode: 0o100644,
      isFile: () => true,
      isSymbolicLink: () => false,
    };
    await expect(
      resolveAmpApiKey(
        { HOME: "/portable/home" },
        {
          lstat: async () => metadata,
          stat: async () => metadata,
          currentUid: () => 501,
          readText: async () => "{broken",
        },
      ),
    ).rejects.toThrow("safety");
    await expect(
      resolveAmpApiKey(
        { HOME: "/portable/home" },
        {
          lstat: async () => ({ ...metadata, mode: 0o100600 }),
          stat: async () => ({ ...metadata, mode: 0o100600 }),
          currentUid: () => 501,
          readText: async () => "{broken",
        },
      ),
    ).rejects.toThrow("malformed");
  });
});
