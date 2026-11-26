// Flash a TanenBase .hex onto a XIAO nRF54LM20A from the browser.
//
// Talks CMSIS-DAP v2 (bulk endpoints of the on-board probe, VID 0x2886 /
// PID 0x0068) over WebUSB, then writes the RRAM straight through the AHB-AP:
// RRAM needs no erase and no flash algorithm, only RRAMC.CONFIG.WEN.
//
// Plain ES module, no dependencies. Works with navigator.usb in Chrome/Edge
// and with the `usb` npm package's WebUSB shim in Node (for bench tests).

export const USB_FILTER = { vendorId: 0x2886, productId: 0x0068 };

// --- nRF54LM20A ------------------------------------------------------------
const RRAMC = 0x5004e000;
const RRAMC_COMMITWRITEBUF = RRAMC + 0x008;
const RRAMC_READY = RRAMC + 0x400;
const RRAMC_WRITEBUFEMPTY = RRAMC + 0x418;
const RRAMC_CONFIG = RRAMC + 0x500;
const RRAM_LINE = 16;
const AP_AHB = 0, AP_CTRL = 2;
const CTRLAP_RESET = 0x00, CTRLAP_ERASEALL = 0x04, CTRLAP_ERASEALLSTATUS = 0x08,
      CTRLAP_IDR = 0xfc;

// --- Cortex-M debug --------------------------------------------------------
const DHCSR = 0xe000edf0, DEMCR = 0xe000edfc, AIRCR = 0xe000ed0c;
const DBGKEY = 0xa05f0000;

// --- CMSIS-DAP / ADIv5 -----------------------------------------------------
const DP_ABORT = 0x0, DP_CTRLSTAT = 0x4, DP_SELECT = 0x8, DP_RDBUFF = 0xc;
const AP_CSW = 0x00, AP_TAR = 0x04, AP_DRW = 0x0c;
const REQ_AP = 1, REQ_READ = 2;
const ACK = { 1: 'OK', 2: 'WAIT', 4: 'FAULT', 7: 'NO_ACK' };

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const hex8 = (v) => '0x' + (v >>> 0).toString(16).padStart(8, '0');

// ---------------------------------------------------------------------------
// Intel HEX -> sorted list of contiguous {addr, data: Uint8Array} segments.
export function parseHex(text) {
  const bytes = new Map();
  let upper = 0;
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    if (!line) continue;
    if (line[0] !== ':') throw new Error('not an Intel HEX file');
    const b = line.slice(1).match(/../g).map((h) => parseInt(h, 16));
    if (b.reduce((s, v) => s + v, 0) & 0xff) throw new Error('HEX checksum error: ' + line);
    const len = b[0], off = (b[1] << 8) | b[2], type = b[3], d = b.slice(4, 4 + len);
    if (type === 0) for (let i = 0; i < len; i++) bytes.set(upper + off + i, d[i]);
    else if (type === 1) break;
    else if (type === 2) upper = ((d[0] << 8) | d[1]) << 4;
    else if (type === 4) upper = (((d[0] << 8) | d[1]) << 16) >>> 0;
  }
  const addrs = [...bytes.keys()].sort((a, b) => a - b);
  const segs = [];
  for (const a of addrs) {
    const s = segs[segs.length - 1];
    if (s && a === s.addr + s.bytes.length) s.bytes.push(bytes.get(a));
    else segs.push({ addr: a, bytes: [bytes.get(a)] });
  }
  return segs.map((s) => ({ addr: s.addr, data: Uint8Array.from(s.bytes) }));
}

// Widen every segment to whole 16-byte RRAM lines, padding with 0xff (the
// erased value). The RRAMC only commits complete lines from its write buffer;
// a trailing partial line is what openocd's load_image silently dropped.
export function alignToLines(segs) {
  return segs.map(({ addr, data }) => {
    const start = addr & ~(RRAM_LINE - 1);
    const end = (addr + data.length + RRAM_LINE - 1) & ~(RRAM_LINE - 1);
    const out = new Uint8Array(end - start).fill(0xff);
    out.set(data, addr - start);
    return { addr: start >>> 0, data: out };
  });
}

// ---------------------------------------------------------------------------
export class Dap {
  constructor(device, log = () => {}) {
    this.dev = device;
    this.log = log;
  }

  async open() {
    const d = this.dev;
    if (!d.opened) await d.open();
    if (d.configuration === null) await d.selectConfiguration(1);
    // CMSIS-DAP v2 = vendor class interface with a bulk IN + bulk OUT pair.
    for (const itf of d.configuration.interfaces) {
      const alt = itf.alternates[0];
      if (alt.interfaceClass !== 0xff) continue;
      const out = alt.endpoints.find((e) => e.direction === 'out' && e.type === 'bulk');
      const inn = alt.endpoints.find((e) => e.direction === 'in' && e.type === 'bulk');
      if (out && inn) {
        this.itf = itf.interfaceNumber;
        this.epOut = out.endpointNumber;
        this.epIn = inn.endpointNumber;
        this.pkt = out.packetSize;
        break;
      }
    }
    if (this.itf === undefined) throw new Error('no CMSIS-DAP v2 interface on this device');
    await d.claimInterface(this.itf);
    // Resync: replies to an earlier, dead session may still be queued. Ask
    // twice with different questions so the second answer drains the queue.
    let sz;
    for (let i = 0; i < 8 && !(sz && sz.length === 2); i++) sz = await this.info(0xff);
    if (sz) this.pkt = sz[0] | (sz[1] << 8);
    for (let i = 0; i < 8; i++) { const f = await this.info(0x04); if (f && f.length > 2) break; }
    this.log(`probe: ${this.str(await this.info(0x02)) || '?'} fw ${this.str(await this.info(0x04)) || '?'}, packet ${this.pkt} B`);
  }

  async close() {
    try { await this.cmd([0x03]); } catch {}
    try { await this.dev.releaseInterface(this.itf); } catch {}
    try { await this.dev.close(); } catch {}
  }

  str(b) { return b ? String.fromCharCode(...b.filter((c) => c)) : ''; }

  async cmd(bytes) {
    const buf = new Uint8Array(bytes);
    const r = await this.dev.transferOut(this.epOut, buf);
    if (r.status !== 'ok') throw new Error('USB OUT ' + r.status);
    // A session that died mid-command (tab closed, reload) leaves its reply
    // queued in the probe; skip such stale replies until ours arrives.
    for (let tries = 0; tries < 8; tries++) {
      let timer;
      const res = await Promise.race([
        this.dev.transferIn(this.epIn, Math.max(this.pkt, 64)),
        new Promise((_, rej) => { timer = setTimeout(() => rej(new Error('probe not responding - unplug and replug the board')), 3000); }),
      ]).finally(() => clearTimeout(timer));
      if (res.status !== 'ok') throw new Error('USB IN ' + res.status);
      const v = new Uint8Array(res.data.buffer, res.data.byteOffset, res.data.byteLength);
      if (v[0] === buf[0]) return v;
    }
    throw new Error(`no DAP reply to command 0x${buf[0].toString(16)}`);
  }

  async info(id) {
    const r = await this.cmd([0x00, id]);
    return r[1] ? Array.from(r.slice(2, 2 + r[1])) : null;
  }

  // ---- SWD link -----------------------------------------------------------
  async connect(hz = 4000000) {
    let r = await this.cmd([0x02, 0x01]);             // DAP_Connect SWD
    if (r[1] !== 1) throw new Error('probe refused SWD mode');
    await this.cmd([0x11, ...u32(hz)]);              // DAP_SWJ_Clock
    await this.cmd([0x04, 0x00, ...u16(0xffff), ...u16(0)]); // TransferConfigure
    await this.cmd([0x13, 0x00]);                    // SWD_Configure
    // line reset, JTAG->SWD 0xE79E, line reset, idle
    await this.cmd([0x12, 56, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff]);
    await this.cmd([0x12, 16, 0x9e, 0xe7]);
    await this.cmd([0x12, 56, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff]);
    await this.cmd([0x12, 8, 0x00]);
    const idr = await this.dpRead(0x0);              // DPIDR
    this.log('DPIDR ' + hex8(idr));
    await this.dpWrite(DP_ABORT, 0x1e);              // clear sticky errors
    await this.dpWrite(DP_CTRLSTAT, 0x50000000);     // CSYS/CDBG power-up
    for (let i = 0; ; i++) {
      if (((await this.dpRead(DP_CTRLSTAT)) >>> 28) === 0xf) break;
      if (i > 100) throw new Error('debug power-up timed out');
      await sleep(5);
    }
    this.sel = -1;
    return idr;
  }

  // ---- raw transfers ------------------------------------------------------
  async transfer(ops) {
    // ops: [{req, data?}] -> array of read values
    const p = [0x05, 0x00, ops.length];
    for (const o of ops) { p.push(o.req); if (!(o.req & REQ_READ)) p.push(...u32(o.data)); }
    const r = await this.cmd(p);
    if (r[1] !== ops.length || r[2] !== 1) {
      await this.dpWrite(DP_ABORT, 0x1e).catch(() => {});
      throw new Error(`SWD ${ACK[r[2] & 7] || 'ack ' + r[2]} after ${r[1]}/${ops.length} transfers`);
    }
    const out = [];
    for (let i = 3; i + 4 <= r.length && out.length < ops.filter((o) => o.req & REQ_READ).length; i += 4) out.push(rd32(r, i));
    return out;
  }
  async dpRead(a) { return (await this.transfer([{ req: REQ_READ | a }]))[0]; }
  async dpWrite(a, v) { await this.transfer([{ req: a, data: v }]); }

  async select(ap, reg) {
    const s = ((ap << 24) | (reg & 0xf0)) >>> 0;
    if (s !== this.sel) { await this.dpWrite(DP_SELECT, s); this.sel = s; }
  }
  async apRead(ap, reg) {
    await this.select(ap, reg);
    return (await this.transfer([{ req: REQ_AP | REQ_READ | (reg & 0xc) }]))[0];
  }
  async apWrite(ap, reg, v) {
    await this.select(ap, reg);
    await this.transfer([{ req: REQ_AP | (reg & 0xc), data: v }]);
  }

  // ---- memory through the CPU's AHB-AP -------------------------------------
  async memInit() {
    const csw = await this.apRead(AP_AHB, AP_CSW);
    // Keep prot bits but clear HNONSEC (bit 30): the probe resets it to 1 and
    // non-secure accesses fault on the secure RRAM and SRAM. 32-bit, auto-inc.
    this.csw = ((csw & 0xbf000000) | 0x00000012) >>> 0;
    await this.apWrite(AP_AHB, AP_CSW, this.csw);
  }
  async read32(addr) {
    await this.apWrite(AP_AHB, AP_TAR, addr);
    return this.apRead(AP_AHB, AP_DRW);
  }
  async write32(addr, v) {
    await this.apWrite(AP_AHB, AP_TAR, addr);
    await this.apWrite(AP_AHB, AP_DRW, v);
  }

  // Block transfers: TAR auto-increment wraps at 1 KiB, so split there.
  get blockWords() { return Math.floor((this.pkt - 5) / 4); }

  async readBlock(addr, words) {
    const out = new Uint32Array(words);
    let done = 0;
    await this.select(AP_AHB, AP_DRW);
    while (done < words) {
      const a = addr + done * 4;
      const n = Math.min(words - done, this.blockWords, (0x400 - (a & 0x3ff)) / 4);
      await this.apWrite(AP_AHB, AP_TAR, a);
      const r = await this.cmd([0x06, 0x00, ...u16(n), REQ_AP | REQ_READ | AP_DRW]);
      const got = r[1] | (r[2] << 8);
      if (got !== n || r[3] !== 1) throw new Error(`block read ${hex8(a)}: ${ACK[r[3] & 7] || r[3]}`);
      for (let i = 0; i < n; i++) out[done + i] = rd32(r, 4 + i * 4);
      done += n;
    }
    return out;
  }

  async writeBlock(addr, data /* Uint32Array */, progress) {
    let done = 0;
    while (done < data.length) {
      const a = addr + done * 4;
      const n = Math.min(data.length - done, this.blockWords, (0x400 - (a & 0x3ff)) / 4);
      await this.apWrite(AP_AHB, AP_TAR, a);
      const p = [0x06, 0x00, ...u16(n), REQ_AP | AP_DRW];
      for (let i = 0; i < n; i++) p.push(...u32(data[done + i]));
      const r = await this.cmd(p);
      const got = r[1] | (r[2] << 8);
      if (got !== n || r[3] !== 1) throw new Error(`block write ${hex8(a)}: ${ACK[r[3] & 7] || r[3]}`);
      done += n;
      if (progress) progress(n * 4);
    }
  }
}

// ---------------------------------------------------------------------------
// High-level nRF54LM20A operations on top of an open Dap.
export class Nrf54 {
  constructor(dap, log = () => {}) { this.dap = dap; this.log = log; }

  async attach() {
    const d = this.dap;
    await d.connect();
    const idr = await d.apRead(AP_CTRL, CTRLAP_IDR);
    this.log('CTRL-AP IDR ' + hex8(idr));
    // APPROTECT shows up as the CPU's AHB-AP refusing access. So can a CPU
    // in System OFF, so hard-reset once before calling it locked.
    const probe = async () => { await d.memInit(); await d.read32(DHCSR); };
    this.locked = false;
    try { await probe(); } catch (e) {
      this.log('AHB-AP refused (' + e.message + '), hard reset');
      await d.dpWrite(DP_ABORT, 0x1e).catch(() => {});
      await this.hardReset();
      await sleep(5);
      try { await d.connect(); await probe(); } catch (e2) {
        this.log('still refused: ' + e2.message + ' -> APPROTECT');
        this.locked = true;
        await d.dpWrite(DP_ABORT, 0x1e).catch(() => {});
      }
    }
    return { idr, locked: this.locked };
  }

  async halt() {
    const d = this.dap;
    await d.write32(DHCSR, DBGKEY | 0x3);              // C_DEBUGEN | C_HALT
    for (let i = 0; i < 50; i++) {
      if ((await d.read32(DHCSR)) & (1 << 17)) return; // S_HALT
      await sleep(10);
    }
    throw new Error('core did not halt');
  }

  // Reset into a halt at the reset vector, so firmware cannot sleep or touch
  // RRAM while we write. If the CPU does not answer (System OFF, crash loop),
  // pulse a hard reset through the CTRL-AP, which is always powered, and
  // catch the core while it boots.
  async resetHalt() {
    try { return await this.sysresetHalt(); } catch (e) {
      this.log('halt failed (' + e.message + '), hard reset via CTRL-AP');
    }
    await this.hardReset();
    let err;
    for (let i = 0; i < 20; i++) {
      try { await this.dap.connect(); await this.dap.memInit(); return await this.sysresetHalt(); }
      catch (e) { err = e; await sleep(20); }
    }
    throw err;
  }

  async hardReset() {
    const d = this.dap;
    await d.apWrite(AP_CTRL, CTRLAP_RESET, 2);
    await sleep(10);
    await d.apWrite(AP_CTRL, CTRLAP_RESET, 0);
  }

  async sysresetHalt() {
    const d = this.dap;
    await d.write32(DHCSR, DBGKEY | 0x3);
    await d.write32(DEMCR, (await d.read32(DEMCR)) | 1); // VC_CORERESET
    await d.write32(AIRCR, 0x05fa0004).catch(() => {});  // SYSRESETREQ, link may blip
    await sleep(50);
    await d.connect();
    await d.memInit();
    await this.halt();
    await d.write32(DEMCR, (await d.read32(DEMCR)) & ~1);
  }

  async run() {
    const d = this.dap;
    await d.write32(DHCSR, DBGKEY);                    // drop debug, let it run
    await d.write32(AIRCR, 0x05fa0004).catch(() => {});
  }

  async rramWait() {
    for (let i = 0; i < 1000; i++) {
      if ((await this.dap.read32(RRAMC_READY)) & 1) return;
      await sleep(1);
    }
    throw new Error('RRAMC not ready');
  }

  // segs: output of alignToLines(parseHex(...))
  async program(segs, progress = () => {}) {
    const d = this.dap;
    const total = segs.reduce((s, x) => s + x.data.length, 0);
    let done = 0;
    await this.rramWait();
    await d.write32(RRAMC_CONFIG, 0x101);              // WEN, WRITEBUFSIZE=1 line
    try {
      for (const s of segs) {
        const w = new Uint32Array(s.data.buffer, s.data.byteOffset, s.data.length / 4);
        await d.writeBlock(s.addr, w, (n) => progress((done += n) / total));
      }
      await d.write32(RRAMC_COMMITWRITEBUF, 1);
      for (let i = 0; i < 100 && !((await d.read32(RRAMC_WRITEBUFEMPTY)) & 1); i++) await sleep(1);
      await this.rramWait();
    } finally {
      await d.write32(RRAMC_CONFIG, 0x0);
    }
  }

  // Returns null if identical, else the first mismatching address.
  async verify(segs, progress = () => {}) {
    const total = segs.reduce((s, x) => s + x.data.length, 0);
    let done = 0;
    for (const s of segs) {
      const want = new Uint32Array(s.data.buffer, s.data.byteOffset, s.data.length / 4);
      for (let off = 0; off < want.length; off += 256) {
        const n = Math.min(256, want.length - off);
        const got = await this.dap.readBlock(s.addr + off * 4, n);
        for (let i = 0; i < n; i++) {
          if (got[i] !== want[off + i]) return { addr: s.addr + (off + i) * 4, got: got[i], want: want[off + i] };
        }
        progress((done += n * 4) / total);
      }
    }
    return null;
  }

  // CTRL-AP ERASEALL: wipes all RRAM incl. settings and removes APPROTECT.
  async eraseAll() {
    const d = this.dap;
    await d.apWrite(AP_CTRL, CTRLAP_ERASEALL, 0);
    await d.apWrite(AP_CTRL, CTRLAP_ERASEALL, 1);
    for (let i = 0; i < 300; i++) {
      const st = await d.apRead(AP_CTRL, CTRLAP_ERASEALLSTATUS);
      if (st === 1) break;                             // READYTORESET
      if (st === 3) throw new Error('ERASEALL failed');
      if (i === 299) throw new Error('ERASEALL timed out');
      await sleep(100);
    }
    await d.apWrite(AP_CTRL, CTRLAP_RESET, 2);
    await sleep(10);
    await d.apWrite(AP_CTRL, CTRLAP_RESET, 0);
    await d.apWrite(AP_CTRL, CTRLAP_ERASEALL, 0);
    await sleep(200);
    return this.attach();
  }
}

function u16(v) { return [v & 0xff, (v >> 8) & 0xff]; }
function u32(v) { return [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff]; }
function rd32(b, i) { return (b[i] | (b[i + 1] << 8) | (b[i + 2] << 16) | (b[i + 3] << 24)) >>> 0; }
