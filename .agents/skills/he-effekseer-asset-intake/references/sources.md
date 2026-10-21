# Effekseer asset sources

## Priority order

1. Use the official Effekseer New Sample Effects area linked from <https://effekseer.github.io/en/contribute.html>. Accept only individual items whose page or bundled evidence explicitly identifies the effect as CC0.
2. Use official Effekseer releases and editor downloads from <https://effekseer.github.io/en/download.html>. Pin an explicit release/tag compatible with HEngine; do not use `master`.
3. Consider third-party candidates from itch.io, BOOTH, OpenGameArt, GitHub, or GitLab only after checking the exact asset and every nested dependency.
4. Treat RPG Maker community assets as high risk unless the terms explicitly permit use in other/custom engines.

The official Old Sample Effects area uses older terms and is not automatically CC0. Review its exact license as a custom license.

## Discovery record

For each candidate, record:

- asset name and public landing/source URL;
- author/publisher and whether the source is first-party;
- free/paid/gated status;
- exact license identifier and stable evidence URL or bundled license path;
- commercial use, modification, attribution, redistribution, platform, engine, region, organization-size, and distribution restrictions;
- Effekseer source format/version, GPU-particle indication, CPU fallback, and renderer requirements when known;
- the gameplay gap it addresses and why its visual language is suitable;
- download decision: `ACCEPT`, `MANUAL_REVIEW`, or `REJECT`.

Do not use a search result, filename, "free," "royalty free," or marketplace category as license evidence. Do not save account details, purchase history, tracking parameters, or unrelated personal data.

## Candidate limits and ranking

Stop once enough candidates satisfy the request. Unless the task sets a different scope, cap the shortlist at 10 and authorized downloads at 5 explicitly licensed official CC0 items; these are ceilings, not targets or download authorization. Never mirror or bulk-download an effect library.

Rank candidates by:

1. gameplay readability;
2. element/weapon differentiation;
3. hit feedback and complete VFX language;
4. performance and renderer fit;
5. visual richness.

When the request covers a complete ability, prefer a coherent cast/spawn, projectile, trail, hit and residue chain. A request for one effect does not require sourcing the whole chain. Reject visually impressive effects that obscure characters, enemies or danger telegraphs.

## Download and inspection commands

Run from the repository root, replacing placeholders and using a license actually supported by the saved evidence. Download authorization and license approval are prerequisites; the dry run writes no files.

```powershell
python .agents/skills/he-effekseer-asset-intake/scripts/fetch_effekseer_asset.py --url <direct-download-url> --source <public-source-url> --license CC0-1.0 --author <author> --asset-id <asset-id> --destination Project/SourceAssets/EffekseerInbox --dry-run
```

Review the URL, license, target and filename, then repeat without `--dry-run` when correct. For an existing archive, inspection alone does not require downloading it again:

```powershell
python .agents/skills/he-effekseer-asset-intake/scripts/inspect_effekseer_archive.py Project/SourceAssets/EffekseerInbox/<asset-id>/<archive> --output Project/SourceAssets/EffekseerInbox/<asset-id>/inspection.json --pretty
```
