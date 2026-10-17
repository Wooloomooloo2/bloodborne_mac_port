#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Co-op addresses (BB_ONLINE=1): the game's own server, the shadNet server and its WebAPI.

From run.py and the launcher of the Windows port of bbport. Messages,
bloodstains and ghosts come from the community game server (The Hunter's Dream by default);
bells and summons go through a shadNet server (shadPS4's public one by default).

`online.py overrides --user DIR` writes DIR/host_overrides.json for this launch (the game's
server -> the community server; summon signs -> the shadNet WebAPI when it has the Bloodborne
summon service) and prints its path for SHADPS4_HTTP_HOST_OVERRIDES_JSON. A player who needs
other addresses puts them in DIR/host_overrides.custom.json, which is then used as it is.

The Hunter's Dream co-op pool (BB_NET_BACKEND=thd, online_pool=huntersdream): the game's servers
and play logs go to the game server over HTTP, and bells and summons through its session service
with the module's own backend (gpu/bbnet/thd). `online.py thd-overrides --user DIR` writes that
launch's host_overrides.json; `online.py signin` links the account with Discord the way bbhost
does (a code to approve on the website) and prints the two bbport.ini lines, or writes them with
--ini FILE.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import socket
import sys
import time
import urllib.error
import urllib.request

COMMUNITY_SERVER = 'https://thehuntersdream.com'
SHADNET_SERVER = 'srv.shadps4.net:31313'
# Bloodborne's own game server, answered by the community server on the same port.
GAME_SERVER = 'https://ss4.scej-network.jp:20443'
# The server's ss.info sends the game to this port for its API (summons among them).
COMMUNITY_API_PORT = 18671


def community_address(value):
    """The game server as https://host (the game picks the ports), The Hunter's Dream by default."""
    value = str(value or '').strip() or COMMUNITY_SERVER
    scheme, _sep, rest = value.rpartition('://')
    host = rest.split('/')[0].split(':')[0]
    return f'{scheme or "https"}://{host}'


def webapi_address(server):
    """The shadNet WebAPI of a server address host:port (the server's default port 31315)."""
    server = str(server or '').strip() or SHADNET_SERVER
    host = server.rsplit(':', 1)[0] if server.count(':') == 1 else server
    return f'http://{host}:31315'


def webapi_setting(webapi, server, community):
    """The WebAPI field, unless it holds the game server's address (a common mix-up): then the
    shadNet server's own WebAPI."""
    webapi = str(webapi or '').strip()
    community_host = community_address(community).split('://', 1)[1]
    if webapi and community_host in webapi:
        webapi = ''
    return webapi or webapi_address(server)


def summon_service(webapi):
    """True when a shadNet WebAPI answers Bloodborne's summon calls: the route exists there
    (an empty request is refused with 400), where other servers answer 404."""
    request = urllib.request.Request(webapi.rstrip('/') + '/summon_messenger/create', data=b'',
                                     method='POST')
    try:
        urllib.request.urlopen(request, timeout=4).close()
        return True
    except urllib.error.HTTPError as error:
        return error.code != 404
    except (OSError, ValueError):
        return False


def write_overrides(user_dir, community, webapi):
    """The host overrides file for this launch; returns its path."""
    user_dir = Path(user_dir)
    user_dir.mkdir(parents=True, exist_ok=True)
    custom = user_dir / 'host_overrides.custom.json'
    if custom.exists():
        print(f'Co-op: addresses from {custom}', file=sys.stderr)
        return custom
    community = community_address(community)
    host = community.split('://', 1)[1]
    overrides = {GAME_SERVER: community}
    if summon_service(webapi):
        overrides.update({f'{scheme}://{host}:{COMMUNITY_API_PORT}/summon_messenger': webapi
                          for scheme in ('https', 'http')})
        print(f'Co-op: summon signs through {webapi}', file=sys.stderr)
    else:
        print(f'Co-op: {webapi} has no summon service; summon signs through {community}',
              file=sys.stderr)
    path = user_dir / 'host_overrides.json'
    path.write_text(json.dumps(overrides, indent=2) + '\n', encoding='utf-8')
    return path


PLAY_LOG_HOSTS = ('bb-playlog-prod.s3.amazonaws.com', 'bb-playlog-test.s3.amazonaws.com')


def write_thd_overrides(user_dir, community):
    """The host overrides file for a launch on The Hunter's Dream: every scej-network.jp host to
    the game server, the play logs to its API port. Returns its path."""
    user_dir = Path(user_dir)
    user_dir.mkdir(parents=True, exist_ok=True)
    base = community_address(community)
    overrides = {'*.scej-network.jp': base}
    overrides.update({host: f'{base}:{COMMUNITY_API_PORT}' for host in PLAY_LOG_HOSTS})
    path = user_dir / 'host_overrides.json'
    path.write_text(json.dumps(overrides, indent=2) + '\n', encoding='utf-8')
    return path


def thd_call(url, body):
    """One call to the account service; the reply as a dict, or ValueError."""
    request = urllib.request.Request(url, data=json.dumps(body).encode(), method='POST',
                                     headers={'Content-Type': 'application/json'})
    try:
        with urllib.request.urlopen(request, timeout=15) as reply:
            text = reply.read().decode('utf-8', 'replace')
    except urllib.error.HTTPError as error:
        text = error.read().decode('utf-8', 'replace')
    except OSError as error:
        raise ValueError(f'{url}: {error}') from None
    try:
        reply = json.loads(text)
    except ValueError:
        reply = None
    if not isinstance(reply, dict) or 'ResKind' not in reply:
        raise ValueError(f'{url} has no account service')
    if reply['ResKind'] != 0:
        raise ValueError(reply.get('Message') or 'refused')
    return reply


def thd_sign_in(community, on_code):
    """Discord sign-in the way bbhost does it: a code to approve on the website (from the
    Windows port's launcher). on_code(page, code) shows it. Returns (online id, token)."""
    auth = community_address(community)
    start = thd_call(auth + '/auth/device/start',
                     {'DeviceName': f'bbport on {platform.node() or "Mac"}'})
    device, user_code = start.get('DeviceCode', ''), start.get('UserCode', '')
    if not device or not user_code:
        raise ValueError('no code in the reply')
    interval = max(1, int(start.get('Interval') or 5))
    deadline = time.monotonic() + int(start.get('ExpiresIn') or 600)
    on_code(start.get('VerificationUri') or auth + '/link', user_code)
    while time.monotonic() < deadline:
        time.sleep(interval)
        poll = thd_call(auth + '/auth/device/poll', {'DeviceCode': device})
        status = poll.get('Status')
        if status == 'approved':
            if not poll.get('OnlineId') or not poll.get('Token'):
                raise ValueError('no token in the reply')
            return poll['OnlineId'], poll['Token']
        if status == 'slow_down':
            interval += 5
        elif status == 'denied':
            raise ValueError('the sign-in was denied')
        elif status == 'expired':
            break
    raise ValueError('the code expired; try again')


def write_ini_account(ini, online_id, token):
    """online_pool, thd_online_id and thd_token in bbport.ini (other lines kept)."""
    ini = Path(ini)
    lines = ini.read_text(encoding='utf-8').splitlines() if ini.exists() else []
    keep = [line for line in lines
            if not line.startswith(('online_pool=', 'thd_online_id=', 'thd_token='))]
    keep += ['online_pool=huntersdream', f'thd_online_id={online_id}', f'thd_token={token}']
    ini.parent.mkdir(parents=True, exist_ok=True)
    ini.write_text('\n'.join(keep) + '\n', encoding='utf-8')


def check(server, webapi, community):
    """(name, address, ok, detail) for the game server, the shadNet server and its WebAPI."""
    results = []
    try:
        with urllib.request.urlopen(community + ':20443/bb-eu/ss.info', timeout=6) as reply:
            reply.read(64)
        results.append(('game', community, True, ''))
    except (OSError, ValueError) as error:
        results.append(('game', community, False, str(error)))
    host, _sep, port = server.rpartition(':')
    try:
        socket.create_connection((host or server, int(port or 31313)), timeout=4).close()
        results.append(('shadnet', server, True, ''))
    except (OSError, ValueError) as error:
        results.append(('shadnet', server, False, str(error)))
    try:
        with urllib.request.urlopen(webapi.rstrip('/') + '/status', timeout=4) as reply:
            ok = b'"ok":true' in reply.read(512).replace(b' ', b'')
        results.append(('webapi', webapi, ok, '' if ok else 'not a shadNet server'))
    except (OSError, ValueError) as error:
        results.append(('webapi', webapi, False, str(error)))
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = parser.add_subparsers(dest='command', required=True)
    overrides = sub.add_parser('overrides')
    overrides.add_argument('--user', required=True)
    thd = sub.add_parser('thd-overrides')
    thd.add_argument('--user', required=True)
    signin = sub.add_parser('signin')
    signin.add_argument('--ini', help='write the account into this bbport.ini')
    sub.add_parser('check')
    a = parser.parse_args()
    server = os.environ.get('BB_SHADNET_SERVER') or SHADNET_SERVER
    community = community_address(os.environ.get('BB_COMMUNITY_SERVER'))
    webapi = webapi_setting(os.environ.get('BB_SHADNET_WEBAPI'), server, community)
    if a.command == 'overrides':
        print(write_overrides(a.user, community, webapi))
    elif a.command == 'thd-overrides':
        print(write_thd_overrides(a.user, community))
    elif a.command == 'signin':
        def on_code(page, code):
            print(f'Open {page} in a browser, sign in with Discord and enter this code: {code}',
                  file=sys.stderr)
            print('Waiting for the approval...', file=sys.stderr)
        try:
            online_id, token = thd_sign_in(community, on_code)
        except ValueError as error:
            sys.exit(f"The Hunter's Dream sign-in failed: {error}")
        if a.ini:
            write_ini_account(a.ini, online_id, token)
            print(f"Signed in as {online_id}; online_pool, thd_online_id and thd_token written to {a.ini}",
                  file=sys.stderr)
        else:
            print(f'online_pool=huntersdream\nthd_online_id={online_id}\nthd_token={token}')
    else:
        for name, address, ok, detail in check(server, webapi, community):
            print(f'{name}: {address}: {"reachable" if ok else "not reachable"} {detail}'.rstrip())


if __name__ == '__main__':
    main()
