# Authorization source and chronology

Source: direct user messages in the same execution task that produced this
packet, before the run starting 2026-09-06T12:20:31Z. This note records the
conversation source; it is not an independently signed authorization artifact.
Individual message timestamps were not exported into this evidence packet.

1. The original work order required a fresh plan followed by separate approval
   of its full SHA. The user approved the earlier plan beginning `2d456cd7`;
   that plan was retired before activation and was not reused.
2. After that stop, the user directly instructed:

   > Ugh. You are making it more difficult than it should be. Just do this in one round, without asking me for any
   > authorization - I already authorized you to do all the tests!!

   The assistant explicitly scoped its response to fresh passive CRU6 price
   selection, one quantity-one TEST lifecycle, the existing checks and
   cancel/recovery logic, and no second or compensating order.
3. The first delegated exercise stopped before posting and exposed the wire
   layout defect. The user then instructed:

   > Don't just record - fix the issue instead

   The assistant stated it would correct the layout, validate offline, and
   run a fresh TEST exercise under the delegation without another approval
   round. The successful corrected run followed.

There was **no separate exact-SHA human approval** of
`d530759ff6e8f8d5f987999739512bf3fe187a3156b7ac817f8dbe8db4a35819`.
The temporary harness selected and installed that fresh plan under the user's
explicit delegated TEST execution instruction, superseding the original
manual handoff. No transport market/account/safety gate was bypassed.

The original `observation.log` retains `EXACT_PLAN_AUTHORIZATION_RECEIVED`.
For this delegated run, the accurate reviewed description of that event is
`DELEGATED_PLAN_SELECTION_APPLIED`: the harness applied the generated SHA,
not a separate human approval/file. The raw log and canonical plan/receipt
remain unmodified; this note explains the legacy label rather than rewriting it.

This record does not authorize production or additional orders. No repeat
order is necessary to correct the audit trail.
