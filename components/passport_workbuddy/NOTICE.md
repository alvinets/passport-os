# WorkBuddy adapter provenance

The device adapter, relay and device protocol in this directory and
`tools/workbuddy_relay.py` are Passport OS project code (MIT), written for this
port. They replace the Muse pairing, Noise session and chat protocol.

The HTTP CONNECT transport under `components/passport_muse/` is retained from
the community Muse port solely as a generic HTTPS client. Its provenance and
Apache-2.0 terms remain in `components/passport_muse/NOTICE.md`.

WorkBuddy Open API and ACP endpoint names and scope semantics follow the public
documentation at https://open.workbuddy.cn/docs/openapi and
https://open.workbuddy.cn/docs/third-party-app. No WorkBuddy credentials or
tokens are compiled into this repository.
