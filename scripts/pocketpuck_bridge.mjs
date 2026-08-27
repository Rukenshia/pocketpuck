#!/usr/bin/env bun

import { lstat, readFile, stat } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { createClient } from "rivetkit/client";

export const THREAD_LIMIT = 8;
export const MAX_DATA_AGE_MS = 30_000;
export const INITIAL_EMPTY_SETTLE_MS = 2_000;
export const PRIVATE_RETRY_INTERVAL_MS = 300_000;
export const PRIVATE_RESYNC_INTERVAL_MS = 20_000;
const AMP_URL = "https://ampcode.com";
const RIVET_PUBLIC_ENDPOINT =
  "https://default:pk_9tm4qz3zrMerdZXTlBRLRsmJIzSQIPH24meKBqiL6vVpscTvc4w1YPiBgymXf9Az@ampcode.com/actors";

export const DETAILED_STATES = [
  "idle",
  "compacting",
  "working",
  "streaming",
  "tool_use",
  "running_tools",
  "awaiting_approval",
  "error",
];
const ACTIVE_STATES = new Set([
  "compacting",
  "working",
  "streaming",
  "tool_use",
  "running_tools",
]);

class TerminalPrivateError extends Error {}

export async function resolveAmpApiKey(
  environment = process.env,
  dependencies = {},
) {
  if (environment.AMP_API_KEY) return environment.AMP_API_KEY;
  const dataHome =
    environment.XDG_DATA_HOME ||
    (environment.HOME
      ? path.join(environment.HOME, ".local", "share")
      : null);
  if (!dataHome) throw new Error("HOME is unavailable");
  const filePath = path.join(dataHome, "amp", "secrets.json");
  const deps = {
    lstat,
    stat,
    readText: (target) => readFile(target, "utf8"),
    currentUid: () =>
      typeof process.getuid === "function" ? process.getuid() : undefined,
    ...dependencies,
  };
  const linkMetadata = await deps.lstat(filePath);
  if (linkMetadata.isSymbolicLink()) {
    throw new Error("Amp secret store may not be a symlink");
  }
  const metadata = await deps.stat(filePath);
  if (
    !metadata.isFile() ||
    (typeof deps.currentUid() === "number" &&
      metadata.uid !== deps.currentUid()) ||
    (metadata.mode & 0o077) !== 0
  ) {
    throw new Error("Amp secret store failed safety checks");
  }
  let store;
  try {
    store = JSON.parse(await deps.readText(filePath));
  } catch {
    throw new Error("Amp secret store is malformed");
  }
  const apiKey = store?.["apiKey@https://ampcode.com/"];
  if (typeof apiKey !== "string" || apiKey.length === 0) {
    throw new Error("production Amp API key not found");
  }
  return apiKey;
}

function projectFromWorkspace(workspace) {
  if (!workspace || typeof workspace.uri !== "string") return "";
  try {
    const uri = new URL(workspace.uri);
    const workspacePath =
      uri.protocol === "file:"
        ? fileURLToPath(uri)
        : decodeURIComponent(uri.pathname);
    return path.basename(workspacePath);
  } catch {
    return path.basename(workspace.uri);
  }
}

function itemPriority(thread) {
  if (thread.state === "awaiting_approval") return 0;
  if (thread.state === "error") return 1;
  if (thread.hasUnreadMessages === true) return 2;
  if (ACTIVE_STATES.has(thread.state)) return 3;
  return 4;
}

function detailedState(thread) {
  if (thread.state === "error") return "error";
  if (thread.indicator?.kind === "action-required") return "awaiting_approval";
  return DETAILED_STATES.includes(thread.state) ? thread.state : "unknown";
}

export class BridgeCache {
  constructor(clock = () => performance.now()) {
    this.clock = clock;
    this.value = null;
    this.receivedAt = null;
    this.availableAt = null;
    this.hasPublishedData = false;
  }

  using(source) {
    return this.value?.source === source;
  }

  publish(value, hasThreads, settleEmpty = false) {
    const now = this.clock();
    this.value = value;
    this.receivedAt = now;
    this.availableAt =
      settleEmpty && !hasThreads && !this.hasPublishedData
        ? now + INITIAL_EMPTY_SETTLE_MS
        : now;
    if (hasThreads) this.hasPublishedData = true;
  }

  markStale(source) {
    if (!this.using(source)) return;
    this.value = { ...this.value, reconnecting: true, stale: true };
  }

  read() {
    const now = this.clock();
    if (
      this.receivedAt === null ||
      now < this.availableAt ||
      now - this.receivedAt > MAX_DATA_AGE_MS
    ) {
      return null;
    }
    return this.value;
  }

  updatePublic(event, allowEmptySourceSwitch = false) {
    if (!Array.isArray(event.threads)) return;
    if (
      event.threads.length === 0 &&
      this.using("user-actor") &&
      !allowEmptySourceSwitch
    ) {
      return;
    }
    const threads = event.threads.filter(
      (thread) =>
        thread &&
        typeof thread === "object" &&
        typeof thread.working === "boolean" &&
        typeof thread.executorConnected === "boolean",
    );
    const running = threads.filter((thread) => thread.working).length;
    const idle = threads.length - running;
    const executorConnected = threads.filter(
      (thread) => thread.executorConnected,
    ).length;
    const reconnecting = event.reconnecting === true;
    const items = threads.slice(0, THREAD_LIMIT).map((thread) => ({
      id: typeof thread.id === "string" ? thread.id : null,
      title:
        typeof thread.title === "string" && thread.title
          ? thread.title
          : "Untitled thread",
      project: typeof thread.project === "string" ? thread.project : "",
      state: thread.working ? "working" : "idle",
      executorConnected: thread.executorConnected,
      unread: false,
    }));
    this.publish(
      {
        schemaVersion: 2,
        source: "amp-top",
        capabilities: { detailedStates: false, unread: false },
        working: running,
        needsAttention: 0,
        running,
        idle,
        total: threads.length,
        states: null,
        unread: null,
        executorConnected,
        headline: { working: running, needsAttention: null, idle },
        threads: {
          working: {
            withExecutor: threads.filter(
              (thread) => thread.working && thread.executorConnected,
            ).length,
            withoutExecutor: threads.filter(
              (thread) => thread.working && !thread.executorConnected,
            ).length,
          },
          idle: {
            withExecutor: threads.filter(
              (thread) => !thread.working && thread.executorConnected,
            ).length,
            withoutExecutor: threads.filter(
              (thread) => !thread.working && !thread.executorConnected,
            ).length,
          },
        },
        items,
        updatedAt: event.updatedAt ?? new Date().toISOString(),
        reconnecting,
        stale: reconnecting,
      },
      threads.length > 0,
      true,
    );
  }

  updatePrivate(rawThreads, reconnecting = false) {
    if (!Array.isArray(rawThreads)) return;
    const threads = rawThreads
      .filter((thread) => thread && typeof thread === "object")
      .map((thread) => ({
        ...thread,
        state: detailedState(thread),
        project: projectFromWorkspace(thread.workspace),
        workspaceDisplayName:
          typeof thread.workspace?.displayName === "string"
            ? thread.workspace.displayName
            : "",
      }))
      .sort((left, right) =>
        String(right.updatedAt ?? "").localeCompare(String(left.updatedAt ?? "")),
      )
      .sort((left, right) => itemPriority(left) - itemPriority(right));
    const states = Object.fromEntries(
      [...DETAILED_STATES, "unknown"].map((state) => [state, 0]),
    );
    for (const thread of threads) states[thread.state] += 1;
    const unread = threads.filter(
      (thread) => thread.hasUnreadMessages === true,
    ).length;
    const executorConnected = threads.filter(
      (thread) => thread.executorConnected === true,
    ).length;
    const running =
      [...ACTIVE_STATES].reduce((sum, state) => sum + states[state], 0) +
      states.awaiting_approval;
    const needsAttention = threads.filter(
      (thread) =>
        thread.state === "awaiting_approval" ||
        thread.state === "error",
    ).length;
    const headlineWorking = threads.filter(
      (thread) => ACTIVE_STATES.has(thread.state),
    ).length;
    const headlineIdle = threads.filter(
      (thread) => thread.state === "idle",
    ).length;
    const items = threads.slice(0, THREAD_LIMIT).map((thread) => ({
      id: typeof thread.threadId === "string" ? thread.threadId : null,
      title:
        typeof thread.title === "string" && thread.title
          ? thread.title
          : "Untitled thread",
      project: thread.project,
      workspaceDisplayName: thread.workspaceDisplayName,
      state: thread.state,
      executorConnected: thread.executorConnected === true,
      unread: thread.hasUnreadMessages === true,
    }));
    this.publish(
      {
        schemaVersion: 2,
        source: "user-actor",
        capabilities: { detailedStates: true, unread: true },
        working: headlineWorking,
        needsAttention,
        running,
        idle: states.idle,
        total: threads.length,
        states,
        unread,
        executorConnected,
        headline: {
          working: headlineWorking,
          needsAttention,
          idle: headlineIdle,
        },
        attention: {
          awaitingApproval: states.awaiting_approval,
          error: states.error,
        },
        items,
        updatedAt: new Date().toISOString(),
        reconnecting,
        stale: reconnecting,
      },
      threads.length > 0,
    );
  }
}

async function bootstrapCredentials(configuration) {
  const response = await fetch(
    `${AMP_URL}/api/user-actor-credentials`,
    {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        Authorization: `Bearer ${configuration.apiKey}`,
      },
      body: "{}",
    },
  );
  if (response.status === 401 || response.status === 403) {
    throw new TerminalPrivateError(`Amp credential request returned ${response.status}`);
  }
  if (!response.ok) {
    throw new Error(`Amp credential request returned ${response.status}`);
  }
  const credentials = await response.json();
  if (
    typeof credentials.userId !== "string" ||
    typeof credentials.wsToken !== "string" ||
    typeof credentials.poolName !== "string"
  ) {
    throw new TerminalPrivateError("Amp credential response shape changed");
  }
  return credentials;
}

async function withTimeout(promise, timeoutMs, message) {
  let timer;
  try {
    return await Promise.race([
      promise,
      new Promise((_, reject) => {
        timer = setTimeout(() => reject(new Error(message)), timeoutMs);
      }),
    ]);
  } finally {
    clearTimeout(timer);
  }
}

export async function connectPrivateOnce(
  cache,
  configuration,
  initialCredentials,
  {
    createActorClient = createClient,
    resyncIntervalMs = PRIVATE_RESYNC_INTERVAL_MS,
  } = {},
) {
  let credentials = initialCredentials;
  const client = createActorClient({
    endpoint: RIVET_PUBLIC_ENDPOINT,
    poolName: credentials.poolName,
    devtools: false,
  });
  const handle = client.userActor.get([credentials.userId], {
    getParams: async () => {
      const next = await bootstrapCredentials(configuration);
      if (
        next.userId !== initialCredentials.userId ||
        next.poolName !== initialCredentials.poolName
      ) {
        throw new TerminalPrivateError("Amp actor identity changed during reconnect");
      }
      credentials = next;
      return { type: "user", wsToken: next.wsToken };
    },
  });
  const connection = handle.connect();
  let threads = new Map();
  let buffered = [];
  let loading = true;
  let loaded = false;
  let connected = false;
  let periodicResync;
  let disconnectTimer;
  let rejectFailure;
  let resyncPromise;
  const failure = new Promise((_, reject) => {
    rejectFailure = reject;
  });

  const apply = (target, summary) => {
    if (!summary || typeof summary.threadId !== "string") return;
    if (summary.archived === true) target.delete(summary.threadId);
    else target.set(summary.threadId, summary);
  };
  const offThread = connection.on("threadStatusUpdated", (summary) => {
    if (loading) buffered.push(summary);
    else {
      apply(threads, summary);
      if (connected) cache.updatePrivate([...threads.values()], false);
      else cache.markStale("user-actor");
    }
  });
  const resync = () => {
    if (resyncPromise) return resyncPromise;
    resyncPromise = (async () => {
      loading = true;
      buffered = [];
      const baseline = await withTimeout(
        connection.action({
          name: "getRecentThreads",
          args: [{ limit: 200, sinceMs: Date.now() - 86_400_000 }],
        }),
        10_000,
        "Amp actor baseline timed out",
      );
      if (!Array.isArray(baseline)) {
        throw new TerminalPrivateError("Amp actor baseline shape changed");
      }
      const replacement = new Map();
      baseline.forEach((summary) => apply(replacement, summary));
      buffered.forEach((summary) => apply(replacement, summary));
      threads = replacement;
      loading = false;
      connected = true;
      cache.updatePrivate([...threads.values()], false);
    })().finally(() => {
      resyncPromise = null;
    });
    return resyncPromise;
  };
  const offStatus = connection.onStatusChange((status) => {
    clearTimeout(disconnectTimer);
    if (status !== "connected") {
      connected = false;
      cache.markStale("user-actor");
      disconnectTimer = setTimeout(
        () => rejectFailure(new Error("Amp actor remained disconnected")),
        30_000,
      );
    } else if (loaded) {
      connected = false;
      resync().catch(rejectFailure);
    } else {
      connected = true;
    }
  });
  const offError = connection.onError(rejectFailure);

  try {
    await withTimeout(connection.ready, 10_000, "Amp actor ready timed out");
    await resync();
    loaded = true;
    periodicResync = setInterval(() => {
      if (connected) resync().catch(rejectFailure);
      else cache.markStale("user-actor");
    }, resyncIntervalMs);
    await failure;
  } finally {
    clearInterval(periodicResync);
    clearTimeout(disconnectTimer);
    if (typeof offThread === "function") offThread();
    if (typeof offStatus === "function") offStatus();
    if (typeof offError === "function") offError();
    await connection.dispose();
  }
}

async function runPrivate(cache, configuration) {
  let delayMs = 1_000;
  for (let attempt = 0; attempt < 5; attempt += 1) {
    try {
      const credentials = await bootstrapCredentials(configuration);
      await connectPrivateOnce(cache, configuration, credentials);
    } catch (error) {
      console.error(
        error instanceof TerminalPrivateError
          ? "Detailed Amp summaries are incompatible or unauthorized"
          : "Detailed Amp summaries lost connection",
      );
      cache.markStale("user-actor");
      if (error instanceof TerminalPrivateError) return;
    }
    if (attempt < 4) {
      const jitter = 0.8 + Math.random() * 0.4;
      await Bun.sleep(delayMs * jitter);
      delayMs = Math.min(delayMs * 2, 30_000);
    }
  }
}

async function runPublic(cache, ampCommand, durationMs) {
  console.log("Using amp top fallback");
  const process = Bun.spawn([ampCommand, "top", "--stream-jsonl"], {
    stdout: "pipe",
    stderr: "inherit",
  });
  const timer =
    durationMs === null
      ? null
      : setTimeout(() => process.kill(), durationMs);
  const decoder = new TextDecoder();
  let pending = "";
  let emptySwitchTimer;
  const consumeLine = (line) => {
    try {
      const event = JSON.parse(line);
      clearTimeout(emptySwitchTimer);
      if (
        Array.isArray(event.threads) &&
        event.threads.length === 0 &&
        cache.using("user-actor")
      ) {
        emptySwitchTimer = setTimeout(
          () => cache.updatePublic(event, true),
          INITIAL_EMPTY_SETTLE_MS,
        );
      } else {
        cache.updatePublic(event);
      }
    } catch {
      console.error(`Ignoring invalid amp top output: ${line}`);
    }
  };
  try {
    for await (const chunk of process.stdout) {
      pending += decoder.decode(chunk, { stream: true });
      const lines = pending.split("\n");
      pending = lines.pop();
      for (const line of lines) consumeLine(line);
    }
    pending += decoder.decode();
    if (pending.trim()) consumeLine(pending);
    await process.exited;
  } finally {
    clearTimeout(timer);
    clearTimeout(emptySwitchTimer);
    cache.markStale("amp-top");
  }
}

export function statsResponse(cache) {
  const value = cache.read();
  if (value === null) {
    return Response.json(
      { error: "waiting for Amp" },
      { status: 503, headers: { "Cache-Control": "no-store" } },
    );
  }
  return Response.json(value, {
    headers: { "Cache-Control": "no-store" },
  });
}

function parseArguments(args) {
  const options = {
    host: process.env.POCKETPUCK_HOST || "0.0.0.0",
    port: Number(process.env.POCKETPUCK_PORT || 8765),
    ampCommand: process.env.AMP_COMMAND || "amp",
  };
  for (let index = 0; index < args.length; index += 1) {
    const value = args[index + 1];
    if (args[index] === "--host") options.host = value;
    if (args[index] === "--port") options.port = Number(value);
    if (args[index] === "--amp-command") options.ampCommand = value;
    if (args[index].startsWith("--")) index += 1;
  }
  return options;
}

export async function followAmpCycle(
  cache,
  ampCommand,
  {
    resolveApiKey = resolveAmpApiKey,
    privateSource = runPrivate,
    publicSource = runPublic,
    logger = console,
  } = {},
) {
  let apiKey;
  try {
    apiKey = await resolveApiKey();
  } catch {
    logger.error("Private Amp integration unavailable: API key discovery failed");
  }
  if (apiKey) {
    logger.log("Trying detailed Amp user-actor summaries");
    await privateSource(cache, { apiKey });
    logger.log("Private Amp integration unavailable; starting fallback");
  }
  await publicSource(cache, ampCommand, PRIVATE_RETRY_INTERVAL_MS);
}

async function followAmp(cache, ampCommand) {
  while (true) {
    try {
      await followAmpCycle(cache, ampCommand);
    } catch (error) {
      console.error(`amp top fallback failed: ${error.message}`);
    }
    await Bun.sleep(5_000);
  }
}

if (import.meta.main) {
  const options = parseArguments(Bun.argv.slice(2));
  const cache = new BridgeCache();
  Bun.serve({
    hostname: options.host,
    port: options.port,
    fetch(request) {
      const url = new URL(request.url);
      if (url.pathname !== "/stats") return new Response("Not Found", { status: 404 });
      return statsResponse(cache);
    },
  });
  console.log(
    `PocketPuck bridge listening on http://${options.host}:${options.port}/stats`,
  );
  followAmp(cache, options.ampCommand).catch((error) => {
    console.error(`PocketPuck bridge stopped: ${error.stack ?? error}`);
    process.exit(1);
  });
}
