# Effekseer asset license policy

Make a decision from explicit, saved evidence. A top-level license does not cover a nested texture, model, material, font, or sound unless its terms say so.

## ACCEPT

Automatically accept only:

- `CC0-1.0` or an unambiguous CC0 dedication;
- `CC-BY-3.0`/`CC-BY-4.0` when the author and full required attribution text are recorded;
- a custom license whose complete text explicitly permits commercial game use, modification, and the intended redistribution of the incorporated asset.

For the fetch script, use `CUSTOM-COMMERCIAL-MODIFICATION-ALLOWED --license-file <complete-terms>` only after saving and manually reading the complete custom terms. The token is a recorded review decision, not a substitute for the supplied license file.

## MANUAL_REVIEW

Require human review when any of these apply:

- modification is prohibited or limited;
- the listing says only "Royalty Free" without complete terms;
- the license is CC-BY-SA;
- terms exist only on a mutable product page;
- terms restrict region, platform, organization size, or distribution method;
- free and paid editions grant different rights;
- a nested dependency has different or unclear terms;
- a custom license is incomplete or ambiguous.

Keep these candidates out of runtime assets. For gated/free-or-paid marketplace items, register the candidate but do not automate login, acquisition, or purchase.

## REJECT

Reject when any of these apply:

- no license or license evidence;
- NonCommercial, personal-use-only, or no-commercial-game terms;
- use restricted to RPG Maker, Unity, Unreal, or another named engine;
- source or author cannot be confirmed;
- the item is a repost without confirmable authorization;
- acquisition requires bypassing login, payment, captcha, rate limits, or access controls;
- downloaded contents conflict with the listing/description.

## Evidence and attribution

Save the original archive, public source URL, author, acquisition date, SHA-256, exact license identifier, and a legally retainable `LICENSE`/terms copy or stable evidence reference. For CC-BY, save the creator name, work title when supplied, source URL, license URL, and required modification notice. Never infer attribution text.

Record separately:

- commercial-use permission;
- attribution requirement and exact text;
- modification permission;
- redistribution limits;
- engine/platform limits;
- decision and reviewer/evidence path.

If any field remains unclear, use `MANUAL_REVIEW` or `REJECT`; never silently convert unknown to allowed.
