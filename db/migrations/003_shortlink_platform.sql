-- Short links may target sing-box configurations in addition to Clash.
--
-- sing-box output is platform specific (macos / windows / linux / android /
-- ios / openwrt), so the chosen platform is stored alongside the target and
-- reused when the short link is refreshed. Existing rows keep an empty
-- platform and are treated as Clash.

ALTER TABLE short_links ADD COLUMN IF NOT EXISTS platform TEXT NOT NULL DEFAULT '';
