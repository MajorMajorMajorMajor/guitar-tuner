# Appstore listing

Everything the Pebble appstore needs for Guitar Tuner. The images are rebuilt
by `tools/store_assets.py` from emulator screenshots.

| Field | Value |
| --- | --- |
| App name | Guitar Tuner |
| Type | Watchapp |
| Category | Tools & Utilities (`tools`) |
| Description | [`description.txt`](description.txt) (plain text) |
| Release notes | [`release_notes.txt`](release_notes.txt) |
| Source code URL | https://github.com/MajorMajorMajorMajor/guitar-tuner |
| Small icon (80×80) | [`icon_small.png`](icon_small.png) |
| Large icon (144×144) | [`icon_large.png`](icon_large.png) |
| Screenshots | [`screenshots/`](screenshots): five per watch, the first an animated GIF |
| Banners (720×320) | [`banner_emery.png`](banner_emery.png), [`banner_gabbro.png`](banner_gabbro.png) |
| Platforms | emery (Pebble Time 2), gabbro (Pebble Round 2), from the .pbw |

The version (`package.json`) must be new for every release, and the UUID must
never change: the appstore uses it to match releases to the listing.

## Publish the first release

From the repository root, signed in with the account that should own the
listing:

```sh
pebble login
pebble publish --non-interactive \
  --name "Guitar Tuner" \
  --category tools \
  --description "$(cat store/description.txt)" \
  --source https://github.com/MajorMajorMajorMajor/guitar-tuner \
  --icon-small store/icon_small.png \
  --icon-large store/icon_large.png \
  --release-notes "$(cat store/release_notes.txt)" \
  --screenshots store/screenshots/emery_* store/screenshots/gabbro_*
```

This builds the app, creates the listing as Listed, and publishes the release.
It prints the listing's link (`https://apps.repebble.com/<id>`).

`pebble publish` has no option for banners. To add them, open the app in the
[Developer Dashboard](https://developer.repebble.com), click Edit, and upload
`banner_emery.png` for emery and `banner_gabbro.png` for gabbro. The
dashboard can also do the whole submission instead of the command above:
New, upload `build/guitar-tuner.pbw`, then fill in the fields from the table.

To test the listing before it goes public, set its visibility to Unlisted
in the dashboard straight after publishing. It is then installable only by
its direct link.

## Later releases

Bump `version` in `package.json`, then:

```sh
pebble publish --release-notes "What changed" \
  --screenshots store/screenshots/emery_* store/screenshots/gabbro_*
```
