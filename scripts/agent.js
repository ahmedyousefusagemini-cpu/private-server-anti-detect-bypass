/*
 * agent.js - the in-process tooling loaded into the Frida Gadget.
 *
 * This script runs inside Conquer.exe. Everything it exposes through
 * rpc.exports is callable from a Frida client on the other side of the
 * Gadget's listener, which is how you actually drive the target:
 *
 *     hook functions      -> hookExport()
 *     search for values   -> scanStart() / scanRefine() / scanResults()
 *     change values       -> writeValue()
 *     inspect the process -> modules() / ranges() / threads() / readValue()
 *
 * Design notes
 * ------------
 * Reads are free; writes and hooks change the target. Nothing here blocks or
 * asks for confirmation, because this file is the raw capability layer. Any
 * approval policy belongs on the host side, where a mistake is recoverable.
 *
 * Scanning uses the classic two-mode model:
 *   - known value   -> exact byte pattern search (fast, no snapshot)
 *   - unknown value -> snapshot the aligned slots, then narrow by comparing
 *                      against that snapshot (changed / increased / ...),
 *                      which is how you find a value you can see but cannot
 *                      read, such as a health bar.
 * Refines compose: each one advances the snapshot to "now".
 */

'use strict';

/* ------------------------------------------------------------------ types */

const TYPES = {
  int8: { size: 1, read: (p) => p.readS8(), write: (p, v) => p.writeS8(v) },
  uint8: { size: 1, read: (p) => p.readU8(), write: (p, v) => p.writeU8(v) },
  int16: { size: 2, read: (p) => p.readS16(), write: (p, v) => p.writeS16(v) },
  uint16: { size: 2, read: (p) => p.readU16(), write: (p, v) => p.writeU16(v) },
  int32: { size: 4, read: (p) => p.readS32(), write: (p, v) => p.writeS32(v) },
  uint32: { size: 4, read: (p) => p.readU32(), write: (p, v) => p.writeU32(v) },
  int64: { size: 8, read: (p) => p.readS64().toString(), write: (p, v) => p.writeS64(v) },
  uint64: { size: 8, read: (p) => p.readU64().toString(), write: (p, v) => p.writeU64(v) },
  float: { size: 4, read: (p) => p.readFloat(), write: (p, v) => p.writeFloat(v) },
  double: { size: 8, read: (p) => p.readDouble(), write: (p, v) => p.writeDouble(v) },
  pointer: {
    size: Process.pointerSize,
    read: (p) => p.readPointer().toString(),
    write: (p, v) => p.writePointer(ptr(v)),
  },
};

// DataView readers, for snapshotting whole ranges in one read. 64-bit types
// are deliberately absent: DataView has no 64-bit accessor, so unknown-value
// scans support the 1/2/4/8-byte primitives below and reject the rest.
const VIEW_READERS = {
  int8: (dv, o) => dv.getInt8(o),
  uint8: (dv, o) => dv.getUint8(o),
  int16: (dv, o) => dv.getInt16(o, true),
  uint16: (dv, o) => dv.getUint16(o, true),
  int32: (dv, o) => dv.getInt32(o, true),
  uint32: (dv, o) => dv.getUint32(o, true),
  float: (dv, o) => dv.getFloat32(o, true),
  double: (dv, o) => dv.getFloat64(o, true),
};

const FLOAT_TYPES = { float: true, double: true };

const DEFAULT_TOLERANCE = 1e-6;
const MAX_READ_LENGTH = 64 * 1024;
const DEFAULT_MAX_BYTES = 64 * 1024 * 1024;
const DEFAULT_MAX_CANDIDATES = 200000;
const DEFAULT_MAX_EVENTS = 10000;

/* --------------------------------------------------------------- utilities */

function hexOf(byteArray) {
  const bytes = new Uint8Array(byteArray);
  let out = '';
  for (let i = 0; i < bytes.length; i++) {
    out += (bytes[i] < 16 ? '0' : '') + bytes[i].toString(16);
  }
  return out;
}

function bytesFromHex(hex) {
  const clean = hex.replace(/\s+/g, '');
  if (clean.length % 2 !== 0) {
    throw new Error('hex string must have an even number of digits');
  }
  const out = new Uint8Array(clean.length / 2);
  for (let i = 0; i < out.length; i++) {
    out[i] = parseInt(clean.substr(i * 2, 2), 16);
  }
  return out;
}

function requireType(type) {
  if (!TYPES[type]) {
    throw new Error('unsupported type: ' + type);
  }
  return TYPES[type];
}

function aligned(address, alignment) {
  return address.toUInt32() % alignment === 0;
}

function tryCall(fn) {
  try {
    const value = fn();
    return value === null || value === undefined ? null : value;
  } catch (error) {
    return null;
  }
}

// Resolve a symbol to an address.
//
// Frida 17 removed the static Module.findExportByName / getExportByName /
// enumerateExports. Per-module lookup moved onto the Module instances that
// Process.getModuleByName and Process.enumerateModules return; only
// Module.getGlobalExportByName and Module.findGlobalExportByName are still
// static, and they throw rather than return null when the symbol is absent.
// The legacy statics are tried last so this keeps working on older Frida.
function resolveExport(moduleName, symbol) {
  if (moduleName) {
    const module = tryCall(() => Process.getModuleByName(moduleName));
    if (module) {
      const address =
        tryCall(() => module.getExportByName(symbol)) ||
        tryCall(() => module.findExportByName(symbol));
      if (address) {
        return address;
      }
    }
  }

  const global =
    tryCall(() => Module.getGlobalExportByName(symbol)) ||
    tryCall(() => Module.findGlobalExportByName(symbol)) ||
    tryCall(() => Module.getExportByName(moduleName || null, symbol)) ||
    tryCall(() => Module.findExportByName(moduleName || null, symbol));

  if (global) {
    return global;
  }

  throw new Error('export not found: ' + (moduleName ? moduleName + '!' : '') + symbol);
}

/* ------------------------------------------------------------------ reads */

rpc.exports.ping = function () {
  return {
    ok: true,
    arch: Process.arch,
    pointerSize: Process.pointerSize,
    platform: Process.platform,
    pid: Process.id,
    processName: Process.mainModule ? Process.mainModule.name : null,
    pageSize: Process.pageSize,
    runtime: Script.runtime,
    fridaVersion: Frida.version,
  };
};

rpc.exports.modules = function () {
  return Process.enumerateModules().map((m) => ({
    name: m.name,
    base: m.base.toString(),
    size: m.size,
    path: m.path,
  }));
};

rpc.exports.ranges = function (protection) {
  return Process.enumerateRanges(protection || 'r--').map((r) => ({
    base: r.base.toString(),
    size: r.size,
    protection: r.protection,
    file: r.file ? { path: r.file.path, offset: r.file.offset, size: r.file.size } : null,
  }));
};

rpc.exports.threads = function () {
  return Process.enumerateThreads().map((t) => ({
    id: t.id,
    name: t.name || null,
    state: t.state,
  }));
};

rpc.exports.readValue = function (address, type, length) {
  const target = ptr(address);

  if (type === 'utf8' || type === 'utf16') {
    const limit = length ? Math.min(length, MAX_READ_LENGTH) : -1;
    const value = type === 'utf8' ? target.readUtf8String(limit) : target.readUtf16String(limit);
    return { type, value };
  }

  if (type === 'bytes') {
    const count = Math.min(length || 16, MAX_READ_LENGTH);
    return { type, length: count, value: hexOf(target.readByteArray(count)) };
  }

  const spec = requireType(type);
  return { type, size: spec.size, value: spec.read(target) };
};

rpc.exports.writeValue = function (address, type, value) {
  const target = ptr(address);

  if (type === 'utf8') {
    target.writeUtf8String(value);
    return { type, address: target.toString(), written: true };
  }
  if (type === 'utf16') {
    target.writeUtf16String(value);
    return { type, address: target.toString(), written: true };
  }
  if (type === 'bytes') {
    const bytes = bytesFromHex(value);
    target.writeByteArray(bytes);
    return { type, address: target.toString(), bytes: bytes.length, written: true };
  }

  const spec = requireType(type);
  spec.write(target, value);
  return { type, address: target.toString(), value, written: true };
};

rpc.exports.resolveExport = function (moduleName, symbol) {
  return resolveExport(moduleName || null, symbol).toString();
};

// List a module's exports, optionally filtered by substring. This is how you
// find something worth hooking. Uses the Frida 17 instance API; the static
// Module.enumerateExports no longer exists.
rpc.exports.exports = function (moduleName, filter) {
  const module = Process.getModuleByName(moduleName);
  const needle = filter ? filter.toLowerCase() : null;
  return module
    .enumerateExports()
    .filter((entry) => !needle || entry.name.toLowerCase().indexOf(needle) !== -1)
    .map((entry) => ({
      type: entry.type,
      name: entry.name,
      address: entry.address.toString(),
    }));
};

/* ------------------------------------------------------------------ scans */

const scans = new Map();
let nextScanId = 1;

function collectRanges(options) {
  if (options.regions && options.regions.length) {
    return options.regions.map((r) => ({ base: ptr(r.base), size: r.size }));
  }
  const protection = options.protection || 'rw-';
  let ranges = Process.enumerateRanges(protection);
  if (!options.includeFileBacked) {
    ranges = ranges.filter((r) => !r.file);
  }
  return ranges.map((r) => ({ base: r.base, size: r.size }));
}

function valueToPattern(type, value) {
  const spec = requireType(type);
  const scratch = Memory.alloc(spec.size);
  spec.write(scratch, value);
  const bytes = new Uint8Array(scratch.readByteArray(spec.size));
  const parts = [];
  for (let i = 0; i < bytes.length; i++) {
    parts.push((bytes[i] < 16 ? '0' : '') + bytes[i].toString(16));
  }
  return parts.join(' ');
}

function readSlot(address, type) {
  return TYPES[type].read(address);
}

function snapshotSlots(ranges, type, alignment, maxBytes) {
  const reader = VIEW_READERS[type];
  if (!reader) {
    throw new Error(
      'unknown-value scans support int8/uint8/int16/uint16/int32/uint32/float/double; ' +
        type + ' is not one of them'
    );
  }

  const size = TYPES[type].size;
  const slots = [];
  let bytesRead = 0;
  let truncated = false;

  outer: for (const range of ranges) {
    let offset = 0;
    while (offset < range.size) {
      const chunk = Math.min(range.size - offset, 1 << 20);
      if (bytesRead + chunk > maxBytes) {
        truncated = true;
        break outer;
      }

      let buffer;
      try {
        buffer = range.base.add(offset).readByteArray(chunk);
      } catch (error) {
        offset += chunk;
        continue;
      }
      if (buffer === null) {
        offset += chunk;
        continue;
      }

      const view = new DataView(buffer);
      for (let inner = 0; inner + size <= chunk; inner += size) {
        const address = range.base.add(offset + inner);
        if (!aligned(address, alignment)) {
          continue;
        }
        slots.push({ address, value: reader(view, inner) });
      }

      bytesRead += chunk;
      offset += chunk;
    }
  }

  return { slots, bytesRead, truncated };
}

function compare(mode, current, previous, options, type) {
  const tolerance = FLOAT_TYPES[type] ? options.tolerance ?? DEFAULT_TOLERANCE : 0;
  const within = (a, b) => Math.abs(a - b) <= tolerance;

  switch (mode) {
    case 'changed':
      return !within(current, previous);
    case 'unchanged':
      return within(current, previous);
    case 'increased':
      return current > previous && !within(current, previous);
    case 'decreased':
      return current < previous && !within(current, previous);
    case 'exact':
      if (options.value === undefined) {
        throw new Error("mode 'exact' requires options.value");
      }
      return within(current, options.value);
    case 'changed_by':
    case 'increased_by':
      return within(current - previous, requireDelta(options));
    case 'decreased_by':
      return within(previous - current, requireDelta(options));
    default:
      throw new Error('unknown refine mode: ' + mode);
  }
}

function requireDelta(options) {
  if (options.delta === undefined) {
    throw new Error("this mode requires options.delta");
  }
  return options.delta;
}

rpc.exports.scanStart = function (type, options) {
  options = options || {};
  const spec = requireType(type);
  const alignment = options.alignment || spec.size;
  const maxCandidates = options.maxCandidates || DEFAULT_MAX_CANDIDATES;
  const ranges = collectRanges(options);
  const rangeBytes = ranges.reduce((total, r) => total + r.size, 0);

  const scanId = nextScanId++;
  const scan = {
    id: scanId,
    type,
    alignment,
    ranges,
    maxCandidates,
    createdAt: Date.now(),
    candidates: [],
    bytesSnapshotted: 0,
    truncated: false,
    exact: false,
  };

  if (options.value !== undefined) {
    scan.exact = true;
    const pattern = valueToPattern(type, options.value);

    for (const range of ranges) {
      let matches;
      try {
        matches = Memory.scanSync(range.base, range.size, pattern);
      } catch (error) {
        continue;
      }
      for (const match of matches) {
        if (!aligned(match.address, alignment)) {
          continue;
        }
        if (scan.candidates.length >= maxCandidates) {
          scan.truncated = true;
          break;
        }
        scan.candidates.push({ address: match.address, value: options.value });
      }
      if (scan.truncated) {
        break;
      }
    }
  } else {
    const snapshot = snapshotSlots(ranges, type, alignment, options.maxBytes || DEFAULT_MAX_BYTES);
    scan.candidates = snapshot.slots;
    scan.bytesSnapshotted = snapshot.bytesRead;
    scan.truncated = snapshot.truncated;

    if (scan.candidates.length > maxCandidates) {
      scan.candidates.length = maxCandidates;
      scan.truncated = true;
    }
  }

  scans.set(scanId, scan);

  return {
    scanId,
    type,
    mode: scan.exact ? 'exact' : 'snapshot',
    value: options.value,
    alignment,
    regionCount: ranges.length,
    rangeBytes,
    candidates: scan.candidates.length,
    bytesSnapshotted: scan.bytesSnapshotted,
    truncated: scan.truncated,
  };
};

rpc.exports.scanRefine = function (scanId, mode, options) {
  options = options || {};
  const scan = scans.get(scanId);
  if (!scan) {
    throw new Error('no such scan: ' + scanId);
  }
  if (scan.exact) {
    throw new Error('scan ' + scanId + ' was an exact scan; re-run scanStart with no value to snapshot');
  }

  const kept = [];
  let unreadable = 0;

  for (const candidate of scan.candidates) {
    let current;
    try {
      current = readSlot(candidate.address, scan.type);
    } catch (error) {
      unreadable++;
      continue;
    }
    if (compare(mode, current, candidate.value, options, scan.type)) {
      kept.push({ address: candidate.address, value: current });
    }
  }

  scan.candidates = kept;
  scan.mode = mode;

  return {
    scanId,
    mode,
    candidates: kept.length,
    unreadable,
    bytesSnapshotted: scan.bytesSnapshotted,
  };
};

rpc.exports.scanResults = function (scanId, offset, limit) {
  const scan = scans.get(scanId);
  if (!scan) {
    throw new Error('no such scan: ' + scanId);
  }

  const start = offset || 0;
  const count = limit || 100;
  const slice = scan.candidates.slice(start, start + count);

  return {
    scanId,
    total: scan.candidates.length,
    offset: start,
    returned: slice.length,
    results: slice.map((candidate) => {
      let value = candidate.value;
      try {
        value = readSlot(candidate.address, scan.type);
      } catch (error) {
        // Report the last known value; the address is simply gone now.
      }
      return { address: candidate.address.toString(), value };
    }),
  };
};

rpc.exports.scanStop = function (scanId) {
  const scan = scans.get(scanId);
  if (!scan) {
    throw new Error('no such scan: ' + scanId);
  }
  scans.delete(scanId);
  return {
    scanId,
    releasedCandidates: scan.candidates.length,
    releasedBytes: scan.bytesSnapshotted,
    activeScans: scans.size,
  };
};

rpc.exports.findPointersTo = function (target, options) {
  options = options || {};
  const needle = ptr(target);
  const ranges = collectRanges(options);
  const maxResults = options.maxCandidates || 10000;
  const found = [];

  for (const range of ranges) {
    let matches;
    try {
      matches = Memory.scanSync(range.base, range.size, needle.toMatchPattern());
    } catch (error) {
      continue;
    }
    for (const match of matches) {
      if (found.length >= maxResults) {
        return { target: needle.toString(), candidates: found.length, truncated: true, results: found };
      }
      found.push(match.address.toString());
    }
  }

  return { target: needle.toString(), candidates: found.length, truncated: false, results: found };
};

/* ------------------------------------------------------------------ hooks */

const traces = new Map();
let nextTraceId = 1;

const NATIVE_TYPES = {
  void: 'void',
  bool: 'bool',
  int8: 'int8',
  uint8: 'uint8',
  int16: 'int16',
  uint16: 'uint16',
  int32: 'int32',
  uint32: 'uint32',
  int64: 'int64',
  uint64: 'uint64',
  long: 'long',
  ulong: 'ulong',
  float: 'float',
  double: 'double',
  pointer: 'pointer',
  char: 'char',
  uchar: 'uchar',
  short: 'short',
  ushort: 'ushort',
  int: 'int',
  uint: 'uint',
};

function parseSignature(signature) {
  const match = /^\s*([A-Za-z0-9_]+)\s*\(([^)]*)\)\s*$/.exec(signature);
  if (!match) {
    throw new Error("signature must look like 'int(int, pointer)', got: " + signature);
  }

  const retType = NATIVE_TYPES[match[1]];
  if (!retType) {
    throw new Error('unsupported return type: ' + match[1]);
  }

  const argTypes = match[2]
    .split(',')
    .map((part) => part.trim())
    // 'void' in the parameter list means "takes no arguments", not "one void
    // argument" - without this, int(void) would read a bogus args[0].
    .filter((part) => part.length > 0 && part !== 'void')
    .map((part) => {
      const mapped = NATIVE_TYPES[part];
      if (!mapped) {
        throw new Error('unsupported argument type: ' + part);
      }
      return mapped;
    });

  return { retType, argTypes };
}

function formatValue(type, raw) {
  switch (type) {
    case 'pointer':
      return raw.toString();
    case 'int64':
    case 'uint64':
      return raw.toString();
    case 'float':
    case 'double':
      return raw;
    case 'bool':
      return raw ? 1 : 0;
    default:
      return typeof raw === 'number' ? raw : raw.toInt32();
  }
}

rpc.exports.hookExport = function (moduleName, symbol, signature, options) {
  options = options || {};
  const address = resolveExport(moduleName || null, symbol);
  const { retType, argTypes } = parseSignature(signature);

  const traceId = nextTraceId++;
  const maxEvents = options.maxEvents || DEFAULT_MAX_EVENTS;

  const trace = {
    id: traceId,
    target: address.toString(),
    module: moduleName || null,
    symbol,
    signature,
    events: [],
    truncated: false,
    finished: false,
    hook: null,
    timer: null,
  };

  function record(event) {
    if (trace.events.length >= maxEvents) {
      trace.truncated = true;
      return;
    }
    trace.events.push(event);
  }

  const pending = new Map();

  trace.hook = Interceptor.attach(address, {
    onEnter(args) {
      const formatted = argTypes.map((type, index) => formatValue(type, args[index]));
      pending.set(this.threadId, {
        at: Date.now(),
        threadId: this.threadId,
        args: formatted,
      });
    },
    onLeave(retval) {
      const entry = pending.get(this.threadId) || { at: Date.now(), threadId: this.threadId, args: [] };
      pending.delete(this.threadId);
      record({
        at: entry.at,
        threadId: entry.threadId,
        args: entry.args,
        retval: retType === 'void' ? null : formatValue(retType, retval),
      });
    },
  });

  const durationMs = options.durationMs || 0;
  if (durationMs > 0) {
    trace.timer = setTimeout(() => {
      stopTrace(traceId);
    }, durationMs);
  }

  traces.set(traceId, trace);

  return { traceId, target: address.toString(), retType, argTypes, durationMs, maxEvents };
};

function stopTrace(traceId) {
  const trace = traces.get(traceId);
  if (!trace || trace.finished) {
    return;
  }
  if (trace.hook) {
    trace.hook.detach();
    trace.hook = null;
  }
  if (trace.timer) {
    clearTimeout(trace.timer);
    trace.timer = null;
  }
  trace.finished = true;
}

rpc.exports.traceEvents = function (traceId, offset, limit) {
  const trace = traces.get(traceId);
  if (!trace) {
    throw new Error('no such trace: ' + traceId);
  }
  const start = offset || 0;
  const count = limit || 100;
  const slice = trace.events.slice(start, start + count);
  return {
    traceId,
    total: trace.events.length,
    truncated: trace.truncated,
    finished: trace.finished,
    offset: start,
    returned: slice.length,
    events: slice,
  };
};

rpc.exports.traceStop = function (traceId) {
  const trace = traces.get(traceId);
  if (!trace) {
    throw new Error('no such trace: ' + traceId);
  }
  stopTrace(traceId);
  const events = trace.events.slice();
  traces.delete(traceId);
  return { traceId, calls: events.length, truncated: trace.truncated, events };
};

/* --------------------------------------------------------------- self test */

// End-to-end check of the primitives, driven from the host with
// rpc.selfTest(). Everything it touches is memory it allocates itself, so it
// is safe to run against a live game: it never reads or writes game state.
rpc.exports.selfTest = function () {
  const checks = [];
  const check = (name, ok, detail) => checks.push({ name, ok: !!ok, detail: String(detail) });

  check(
    'target is 32-bit',
    Process.arch === 'ia32' && Process.pointerSize === 4,
    'arch=' + Process.arch + ' pointerSize=' + Process.pointerSize
  );

  // Typed read/write round trip through the public rpc surface.
  const scratch = Memory.alloc(64);
  const address = scratch.toString();
  const region = [{ base: address, size: 64 }];

  rpc.exports.writeValue(address, 'int32', 1234567);
  const readBack = rpc.exports.readValue(address, 'int32');
  check(
    'writeValue / readValue round trip',
    readBack.value === 1234567,
    'wrote 1234567, read ' + readBack.value
  );

  // Exact scan must find a sentinel we placed ourselves, so the expected
  // answer is known rather than inferred from whatever the game happens to hold.
  const SENTINEL = 0xdeadbeef;
  scratch.writeU32(SENTINEL);
  const exact = rpc.exports.scanStart('uint32', { value: SENTINEL, regions: region });
  const hits = rpc.exports.scanResults(exact.scanId, 0, 32).results;
  const found = hits.some((hit) => ptr(hit.address).equals(scratch));
  check(
    'exact scan finds the sentinel',
    found,
    'candidates=' + exact.candidates + ' found=' + found
  );
  rpc.exports.scanStop(exact.scanId);

  // Snapshot, mutate one slot, then narrow by 'changed'. Exactly one candidate
  // should survive - the slot we touched.
  const snapshot = rpc.exports.scanStart('uint32', { regions: region });
  scratch.writeU32(0x0badf00d);
  const refined = rpc.exports.scanRefine(snapshot.scanId, 'changed');
  check(
    'snapshot + changed refine narrows to one slot',
    refined.candidates === 1,
    'candidates=' + refined.candidates
  );
  rpc.exports.scanStop(snapshot.scanId);

  // Hook a live export and drive it ourselves, so the result does not depend on
  // how often the game happens to call it.
  try {
    const moduleName = 'kernel32.dll';
    const symbol = 'GetTickCount';
    const target = resolveExport(moduleName, symbol);
    const trace = rpc.exports.hookExport(moduleName, symbol, 'uint(void)', { maxEvents: 16 });
    const call = new NativeFunction(target, 'uint', []);
    for (let i = 0; i < 3; i++) {
      call();
    }
    const stopped = rpc.exports.traceStop(trace.traceId);
    check('Interceptor hook fires', stopped.calls >= 3, 'calls=' + stopped.calls);
  } catch (error) {
    check('Interceptor hook fires', false, String(error));
  }

  return {
    ok: checks.every((entry) => entry.ok),
    arch: Process.arch,
    pointerSize: Process.pointerSize,
    checks,
  };
};

/* ------------------------------------------------------------------ ready */

send({ type: 'agent_ready', arch: Process.arch, pointerSize: Process.pointerSize });
