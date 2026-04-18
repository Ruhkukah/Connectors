# Redaction Policy

Always redact:

- passwords
- refresh tokens
- session secrets
- private keys
- authorization headers

Conditionally mask:

- account and login identifiers when diagnostics require partial visibility

All certification exports and logs must apply deterministic redaction so repeated runs remain diff-stable.
