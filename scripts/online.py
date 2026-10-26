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
and play logs go to the game server over HTTPS, and bells and summons through its session service
with the module's own backend (gpu/bbnet/thd). `online.py thd-overrides --user DIR` writes that
launch's host_overrides.json; `online.py signin` links the account with Discord the way bbhost
does (a code to approve on the website) and prints the two bbport.ini lines, or writes them with
--ini FILE.

The community server must be HTTPS (its account token and the game's traffic go there); plain
HTTP is accepted only for a server on this Mac (localhost, 127.0.0.1). `online.py community`
prints the address run.sh uses, or fails with the reason.
"""
import argparse
import getpass
import json
import os
from pathlib import Path
import re
import socket
import stat
import subprocess
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


LOCAL_HOSTS = ('localhost', '127.0.0.1')
# What The Hunter's Dream accepts as an online id (bbhost's rule).
ONLINE_ID = re.compile(r'[A-Za-z0-9_-]{3,16}')


def community_address(value):
    """The game server as https://host (the game picks the ports), The Hunter's Dream by default.
    ValueError for another scheme, or http:// to a host that is not this Mac."""
    value = str(value or '').strip() or COMMUNITY_SERVER
    scheme, _sep, rest = value.rpartition('://')
    host = rest.split('/')[0].split(':')[0]
    scheme = (scheme or 'https').lower()
    if not host:
        raise ValueError(f'no host in the community server address {value!r}')
    if scheme != 'https' and not (scheme == 'http' and host.lower() in LOCAL_HOSTS):
        raise ValueError(f'the community server must be https:// (got {value!r}; plain http only '
                         'for localhost)')
    return f'{scheme}://{host}'


def on_community_host(url, community):
    """True when url is the community server's own address (its scheme and host, or a host
    under it): the only page the sign-in may send the player to."""
    scheme, _sep, rest = str(url or '').partition('://')
    host = rest.split('/')[0].split(':')[0].lower()
    base_scheme, base_host = community.split('://', 1)
    return (scheme.lower() == base_scheme and bool(host)
            and (host == base_host.lower() or host.endswith('.' + base_host.lower())))


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
                     {'DeviceName': 'bbport on macOS'})  # no hostname: it names the Mac on the account page
    device, user_code = start.get('DeviceCode', ''), start.get('UserCode', '')
    if not device or not user_code:
        raise ValueError('no code in the reply')
    # The server's numbers within reason: 1-60 s between polls, at most 30 minutes.
    interval = min(60, max(1, int(start.get('Interval') or 5)))
    deadline = time.monotonic() + min(1800, max(0, int(start.get('ExpiresIn') or 600)))
    page = start.get('VerificationUri')
    on_code(page if on_community_host(page, auth) else auth + '/link', user_code)
    while time.monotonic() < deadline:
        time.sleep(interval)
        poll = thd_call(auth + '/auth/device/poll', {'DeviceCode': device})
        status = poll.get('Status')
        if status == 'approved':
            if not poll.get('OnlineId') or not poll.get('Token'):
                raise ValueError('no token in the reply')
            if not ONLINE_ID.fullmatch(str(poll['OnlineId'])):
                raise ValueError('the reply has an invalid online id')
            if any(ord(c) < 0x21 for c in str(poll['Token'])):
                raise ValueError('the reply has an invalid token')
            return poll['OnlineId'], poll['Token']
        if status == 'slow_down':
            interval = min(60, interval + 5)
        elif status == 'denied':
            raise ValueError('the sign-in was denied')
        elif status == 'expired':
            break
    raise ValueError('the code expired; try again')


SECRET_KEYS = ('shadnet_password', 'thd_token')


def write_ini_keys(ini, values):
    """Replaces these keys in bbport.ini (other lines kept). A file that holds a secret in clear
    is made readable by its owner only before the secret is written; one that says `keychain`
    gets no secret written."""
    ini = Path(ini)
    lines = ini.read_text(encoding='utf-8').splitlines() if ini.exists() else []
    keep = [line for line in lines if not line.startswith(tuple(f'{k}=' for k in values))]
    keep += [f'{k}={v}' for k, v in values.items()]
    ini.parent.mkdir(parents=True, exist_ok=True)
    secret = any(line.startswith(tuple(f'{k}=' for k in SECRET_KEYS)) and not line.endswith('=keychain')
                 for line in keep)
    owner_only = stat.S_IRUSR | stat.S_IWUSR
    if secret and ini.exists():
        ini.chmod(owner_only)
    fd = os.open(ini, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, owner_only if secret else 0o644)
    with os.fdopen(fd, 'w', encoding='utf-8') as file:
        file.write('\n'.join(keep) + '\n')


def write_ini_account(ini, online_id, token):
    """online_pool, thd_online_id and thd_token in bbport.ini (`keychain` as the token when it is
    stored there)."""
    write_ini_keys(ini, {'online_pool': 'huntersdream', 'thd_online_id': online_id, 'thd_token': token})


def security_quote(text):
    """One argument of a `security -i` command line: in double quotes, \\ and \" escaped."""
    if any(ord(c) < 0x20 for c in text):
        raise ValueError('a keychain value cannot hold control characters or line breaks')
    return '"' + text.replace('\\', '\\\\').replace('"', '\\"') + '"'


def keychain_store(service, account, secret):
    """The login keychain item launch_settings.sh reads (`security find-generic-password -w`).
    The command goes to `security -i` on its standard input, so the secret is never an argument
    other processes can see; `security` made the item, so it reads it back without a prompt
    (as can anything else that runs /usr/bin/security as this user). `security -i` does not
    report a failed command in its status: the item is read back to check it."""
    command = ' '.join(['add-generic-password', '-U', '-s', security_quote(service),
                        '-a', security_quote(account), '-l', security_quote(f'{service} ({account})'),
                        '-w', security_quote(secret)])
    subprocess.run(['security', '-i'], input=command + '\n', text=True, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    stored = subprocess.run(['security', 'find-generic-password', '-s', service, '-a', account, '-w'],
                            capture_output=True, text=True)
    # (`-w` prints a value that is not plain ASCII in hex.)
    if stored.returncode != 0 or stored.stdout.rstrip('\n') not in (secret, secret.encode().hex()):
        raise ValueError(f'the keychain did not take the item {service} ({account})')


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
    signin.add_argument('--keychain', action='store_true',
                        help='keep the token in the login keychain (bbport.ini then says thd_token=keychain)')
    keychain = sub.add_parser('keychain', help='store a secret in the login keychain instead of bbport.ini')
    keychain.add_argument('what', choices=['shadnet'], help='shadnet: the shadNet account password')
    keychain.add_argument('--npid', required=True, help='the shadNet account name')
    keychain.add_argument('--ini', help='also write shadnet_npid and shadnet_password=keychain here')
    sub.add_parser('check')
    sub.add_parser('community', help='print the community server address (https://host)')
    a = parser.parse_args()
    try:
        run(a)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        sys.exit(f'online.py {a.command}: {error}')


def run(a):
    server = os.environ.get('BB_SHADNET_SERVER') or SHADNET_SERVER
    community = community_address(os.environ.get('BB_COMMUNITY_SERVER'))
    webapi = webapi_setting(os.environ.get('BB_SHADNET_WEBAPI'), server, community)
    if a.command == 'community':
        print(community)
    elif a.command == 'overrides':
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
        if a.keychain:
            keychain_store('bbport-huntersdream', online_id, token)
            token = 'keychain'
            print(f"Token stored in the login keychain (bbport-huntersdream, {online_id})", file=sys.stderr)
        if a.ini:
            write_ini_account(a.ini, online_id, token)
            print(f"Signed in as {online_id}; online_pool, thd_online_id and thd_token written to {a.ini}",
                  file=sys.stderr)
        else:
            print(f'online_pool=huntersdream\nthd_online_id={online_id}\nthd_token={token}')
    elif a.command == 'keychain':
        secret = getpass.getpass(f'shadNet password for {a.npid}: ')
        if not secret:
            sys.exit('no password entered')
        keychain_store('bbport-shadnet', a.npid, secret)
        print(f'Password stored in the login keychain (bbport-shadnet, {a.npid})', file=sys.stderr)
        if a.ini:
            write_ini_keys(a.ini, {'shadnet_npid': a.npid, 'shadnet_password': 'keychain'})
            print(f'shadnet_npid and shadnet_password=keychain written to {a.ini}', file=sys.stderr)
    else:
        for name, address, ok, detail in check(server, webapi, community):
            print(f'{name}: {address}: {"reachable" if ok else "not reachable"} {detail}'.rstrip())


if __name__ == '__main__':
    main()
