#!/usr/bin/env python3
"""Control point test of the StreamCore32 DLNA renderer.

Uses async_upnp_client (the DLNA library of Home Assistant) as an
independent implementation:  pip install async-upnp-client
Start the renderer first:  ./dlna_test 49152
"""
import asyncio
import sys

import aiohttp
from async_upnp_client.aiohttp import AiohttpNotifyServer, AiohttpRequester
from async_upnp_client.client_factory import UpnpFactory
from async_upnp_client.exceptions import UpnpActionResponseError, UpnpError
from async_upnp_client.profiles.dlna import DmrDevice, TransportState

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 49152
URL = f"http://127.0.0.1:{PORT}/dlna/description.xml"

fails = 0


def check(ok, what):
    global fails
    print(f"{what:60s} {'OK' if ok else 'FAIL'}")
    if not ok:
        fails += 1


async def wait_for(cond, timeout=3.0):
    t = 0.0
    while t < timeout:
        if cond():
            return True
        await asyncio.sleep(0.05)
        t += 0.05
    return cond()


async def main():
    requester = AiohttpRequester()
    factory = UpnpFactory(requester, non_strict=False)
    device = await factory.async_create_device(URL)
    check(device.device_type == "urn:schemas-upnp-org:device:MediaRenderer:1", "device type")
    check(device.name == "StreamCore32 Test", "friendly name")
    check(len(device.services) == 3, "three services")
    check(DmrDevice.is_profile_device(device), "recognised as DMR profile")

    notify = AiohttpNotifyServer(requester, source=("127.0.0.1", 0))
    await notify.async_start_server()
    events = []
    dmr = DmrDevice(device, notify.event_handler)
    dmr.on_event = lambda svc, vars: events.append((svc.service_id, [v.name for v in vars]))
    await dmr.async_subscribe_services(auto_resubscribe=False)
    await asyncio.sleep(0.6)
    check(any("AVTransport" in e[0] for e in events), "initial AVTransport event")
    check(any("RenderingControl" in e[0] for e in events), "initial RenderingControl event")
    check(any("ConnectionManager" in e[0] for e in events), "initial ConnectionManager event")
    check(dmr.transport_state == TransportState.NO_MEDIA_PRESENT, "state NO_MEDIA_PRESENT")
    check(abs((dmr.volume_level or 0) - 0.40) < 0.01, "volume from event (40)")

    sink = await dmr.async_get_protocol_info()
    check("audio/flac" in " ".join(sink["sink"]), "sink protocol info has flac")

    meta = await dmr.construct_play_media_metadata(
        media_url="http://192.168.1.10:8200/MediaItems/1.flac",
        media_title="Song & Dance",
        override_mime_type="audio/flac",
        override_upnp_class="object.item.audioItem.musicTrack",
        meta_data={"artist": "The Band", "album": "Best <Of>"},
    )
    await dmr.async_set_transport_uri(
        "http://192.168.1.10:8200/MediaItems/1.flac?a=1&b=2", "Song & Dance", meta)
    await dmr.async_wait_for_can_play()
    check(await wait_for(lambda: dmr.transport_state == TransportState.STOPPED), "state STOPPED after SetAVTransportURI")
    check(dmr.current_track_uri == "http://192.168.1.10:8200/MediaItems/1.flac?a=1&b=2", "track uri with & survives")
    check(dmr.media_title == "Song & Dance", "title from metadata event")
    check(dmr.media_artist == "The Band", "artist")
    check(dmr.media_album_name == "Best <Of>", "album with <>")

    await dmr.async_play()
    check(await wait_for(lambda: dmr.transport_state == TransportState.PLAYING), "PLAYING event")
    await asyncio.sleep(1.2)
    await dmr.async_update()
    check((dmr.media_position or 0) >= 1, f"position advances ({dmr.media_position})")

    await dmr.async_set_volume_level(0.25)
    check(await wait_for(lambda: abs((dmr.volume_level or 0) - 0.25) < 0.01), "volume 25 event")
    await dmr.async_mute_volume(True)
    check(await wait_for(lambda: dmr.is_volume_muted is True), "mute event")
    await dmr.async_mute_volume(False)
    check(await wait_for(lambda: dmr.is_volume_muted is False), "unmute event")

    await dmr.async_pause()
    check(await wait_for(lambda: dmr.transport_state == TransportState.PAUSED_PLAYBACK), "PAUSED event")
    from datetime import timedelta
    await dmr.async_seek_rel_time(timedelta(seconds=65))
    await dmr.async_update()
    check(dmr.media_position == 65, f"seek to 65 s ({dmr.media_position})")

    await dmr.async_set_next_transport_uri("http://192.168.1.10:8200/MediaItems/2.mp3", "Two", None)
    check(await wait_for(lambda: "next" in dmr._current_transport_actions), "next uri via event (Next allowed)")
    await dmr.async_next()
    check(await wait_for(lambda: dmr.current_track_uri == "http://192.168.1.10:8200/MediaItems/2.mp3"),
          "Next switches to next uri")

    # errors
    avt = device.service("urn:schemas-upnp-org:service:AVTransport:1")
    try:
        await avt.action("Play").async_call(InstanceID=5, Speed="1")
        check(False, "invalid InstanceID -> 718")
    except UpnpActionResponseError as e:
        check(e.error_code == 718, f"invalid InstanceID -> 718 ({e.error_code})")
    try:
        await avt.action("Seek").async_call(InstanceID=0, Unit="REL_TIME", Target="x:y")
        check(False, "bad seek target -> 711")
    except UpnpActionResponseError as e:
        check(e.error_code == 711, f"bad seek target -> 711 ({e.error_code})")
    r = await avt.action("GetTransportInfo").async_call(InstanceID=0)
    check(r["CurrentSpeed"] == "1", "GetTransportInfo")
    r = await avt.action("GetCurrentTransportActions").async_call(InstanceID=0)
    check("Play" in r["Actions"], f"transport actions ({r['Actions']})")
    rc = device.service("urn:schemas-upnp-org:service:RenderingControl:1")
    r = await rc.action("GetVolume").async_call(InstanceID=0, Channel="Master")
    check(r["CurrentVolume"] == 25, "GetVolume")

    await dmr.async_stop()
    check(await wait_for(lambda: dmr.transport_state == TransportState.STOPPED), "STOPPED event")

    # raw requests: SOAP without SOAPACTION header, 404, renewal
    async with aiohttp.ClientSession() as s:
        body = ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/">'
                '<s:Body><u:GetVolume xmlns:u="urn:schemas-upnp-org:service:RenderingControl:1">'
                '<InstanceID>0</InstanceID><Channel>Master</Channel></u:GetVolume></s:Body></s:Envelope>')
        async with s.post(f"http://127.0.0.1:{PORT}/dlna/RenderingControl/control", data=body,
                          headers={"Content-Type": 'text/xml; charset="utf-8"'}) as resp:
            t = await resp.text()
            check(resp.status == 200 and "<CurrentVolume>25</CurrentVolume>" in t, "action from body (no SOAPACTION)")
        async with s.get(f"http://127.0.0.1:{PORT}/nothing") as resp:
            check(resp.status == 404, "404 for unknown path")
        async with s.request("SUBSCRIBE", f"http://127.0.0.1:{PORT}/dlna/AVTransport/event",
                             headers={"SID": "uuid:unknown", "TIMEOUT": "Second-300"}) as resp:
            check(resp.status == 412, "renew unknown SID -> 412")

    await dmr.async_unsubscribe_services()
    await notify.async_stop_server()
    print("FAILED" if fails else "ALL PASSED", f"({fails} failures)")
    return fails


sys.exit(1 if asyncio.run(main()) else 0)
