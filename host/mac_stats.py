#!/usr/bin/env python3
"""Sample macOS system stats and push them to the ESP32 mac_monitor over USB serial.

Usage:
    python3 mac_stats.py                    # UDP broadcast on the LAN (default)
    python3 mac_stats.py /dev/cu.usbmodemXXXX [interval]   # USB serial instead

WiFi mode sends each sample line as a UDP datagram to the LAN broadcast
addresses (255.255.255.255 plus the en0 directed broadcast) on port 45678;
the board listens on that port. Board and Mac must be on the same network.

Line protocol (one line per sample, space-separated key=val, -1 = unavailable):
    cpu=37 gpu=22 mem=61 cpufreq=3200 gpufreq=1396 memu=10.2 memt=16 \
    fan=2150 cput=55.2 gput=48.1 pwr=23.4

Slow-moving account data is refreshed on longer cadences and appended to the
same line when known (keys omitted entirely when the source is unavailable):
    kimi5h=42 kimiweek=17 dsbal=15.51 dstok=1234567

Data sources (stdlib only):
  - CPU %   : top -l 2 (second sample)
  - memory  : vm_stat (active + wired + compressor) vs sysctl hw.memsize
  - GPU %/freq, package power, CPU freq: `sudo -n powermetrics --samplers
              cpu_power,gpu_power` (best effort). Without sudo rights these
              fields are sent as -1 and the display shows '-'. To enable:
              sudo visudo ->  <you> ALL=(ALL) NOPASSWD: /usr/bin/powermetrics
              or just run this script with sudo.
              NOTE: recent macOS removed powermetrics' `smc` sampler, so
              temperatures and fan RPM do NOT come from powermetrics.
  - CPU/GPU die temp, fan RPM: AppleSMC via IOKit (ctypes, no sudo) —
              CPU = TCMb, GPU = hottest key of the Tg* group (same keys as
              kimi_monitor's SMC.swift), fans = F0Ac..F{n}Ac averaged.
  - GPU % (fallback), system power (fallback): `ioreg` — IOAccelerator's
              "Device Utilization %" and AppleSmartBattery's SystemPowerIn,
              both readable without sudo.
  - Kimi quota (every 60s): GET {apiBase}/usages with the CLI's own
              credentials from ~/.kimi-code/credentials/kimi-code.json
              (token auto-refreshed via {oauthHost}/api/oauth/token and
              atomically written back, same as kimi_monitor's QuotaMonitor)
  - DeepSeek balance (every 300s): GET api.deepseek.com/user/balance with the
              DEEPSEEK_API_KEY from ~/.dsh/.credentials.yaml (refs: block)
  - DeepSeek today tokens (every 120s): replays ~/.dsh/sessions/*/*/session*.
              jsonl.zstd via an external `zstd` binary (brew install zstd),
              same as kimi_monitor's DshTokenLog
"""

import ctypes
import fcntl
import glob
import json
import os
import re
import socket
import struct
import subprocess
import sys
import time
import urllib.parse
import urllib.request

INTERVAL = 2.0
UDP_STATS_PORT = 45678


def find_port():
    ports = glob.glob("/dev/cu.usbmodem*")
    if not ports:
        sys.exit("no /dev/cu.usbmodem* device found; is the board plugged in?")
    return max(ports, key=os.path.getmtime)


def sample_cpu_percent():
    out = subprocess.run(["top", "-l", "2", "-n", "0", "-s", "1"],
                         capture_output=True, text=True, timeout=15).stdout
    matches = re.findall(r"CPU usage: [\d.]+% user, [\d.]+% sys, ([\d.]+)% idle", out)
    if not matches:
        return -1
    return round(100.0 - float(matches[-1]))


def sample_memory():
    pagesize = int(subprocess.run(["sysctl", "-n", "vm.pagesize"],
                                  capture_output=True, text=True).stdout.strip())
    total = int(subprocess.run(["sysctl", "-n", "hw.memsize"],
                               capture_output=True, text=True).stdout.strip())
    out = subprocess.run(["vm_stat"], capture_output=True, text=True).stdout

    def pages(name):
        m = re.search(name + r":\s+(\d+)\.", out)
        return int(m.group(1)) if m else 0

    used = (pages("Pages active") + pages("Pages wired down")
            + pages("Pages occupied by compressor")) * pagesize
    return used / (1 << 30), total / (1 << 30), round(100.0 * used / total)


def sample_powermetrics():
    """Best-effort privileged sensors; every field defaults to -1."""
    r = dict(gpu=-1, gpufreq=-1, cpufreq=-1, pwr=-1)
    try:
        out = subprocess.run(
            ["sudo", "-n", "powermetrics",
             "--samplers", "cpu_power,gpu_power", "-n", "1", "-i", "1000"],
            capture_output=True, text=True, timeout=20).stdout
    except (subprocess.TimeoutExpired, OSError):
        return r
    if not out:
        return r

    m = re.findall(r"^(?:E|P\d*)-Cluster HW active frequency:\s*(\d+)", out, re.M)
    if m:
        r["cpufreq"] = max(int(x) for x in m)
    m = re.search(r"GPU HW active frequency:\s*(\d+)", out)
    if m:
        r["gpufreq"] = int(m.group(1))
    m = re.search(r"GPU HW active residency:\s*([\d.]+)", out)
    if m:
        r["gpu"] = round(float(m.group(1)))
    m = re.search(r"Combined Power \(CPU \+ GPU \+ ANE\):\s*([\d.]+)\s*mW", out)
    if m:
        r["pwr"] = round(float(m.group(1)) / 1000.0, 1)
    return r


def sample_gpu_ioreg():
    """GPU busy % from IOAccelerator's PerformanceStatistics (no sudo)."""
    try:
        out = subprocess.run(["ioreg", "-r", "-c", "IOAccelerator", "-d", "1"],
                             capture_output=True, text=True, timeout=10).stdout
    except (subprocess.TimeoutExpired, OSError):
        return None
    m = re.search(r'"Device Utilization %"=(\d+)', out)
    return int(m.group(1)) if m else None


def sample_power_ioreg():
    """System power in watts from AppleSmartBattery telemetry (no sudo)."""
    try:
        out = subprocess.run(["ioreg", "-r", "-c", "AppleSmartBattery"],
                             capture_output=True, text=True, timeout=10).stdout
    except (subprocess.TimeoutExpired, OSError):
        return None
    for key in ("SystemPowerIn", "BatteryPower"):
        m = re.search(rf'"{key}"=([\d.]+)', out)
        if m and float(m.group(1)) > 0:
            return round(float(m.group(1)) / 1000.0, 1)  # mW -> W
    return None


# ------------------------------------------------- AppleSMC (no sudo needed)

class _SMCKeyData(ctypes.Structure):
    # Must match SMCKeyData_t: keyInfo at 28, result at 40, data8 at 42,
    # data32 at 44, bytes at 48, total 80 (SMCKit layout, same as
    # kimi_monitor's SMC.swift).
    _fields_ = [
        ("key", ctypes.c_uint32),            # 0
        ("vers", ctypes.c_uint8 * 6),        # 4
        ("pLimit", ctypes.c_uint8 * 16),     # 10
        ("_pad0", ctypes.c_uint8 * 2),       # 26
        ("dataSize", ctypes.c_uint32),       # 28
        ("dataType", ctypes.c_uint32),       # 32
        ("dataAttributes", ctypes.c_uint8),  # 36
        ("_pad1", ctypes.c_uint8 * 3),       # 37
        ("result", ctypes.c_uint8),          # 40
        ("status", ctypes.c_uint8),          # 41
        ("data8", ctypes.c_uint8),           # 42
        ("_pad2", ctypes.c_uint8),           # 43
        ("data32", ctypes.c_uint32),         # 44
        ("bytes", ctypes.c_uint8 * 32),      # 48
    ]


class _SMC:
    _HANDLE_YPC_EVENT = 2
    _READ_KEY = 5
    _GET_KEY_FROM_INDEX = 8
    _GET_KEY_INFO = 9

    def __init__(self):
        iokit = ctypes.cdll.LoadLibrary(
            "/System/Library/Frameworks/IOKit.framework/IOKit")
        self._iokit = iokit
        iokit.IOServiceMatching.restype = ctypes.c_void_p
        matching = iokit.IOServiceMatching(b"AppleSMC")
        service = iokit.IOServiceGetMatchingService(0, ctypes.c_void_p(matching))
        if not service:
            raise RuntimeError("AppleSMC service not found")
        self._conn = ctypes.c_uint32()
        kernel = ctypes.CDLL("/usr/lib/system/libsystem_kernel.dylib")
        task = ctypes.c_uint32.in_dll(kernel, "mach_task_self_")
        kr = iokit.IOServiceOpen(service, task, 0, ctypes.byref(self._conn))
        iokit.IOObjectRelease(service)
        if kr != 0:
            raise RuntimeError(f"IOServiceOpen failed: {kr:#x}")
        # Cache the Tg* group once (GPU die sensors), like kimi_monitor does.
        self._gpu_keys = [k for k in self._all_keys()
                          if k.startswith("Tg")]

    @staticmethod
    def _fourcc(s):
        return struct.unpack(">I", s.encode("latin1"))[0]

    @staticmethod
    def _fourcc_str(v):
        return struct.pack(">I", v).decode("latin1")

    def _call(self, data):
        out = _SMCKeyData()
        out_size = ctypes.c_size_t(ctypes.sizeof(_SMCKeyData))
        kr = self._iokit.IOConnectCallStructMethod(
            self._conn, self._HANDLE_YPC_EVENT,
            ctypes.byref(data), ctypes.sizeof(_SMCKeyData),
            ctypes.byref(out), ctypes.byref(out_size))
        if kr != 0 or out.result != 0:
            return None
        return out

    def read_number(self, name):
        data = _SMCKeyData()
        data.key = self._fourcc(name)
        data.data8 = self._GET_KEY_INFO
        out = self._call(data)
        if out is None:
            return None
        size, type_ = out.dataSize, self._fourcc_str(out.dataType)

        data = _SMCKeyData()
        data.key = self._fourcc(name)
        data.dataSize = size
        data.data8 = self._READ_KEY
        out = self._call(data)
        if out is None:
            return None
        raw = bytes(out.bytes[:size])
        if type_ == "flt " and len(raw) >= 4:
            return struct.unpack("<f", raw[:4])[0]
        if type_ == "sp78" and len(raw) >= 2:
            return struct.unpack(">h", raw[:2])[0] / 256.0
        if type_ == "fpe2" and len(raw) >= 2:
            return struct.unpack(">H", raw[:2])[0] / 4.0
        if type_.startswith(("ui", "si")):
            return int.from_bytes(raw, "big")
        return None

    def _all_keys(self):
        data = _SMCKeyData()
        data.key = self._fourcc("#KEY")
        data.data8 = self._GET_KEY_INFO
        out = self._call(data)
        if out is None:
            return []
        size = out.dataSize
        data = _SMCKeyData()
        data.key = self._fourcc("#KEY")
        data.dataSize = size
        data.data8 = self._READ_KEY
        out = self._call(data)
        if out is None:
            return []
        count = int.from_bytes(bytes(out.bytes[:size]), "big")
        keys = []
        for i in range(count):
            data = _SMCKeyData()
            data.data8 = self._GET_KEY_FROM_INDEX
            data.data32 = i
            out = self._call(data)
            if out is not None:
                keys.append(self._fourcc_str(out.key))
        return keys

    def fan_rpm(self):
        """Average RPM across all fans; None when fanless/unavailable."""
        n = self.read_number("FNum")
        if not n:
            return None
        rpms = [self.read_number(f"F{i}Ac") for i in range(int(n))]
        rpms = [r for r in rpms if r is not None]
        return round(sum(rpms) / len(rpms)) if rpms else None

    def cpu_temp(self):
        return self.read_number("TCMb")

    def gpu_temp(self):
        temps = [self.read_number(k) for k in self._gpu_keys]
        temps = [t for t in temps if t is not None and t > 0]
        return max(temps) if temps else None


_smc = None


def sample_smc():
    """(fan_rpm, cpu_temp_c, gpu_temp_c); Nones when unavailable."""
    global _smc
    if _smc is None:
        try:
            _smc = _SMC()
        except Exception:
            _smc = False
    if not _smc:
        return None, None, None
    try:
        return _smc.fan_rpm(), _smc.cpu_temp(), _smc.gpu_temp()
    except Exception:
        return None, None, None


# ---------------------------------------------------------------- Kimi quota

KIMI_CRED_PATH = os.path.expanduser("~/.kimi-code/credentials/kimi-code.json")
KIMI_REGION_PATH = os.path.expanduser("~/.kimi-code/region")
KIMI_CLIENT_ID = "17e5f671-d194-4dfb-9706-5516cb48c098"


def kimi_hosts():
    try:
        region = open(KIMI_REGION_PATH).read().strip()
    except OSError:
        region = "cn"
    cn = region != "global"
    return (("https://auth.kimi.com", "https://api.kimi.com/coding/v1") if cn
            else ("https://auth.kimi.ai", "https://api.kimi.ai/coding/v1"))


def kimi_access_token():
    """Valid access token, refreshing (and writing back) when expired."""
    try:
        with open(KIMI_CRED_PATH) as f:
            creds = json.load(f)
    except (OSError, ValueError):
        return None
    if creds.get("expires_at", 0) > time.time() + 30:
        return creds["access_token"]
    refresh_token = creds.get("refresh_token")
    if not refresh_token:
        return None
    oauth_host, _ = kimi_hosts()
    body = urllib.parse.urlencode({
        "client_id": KIMI_CLIENT_ID,
        "refresh_token": refresh_token,
        "grant_type": "refresh_token",
    }).encode()
    req = urllib.request.Request(oauth_host + "/api/oauth/token", data=body,
                                 headers={"Content-Type": "application/x-www-form-urlencoded"})
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            refreshed = json.load(resp)
    except (OSError, ValueError):
        return None
    creds["access_token"] = refreshed["access_token"]
    if refreshed.get("refresh_token"):
        creds["refresh_token"] = refreshed["refresh_token"]
    if refreshed.get("expires_in"):
        creds["expires_at"] = time.time() + refreshed["expires_in"]
    tmp = KIMI_CRED_PATH + ".tmp-monitor"
    with open(tmp, "w") as f:
        json.dump(creds, f)
    os.chmod(tmp, 0o600)
    os.rename(tmp, KIMI_CRED_PATH)
    return creds["access_token"]


def quota_pct(quota):
    try:
        limit = float(quota.get("limit") or 0)
        used = float(quota.get("used") or 0)
    except (TypeError, ValueError):
        return None
    if limit <= 0:
        return None
    return max(0, min(100, round(100.0 * used / limit)))


def sample_kimi_quota():
    """(5h-window pct, weekly pct); Nones when unavailable."""
    token = kimi_access_token()
    if not token:
        return None, None
    _, api_base = kimi_hosts()
    req = urllib.request.Request(api_base + "/usages",
                                 headers={"Authorization": "Bearer " + token})
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            data = json.load(resp)
    except (OSError, ValueError):
        return None, None
    weekly = quota_pct(data.get("usage") or {})
    five = None
    limits = data.get("limits") or []
    for entry in limits:
        window = entry.get("window") or {}
        duration, unit = window.get("duration"), window.get("timeUnit")
        if (duration == 300 and unit == "TIME_UNIT_MINUTE") or \
           (duration == 5 and unit == "TIME_UNIT_HOUR"):
            five = quota_pct(entry.get("detail") or {})
            break
    if five is None and limits:
        five = quota_pct(limits[0].get("detail") or {})
    return five, weekly


# ------------------------------------------------------------------ DeepSeek

DSH_CRED_PATH = os.path.expanduser("~/.dsh/.credentials.yaml")
DSH_SESSIONS_ROOT = os.path.expanduser("~/.dsh/sessions")
ZSTD_CANDIDATES = [
    "/opt/homebrew/bin/zstd",
    "/usr/local/bin/zstd",
    "/opt/miniconda3/bin/zstd",
    "/opt/anaconda3/bin/zstd",
    "/usr/bin/zstd",
]


def deepseek_api_key():
    """DEEPSEEK_API_KEY from env or the refs: block of dsh's credential store."""
    if os.environ.get("DEEPSEEK_API_KEY"):
        return os.environ["DEEPSEEK_API_KEY"]
    try:
        text = open(DSH_CRED_PATH).read()
    except OSError:
        return None
    in_refs = False
    for raw in text.split("\n"):
        indented = raw[:1] in (" ", "\t")
        line = raw.strip()
        if not line or line.startswith("#") or ":" not in line:
            continue
        key, _, value = line.partition(":")
        if not indented:
            in_refs = key.strip() == "refs"
            continue
        if not in_refs or key.strip() != "DEEPSEEK_API_KEY":
            continue
        value = value.strip()
        if len(value) >= 2 and value.startswith('"') and value.endswith('"'):
            value = value[1:-1]
        if value.startswith("env:"):
            return os.environ.get(value[4:]) or None
        return value or None
    return None


def sample_deepseek_balance():
    key = deepseek_api_key()
    if not key:
        return None
    req = urllib.request.Request("https://api.deepseek.com/user/balance",
                                 headers={"Authorization": "Bearer " + key,
                                          "Accept": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            data = json.load(resp)
        infos = data.get("balance_infos") or []
        return float(infos[0]["total_balance"]) if infos else None
    except (OSError, ValueError, KeyError, TypeError):
        return None


def find_zstd():
    for candidate in ZSTD_CANDIDATES:
        if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    for directory in os.environ.get("PATH", "").split(":"):
        candidate = os.path.join(directory, "zstd")
        if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    return None


def sample_deepseek_today_tokens():
    """Sum provider-reported tokens from every dsh session log for today."""
    zstd = find_zstd()
    if not zstd:
        return None
    files = []
    try:
        for cwd in os.listdir(DSH_SESSIONS_ROOT):
            cwd_dir = os.path.join(DSH_SESSIONS_ROOT, cwd)
            if not os.path.isdir(cwd_dir):
                continue
            for session in os.listdir(cwd_dir):
                session_dir = os.path.join(cwd_dir, session)
                modern = os.path.join(session_dir, "session.v3.jsonl.zstd")
                legacy = os.path.join(session_dir, "session.jsonl.zstd")
                if os.path.exists(modern):
                    files.append(modern)
                elif os.path.exists(legacy):
                    files.append(legacy)
    except OSError:
        return None
    if not files:
        return None
    try:
        # A truncated trailing frame on the live log is normal; keep what decoded.
        out = subprocess.run([zstd, "-dc", "--quiet"] + sorted(files),
                             capture_output=True, timeout=60).stdout
    except (subprocess.TimeoutExpired, OSError):
        return None
    if not out:
        return None
    lt = time.localtime()
    day_start = time.mktime((lt.tm_year, lt.tm_mon, lt.tm_mday,
                             0, 0, 0, 0, 0, -1)) * 1000
    day_end = day_start + 86_400_000
    total = 0
    marker = b'"assistant/message"'
    for line in out.split(b"\n"):
        if marker not in line:
            continue
        try:
            obj = json.loads(line)
        except ValueError:
            continue
        t = obj.get("time")
        if not isinstance(t, (int, float)) or not (day_start <= t < day_end):
            continue
        usage = (obj.get("data") or {}).get("usage") or {}
        total += (int(usage.get("inputTokens") or 0)
                  + int(usage.get("outputTokens") or 0)
                  + int(usage.get("cacheReadTokens") or 0))
    return total


# ------------------------------------------------------- CLI session status
#
# Same rules as kimi_monitor: Kimi/Claude status files die after 150 s without
# an event/heartbeat; dsh has no heartbeat, so liveness comes from the flock
# dsh holds on session.lock for the whole session lifetime. dsh state merges
# the hook status file (waiting_user) with the projection cache (working when
# a step is open or tool calls are pending). The zstd approval-log probe is
# not ported: dsh permission prompts read as "working" here.

KIMI_STATUS_DIR = os.path.expanduser("~/.kimi-code/status")
CLAUDE_STATUS_DIR = os.path.expanduser("~/.claude/status")
DSH_STATUS_DIR = os.path.expanduser("~/.dsh/status")
DSH_PROJECTIONS_DIR = os.path.expanduser("~/.dsh/storages/session_projcache/sessions")
SESSION_STALE_AFTER = 150

SESSION_NONE = 0
SESSION_IDLE = 1
SESSION_WORKING = 2
SESSION_WAITING = 3

_STATUS_RANK = {"idle": SESSION_IDLE, "working": SESSION_WORKING,
                "waiting_user": SESSION_WAITING}


def _read_status_files(directory):
    """Live hook-driven sessions: {session_id: rank}; dead files are skipped."""
    now = time.time()
    sessions = {}
    try:
        names = os.listdir(directory)
    except OSError:
        return sessions
    for name in names:
        if not name.endswith(".json"):
            continue
        try:
            with open(os.path.join(directory, name)) as f:
                state = json.load(f)
        except (OSError, ValueError):
            continue
        last_seen = max(state.get("heartbeat_at") or 0, state.get("updated_at") or 0)
        if now - last_seen > SESSION_STALE_AFTER:
            continue
        sid = state.get("session_id") or name[:-5]
        sessions[sid] = _STATUS_RANK.get(state.get("status"), SESSION_IDLE)
    return sessions


def _dsh_lock_alive(session_id):
    """True while a dsh process holds flock on the session's lock file."""
    for lock in glob.glob(
            os.path.join(DSH_SESSIONS_ROOT, "*", session_id, "session.lock")):
        try:
            fd = os.open(lock, os.O_RDONLY)
        except OSError:
            continue
        try:
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                return True  # locked by a live dsh process
            fcntl.flock(fd, fcntl.LOCK_UN)
            return False  # lock free: the process is gone
        finally:
            os.close(fd)
    return False


def _dsh_projection_working(session_id):
    path = os.path.join(DSH_PROJECTIONS_DIR, session_id + ".json")
    try:
        with open(path) as f:
            proj = json.load(f)
        stats = (proj.get("record") or {}).get("rows", {}) \
            .get("sessionStats", {}).get("val") or {}
    except (OSError, ValueError, AttributeError):
        return False
    return bool(stats.get("openStep") or stats.get("pendingCalls"))


def _dsh_sessions():
    """Live dsh sessions: {session_id: rank}."""
    sessions = {}
    try:
        names = os.listdir(DSH_STATUS_DIR)
    except OSError:
        names = []
    status = {}
    for name in names:
        if not name.endswith(".json"):
            continue
        try:
            with open(os.path.join(DSH_STATUS_DIR, name)) as f:
                status[name[:-5]] = json.load(f)
        except (OSError, ValueError):
            continue
    now = time.time()
    for sid in set(status) | {n[:-5] for n in
                              (os.listdir(DSH_PROJECTIONS_DIR)
                               if os.path.isdir(DSH_PROJECTIONS_DIR) else [])
                              if n.endswith(".json")}:
        if not _dsh_lock_alive(sid):
            continue
        state = status.get(sid) or {}
        if state.get("status") == "waiting_user":
            sessions[sid] = SESSION_WAITING
        elif _dsh_projection_working(sid):
            sessions[sid] = SESSION_WORKING
        elif (state.get("status") == "working"
              and now - (state.get("updated_at") or 0) <= SESSION_STALE_AFTER):
            sessions[sid] = SESSION_WORKING
        else:
            sessions[sid] = SESSION_IDLE
    return sessions


def sample_sessions():
    """Aggregate CLI session state.

    Returns (state, total, working, waiting): state is one of SESSION_* —
    the most urgent across Kimi/Claude/dsh sessions.
    """
    sessions = {}
    for source in (_read_status_files(KIMI_STATUS_DIR),
                   _read_status_files(CLAUDE_STATUS_DIR),
                   _dsh_sessions()):
        sessions.update(source)
    if not sessions:
        return SESSION_NONE, 0, 0, 0
    working = sum(1 for r in sessions.values() if r == SESSION_WORKING)
    waiting = sum(1 for r in sessions.values() if r == SESSION_WAITING)
    state = (SESSION_WAITING if waiting else
             SESSION_WORKING if working else SESSION_IDLE)
    return state, len(sessions), working, waiting


def udp_broadcast_addrs():
    addrs = {"255.255.255.255"}
    try:
        out = subprocess.run(["ipconfig", "getbroadcast", "en0"],
                             capture_output=True, text=True, timeout=5).stdout.strip()
        if re.fullmatch(r"\d+\.\d+\.\d+\.\d+", out):
            addrs.add(out)
    except (subprocess.TimeoutExpired, OSError):
        pass
    return sorted(addrs)


def main():
    serial_path = None
    if len(sys.argv) > 1 and sys.argv[1] != "udp":
        serial_path = sys.argv[1]
    interval = float(sys.argv[2]) if len(sys.argv) > 2 else INTERVAL

    port = None
    udp_sock = None
    if serial_path:
        print(f"pushing stats over serial {serial_path} every {interval:.0f}s "
              f"(Ctrl-C to stop)")
    else:
        udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        udp_sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        bcasts = udp_broadcast_addrs()
        print(f"broadcasting stats over udp/{UDP_STATS_PORT} to "
              f"{', '.join(bcasts)} every {interval:.0f}s (Ctrl-C to stop)")

    def send(payload):
        nonlocal port
        if udp_sock is not None:
            for addr in udp_broadcast_addrs():
                udp_sock.sendto(payload, (addr, UDP_STATS_PORT))
            return
        try:
            if port is None:
                port = open(serial_path, "wb", buffering=0)
            port.write(payload)
        except (OSError, BrokenPipeError) as e:
            print(f"serial error: {e}; retrying", file=sys.stderr)
            if port is not None:
                try:
                    port.close()
                except OSError:
                    pass
                port = None

    slow = {"kimi5h": None, "kimiweek": None, "dsbal": None, "dstok": None}
    next_quota = next_balance = next_tokens = 0.0
    while True:
        start = time.time()

        cpu = sample_cpu_percent()
        memu, memt, mem = sample_memory()
        pm = sample_powermetrics()
        if pm["gpu"] < 0:
            gpu = sample_gpu_ioreg()
            if gpu is not None:
                pm["gpu"] = gpu
        if pm["pwr"] < 0:
            pwr = sample_power_ioreg()
            if pwr is not None:
                pm["pwr"] = pwr
        fan, cput, gput = sample_smc()
        sess, sessn, sessw, sessp = sample_sessions()

        # Slow account data, each on its own cadence; failures keep the
        # last good value so the display never flickers back to '-'.
        if start >= next_quota:
            five, weekly = sample_kimi_quota()
            if five is not None:
                slow["kimi5h"] = five
            if weekly is not None:
                slow["kimiweek"] = weekly
            next_quota = start + 60
        if start >= next_balance:
            bal = sample_deepseek_balance()
            if bal is not None:
                slow["dsbal"] = bal
            next_balance = start + 300
        if start >= next_tokens:
            tok = sample_deepseek_today_tokens()
            if tok is not None:
                slow["dstok"] = tok
            next_tokens = start + 120

        line = (f"cpu={cpu} gpu={pm['gpu']} mem={mem} "
                f"cpufreq={pm['cpufreq']} gpufreq={pm['gpufreq']} "
                f"memu={memu:.1f} memt={memt:.0f} "
                f"fan={fan if fan is not None else -1} "
                f"cput={cput if cput is not None else -1} "
                f"gput={gput if gput is not None else -1} "
                f"pwr={pm['pwr']}"
                f" sess={sess} sessn={sessn} sessw={sessw} sessp={sessp}")
        if slow["kimi5h"] is not None:
            line += f" kimi5h={slow['kimi5h']}"
        if slow["kimiweek"] is not None:
            line += f" kimiweek={slow['kimiweek']}"
        if slow["dsbal"] is not None:
            line += f" dsbal={slow['dsbal']:.2f}"
        if slow["dstok"] is not None:
            line += f" dstok={slow['dstok']}"
        line += "\n"
        send(line.encode())
        sys.stdout.write(line)
        sys.stdout.flush()

        elapsed = time.time() - start
        time.sleep(max(0.1, interval - elapsed % interval))


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
