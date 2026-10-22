#!/usr/bin/env python3
"""M4 server acceptance test.

Uses the real TCP protocol against chatserver and intentionally leaves the
generated test accounts in the database so cleanup is not performed implicitly.
"""

import json
import secrets
import socket
import struct
import time

HOST = "192.168.211.128"
PORT = 8888
HEADER = struct.Struct("<HII")


class Client:
    def __init__(self):
        self.sock = socket.create_connection((HOST, PORT), timeout=5)
        self.sock.settimeout(5)
        self.seq = 0

    def request(self, msg_id, body, response_msg_id=None):
        expected = response_msg_id if response_msg_id is not None else msg_id
        self.seq += 1
        payload = json.dumps(body, separators=(",", ":")).encode("utf-8")
        self.sock.sendall(HEADER.pack(msg_id, len(payload), self.seq) + payload)

        while True:
            frame = self.recv_frame()
            if frame[1] == self.seq:
                assert frame[0] == expected, frame
                return frame

    def recv_frame(self):
        header = self.recv_exact(HEADER.size)
        msg_id, body_len, seq = HEADER.unpack(header)
        body = json.loads(self.recv_exact(body_len).decode("utf-8"))
        return msg_id, seq, body

    def recv_exact(self, size):
        data = b""
        while len(data) < size:
            chunk = self.sock.recv(size - len(data))
            if not chunk:
                raise ConnectionError("connection closed")
            data += chunk
        return data

    def close(self):
        self.sock.close()


def register_login(client, name):
    response = client.request(
        1002, {"name": name, "pwd": "123456", "nick": name}
    )
    assert response[2]["error"] == 0, response
    response = client.request(1004, {"name": name, "pwd": "123456"})
    assert response[2]["error"] == 0, response
    return int(response[2]["uid"])


def main():
    suffix = secrets.token_hex(3)
    names = ["m4acc_a_" + suffix, "m4acc_b_" + suffix, "m4acc_c_" + suffix]
    a, b, c = Client(), Client(), Client()

    try:
        uid_a = register_login(a, names[0])
        uid_b = register_login(b, names[1])
        uid_c = register_login(c, names[2])
        print(f"accounts: {names[0]}={uid_a}, {names[1]}={uid_b}, {names[2]}={uid_c}")

        search = a.request(1007, {"keyword": names[1]}, 1008)
        assert search[2]["error"] == 0, search
        users = search[2]["users"]
        assert any(int(item["uid"]) == uid_b for item in users), users
        assert all("password" not in item for item in users), users
        print("[OK] 1007/1008 search returns public fields")

        self_add = a.request(1009, {"to_uid": uid_a, "msg": "self"}, 1010)
        assert self_add[2]["error"] == 8, self_add
        print("[OK] cannot add self")

        add_ab = a.request(1009, {"to_uid": uid_b, "msg": "hello B"}, 1010)
        assert add_ab[2]["error"] == 0, add_ab
        apply_ab = int(add_ab[2]["apply_id"])
        push = b.recv_frame()
        assert push[0] == 1011 and push[1] == 0, push
        assert int(push[2]["apply_id"]) == apply_ab, push
        print("[OK] 1009/1010/1011 friend request delivered")

        unauthorized = c.request(
            1013, {"apply_id": apply_ab, "accept": True}, 1014
        )
        assert unauthorized[2]["error"] == 11, unauthorized
        print("[OK] unrelated user cannot process the apply")

        accepted = b.request(
            1013, {"apply_id": apply_ab, "accept": True}, 1014
        )
        assert accepted[2]["error"] == 0, accepted
        handled = a.recv_frame()
        assert handled[0] == 1015 and handled[1] == 0, handled
        assert handled[2]["accept"] is True, handled
        print("[OK] 1013 accept -> 1014/1015")

        for client, expected_uid, label in (
            (a, uid_b, "A"),
            (b, uid_a, "B"),
        ):
            friends = client.request(1030, {}, 1034)
            assert friends[2]["error"] == 0, friends
            assert any(int(item["uid"]) == expected_uid for item in friends[2]["friends"]), friends
            print(f"[OK] {label} friend list contains the other user")

        duplicate = a.request(1009, {"to_uid": uid_b, "msg": "again"}, 1010)
        assert duplicate[2]["error"] == 9, duplicate
        print("[OK] duplicate friend request rejected")

        add_ac = a.request(1009, {"to_uid": uid_c, "msg": "hello C"}, 1010)
        assert add_ac[2]["error"] == 0, add_ac
        apply_ac = int(add_ac[2]["apply_id"])
        push_c = c.recv_frame()
        assert push_c[0] == 1011 and int(push_c[2]["apply_id"]) == apply_ac, push_c

        rejected = c.request(
            1013, {"apply_id": apply_ac, "accept": False}, 1014
        )
        assert rejected[2]["error"] == 0, rejected
        handled_c = a.recv_frame()
        assert handled_c[0] == 1015 and handled_c[2]["accept"] is False, handled_c

        friends_c = c.request(1030, {}, 1034)
        assert all(int(item["uid"]) != uid_a for item in friends_c[2]["friends"]), friends_c
        print("[OK] reject path does not create friendship")

        print("M4_SERVER_ACCEPTANCE=PASS")
        print("TEST_ACCOUNTS=" + ",".join(names))
    finally:
        a.close()
        b.close()
        c.close()


if __name__ == "__main__":
    main()
