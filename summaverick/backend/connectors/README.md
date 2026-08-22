# Connectors

A connector is the boundary between Summaverick and a support channel.

## What ships here

- `base.py` — the abstract `BaseConnector` interface.
- `mock.py` — `MockConnector`, backed by the in-process mock support bot. This
  is the **default** connector for demos and tests. It never touches a real
  service or account.

## What deliberately does NOT ship here

Live connectors that log into and operate **real** Zomato / Swiggy / Amazon /
subscription accounts on a user's behalf are **not** implemented in this
repository, and neither are the anti-bot-evasion utilities the original
blueprint listed (browser-fingerprint rotation, residential-proxy rotation,
credential scraping). Those components exist specifically to circumvent a
platform's access controls and bot-detection, which is not something this
project builds.

If you extend Summaverick with a real-platform connector, it must:

1. Use **authorised, authenticated** sessions that the user has explicitly
   consented to, per the platform's Terms of Service and any published API.
2. Prefer official support/consumer APIs over scraping wherever they exist.
3. Not attempt to evade rate limits, bot detection, or access controls.
4. Keep a human in the loop for account linking and for any action the user
   has not pre-approved via their autonomy settings.
5. Redact PII before logging (see `guardrails/filter.py`).

Building an integration on those terms is a legitimate consumer-advocacy tool.
Building one that hides from the platform is not — keep that line bright.
