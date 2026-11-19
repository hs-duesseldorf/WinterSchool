"""Runs the engine's web server (serve.py) so a phone's page log reaches it through `tailscale serve`.

serve.py --game-run takes a page's console lines (POST __ue/log, sent by a page opened with ?ue-session=1) only from a
browser on this computer that addresses the server as localhost. `tailscale serve` forwards from 127.0.0.1 but keeps the
tailnet name as the Host, so the phone's posts are refused. Here a log post from loopback naming an --allow-host name is
handed to serve.py as if addressed to localhost. Everything else is unchanged.
Usage: serve_phone_log.py <path to serve.py> <serve.py arguments...>
"""
import importlib.util
import sys

spec = importlib.util.spec_from_file_location("ue_serve", sys.argv[1])
serve = importlib.util.module_from_spec(spec)
spec.loader.exec_module(serve)

original_post = serve.Handler.do_POST


def do_POST(self):
    host = (self.headers.get("Host") or "").split(":")[0].lower().rstrip(".")
    if (self.client_address[0] in ("127.0.0.1", "::1") and host in serve.Handler.allow_hosts
            and self.path.split("?")[0].endswith("/__ue/log")):
        local = f"127.0.0.1:{self.server.server_address[1]}"
        del self.headers["Host"], self.headers["Origin"]
        self.headers["Host"] = local
    original_post(self)


serve.Handler.do_POST = do_POST
sys.exit(serve.main(sys.argv[2:]))
