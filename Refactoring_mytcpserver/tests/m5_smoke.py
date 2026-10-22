#!/usr/bin/env python3
import argparse
import json
import secrets
import socket
import struct
from pathlib import Path

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
        return msg_id, seq, json.loads(self.recv_exact(body_len).decode("utf-8"))

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


def register_login(client, name, password="123456"):
    response = client.request(
        1002, {"name": name, "pwd": password, "nick": name}
    )
    assert response[2]["error"] == 0, response
    response = client.request(1004, {"name": name, "pwd": password})
    assert response[2]["error"] == 0, response
    return int(response[2]["uid"])


def login(client, name, password="123456"):
    response = client.request(1004, {"name": name, "pwd": password})
    assert response[2]["error"] == 0, response
    return int(response[2]["uid"])


def setup(metadata_path):
    suffix = secrets.token_hex(3)
    names = {
        "a": "m5a_" + suffix,
        "b": "m5b_" + suffix,
        "c": "m5c_" + suffix,
    }
    content = "m5_message_" + suffix
    a, b, c = Client(), Client(), Client()
    try:
        uid_a = register_login(a, names["a"])
        uid_b = register_login(b, names["b"])
        uid_c = register_login(c, names["c"])

        add = a.request(1009, {"to_uid": uid_b, "msg": "M5 fixture"}, 1010)
        assert add[2]["error"] == 0, add
        apply_id = int(add[2]["apply_id"])
        assert b.recv_frame()[0] == 1011
        accept = b.request(1013, {"apply_id": apply_id, "accept": True}, 1014)
        assert accept[2]["error"] == 0, accept
        assert a.recv_frame()[0] == 1015

        sent = a.request(
            1017,
            {
                "to_uid": uid_b,
                "content": content,
                "client_msg_id": "client_" + suffix,
            },
            1018,
        )
        assert sent[2]["error"] == 0, sent
        msg_id = int(sent[2]["msg_id"])

        push = b.recv_frame()
        assert push[0] == 1019 and push[1] == 0, push
        assert int(push[2]["msg_id"]) == msg_id, push
        assert int(push[2]["from_uid"]) == uid_a, push
        assert push[2]["content"] == content, push

        list_a = a.request(1031, {}, 1035)
        assert list_a[2]["error"] == 0, list_a
        assert any(
            int(item["peer_uid"]) == uid_b
            and item["last_content"] == content
            and int(item["last_msg_id"]) == msg_id
            for item in list_a[2]["conversations"]
        ), list_a

        list_b = b.request(1031, {}, 1035)
        assert any(
            int(item["peer_uid"]) == uid_a
            and item["last_content"] == content
            for item in list_b[2]["conversations"]
        ), list_b

        non_friend = a.request(
            1017,
            {
                "to_uid": uid_c,
                "content": "must fail",
                "client_msg_id": "nonfriend_" + suffix,
            },
            1018,
        )
        assert non_friend[2]["error"] == 12, non_friend

        empty = a.request(
            1017,
            {
                "to_uid": uid_b,
                "content": "",
                "client_msg_id": "empty_" + suffix,
            },
            1018,
        )
        assert empty[2]["error"] == 1, empty

        metadata = {
            "names": names,
            "uids": {"a": uid_a, "b": uid_b, "c": uid_c},
            "content": content,
            "msg_id": msg_id,
        }
        Path(metadata_path).write_text(
            json.dumps(metadata, ensure_ascii=False, indent=2),
            encoding="utf-8",
        )
        print(json.dumps(metadata, ensure_ascii=False))
        print("M5_SETUP=PASS")
    finally:
        a.close()
        b.close()
        c.close()


def reload_check(metadata_path):
    metadata = json.loads(Path(metadata_path).read_text(encoding="utf-8"))
    a, b = Client(), Client()
    try:
        uid_a = login(a, metadata["names"]["a"])
        uid_b = login(b, metadata["names"]["b"])

        list_a = a.request(1031, {}, 1035)
        assert any(
            int(item["peer_uid"]) == uid_b
            and item["last_content"] == metadata["content"]
            and int(item["last_msg_id"]) == metadata["msg_id"]
            for item in list_a[2]["conversations"]
        ), list_a

        list_b = b.request(1031, {}, 1035)
        assert any(
            int(item["peer_uid"]) == uid_a
            and item["last_content"] == metadata["content"]
            for item in list_b[2]["conversations"]
        ), list_b

        second_content = metadata["content"] + "_after_restart"
        sent = a.request(
            1017,
            {
                "to_uid": uid_b,
                "content": second_content,
                "client_msg_id": "after_restart",
            },
            1018,
        )
        assert sent[2]["error"] == 0, sent
        push = b.recv_frame()
        assert push[0] == 1019 and push[2]["content"] == second_content, push
        print("M5_RELOAD_PASS")
    finally:
        a.close()
        b.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("phase", choices=["setup", "reload"])
    parser.add_argument("--metadata", default=".m5-last-run.json")
    args = parser.parse_args()
    if args.phase == "setup":
        setup(args.metadata)
    else:
        reload_check(args.metadata)


if __name__ == "__main__":
    main()
