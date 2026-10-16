#!/bin/bash
#
# M1 服务端验收脚本：真实 socket 上的拆包验证
#
# ./test_codec 只能证明 Codec 自己自洽；这个脚本走真实 socket，证明
# 「内核把一帧切成 N 段送来，服务端还能拼回来」—— 这才是 M1 的验收标准。
#
# 用法：bash tests/m1_smoke.sh [port]
#
set -u

PORT="${1:-18888}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/protocol_echo_server"
LOG=/tmp/m1_protocol_echo.log

if [ ! -x "$BIN" ]; then
    echo "找不到 $BIN"
    echo "先构建：cmake -S \"$ROOT\" -B \"$ROOT/build\" && cmake --build \"$ROOT/build\""
    exit 1
fi

"$BIN" "$PORT" > "$LOG" 2>&1 &
SRV_PID=$!
trap 'kill $SRV_PID 2>/dev/null' EXIT
sleep 0.7

if ! kill -0 $SRV_PID 2>/dev/null; then
    echo "服务端没起来，日志："
    cat "$LOG"
    exit 1
fi

echo "protocol_echo_server 监听 127.0.0.1:$PORT (pid $SRV_PID)"
echo

M1_PORT="$PORT" python3 - <<'PY'
import os
import socket
import sys
import time

PORT = int(os.environ["M1_PORT"])
HDR = __import__("struct").Struct("<HII")       # msgid / bodyLen / seq，全部小端

results = []

def check(name, ok, detail=""):
    results.append(ok)
    print(("  [OK]   " if ok else "  [FAIL] ") + name + ("" if ok else "   " + detail))

def conn():
    return socket.create_connection(("127.0.0.1", PORT), timeout=5)

def recv_exact(s, n):
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            break
        buf += chunk
    return buf

def pack(msgid, seq, body=b""):
    return HDR.pack(msgid, len(body), seq) + body

# 1. 整帧往返：12 字节金标准，两端一致性就看这一条
s = conn()
s.sendall(pack(1004, 1, b"{}"))
got = recv_exact(s, 12)
check("1. 整帧往返 = 12 字节金标准", got.hex() == "ec0302000000010000007b7d", got.hex())
s.close()

# 2. 逐字节发送同一帧：这才真正压到 tryDecode 的半包分支
s = conn()
frame = pack(1002, 42, b'{"name":"bob"}')
for b in frame:
    s.send(bytes([b]))
    time.sleep(0.005)
got = recv_exact(s, len(frame))
check("2. 逐字节发送仍能拼回一帧", got == frame, got.hex())
s.close()

# 3. 粘包：3 帧一次性写完（含空正文帧）
s = conn()
stream = pack(1004, 1, b'{"a":1}') + pack(1005, 2, b'{"b":2}') + pack(1023, 3)
s.sendall(stream)
got = recv_exact(s, len(stream))
check("3. 粘包 3 帧一次收全（含空正文）", got == stream, got.hex())
s.close()

# 4. 100KB 大包，分 3 段送
body = bytes((65 + i % 26) for i in range(100 * 1024))
data = pack(1017, 99, body)
step = len(data) // 3
s = conn()
s.sendall(data[:step])
time.sleep(0.02)
s.sendall(data[step:2 * step])
time.sleep(0.02)
s.sendall(data[2 * step:])
got = recv_exact(s, len(data))
check("4. 100KB 大包分 3 段送回完整", got == data, "len=%d" % len(got))
s.close()

# 5. 非法帧头：bodyLen 越界，服务端必须断开而不是傻等
s = conn()
s.sendall(HDR.pack(1004, 0x7FFFFFFF, 0))
time.sleep(0.3)
try:
    tail = s.recv(64)
except socket.timeout:
    tail = b"<timeout>"
check("5. 非法帧头 -> 服务端断开连接", tail == b"", repr(tail))
s.close()

print()
print("  %d 项检查，%d 项失败" % (len(results), results.count(False)))
sys.exit(0 if all(results) else 1)
PY

RC=$?

echo
echo "--- 服务端日志 ---"
cat "$LOG"

if grep -q 'bad frame header' "$LOG"; then
    echo "  [OK]   日志里有 bad frame header（R9：每条消息都留痕）"
else
    echo "  [FAIL] 日志里没有 bad frame header"
    RC=1
fi

exit $RC
