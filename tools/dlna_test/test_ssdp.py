#!/usr/bin/env python3
"""SSDP test: M-SEARCH (unicast to 127.0.0.1 and multicast) -> answers."""
import socket, sys, time
fails = 0
def check(ok, what):
    global fails
    print(f"{what:60s} {'OK' if ok else 'FAIL'}"); fails += 0 if ok else 1

def search(st, dest=("127.0.0.1", 1900), wait=1.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.3)
    msg = ("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
           f"MAN: \"ssdp:discover\"\r\nMX: 1\r\nST: {st}\r\n\r\n").encode()
    s.sendto(msg, dest)
    out, t0 = [], time.time()
    while time.time() - t0 < wait:
        try:
            d, _ = s.recvfrom(2048); out.append(d.decode())
        except socket.timeout:
            pass
    s.close()
    return out

a = search("ssdp:all")
check(len(a) == 6, f"ssdp:all -> 6 answers ({len(a)})")
check(all("LOCATION: http://127.0.0.1:49152/dlna/description.xml" in x for x in a), "LOCATION")
check(any("USN: uuid:5c333200-0000-1000-8000-0123456789ab::urn:schemas-upnp-org:device:MediaRenderer:1" in x for x in a), "USN device")
a = search("urn:schemas-upnp-org:device:MediaRenderer:1")
check(len(a) == 1 and "ST: urn:schemas-upnp-org:device:MediaRenderer:1" in a[0], "ST MediaRenderer:1")
a = search("urn:schemas-upnp-org:service:AVTransport:1")
check(len(a) == 1, "ST AVTransport:1")
a = search("urn:schemas-upnp-org:device:MediaServer:1")
check(len(a) == 0, "no answer for MediaServer")
a = search("uuid:5c333200-0000-1000-8000-0123456789ab")
check(len(a) == 1 and "USN: uuid:5c333200-0000-1000-8000-0123456789ab\r\n" in a[0], "ST uuid")
print("FAILED" if fails else "ALL PASSED", f"({fails} failures)")
sys.exit(1 if fails else 0)
