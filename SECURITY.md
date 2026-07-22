# Security policy

## Supported versions

Only the latest release line receives fixes.

## Reporting a vulnerability

Please report vulnerabilities privately through GitHub security advisories
(Security -> Report a vulnerability) instead of opening a public issue.

Decoder robustness reports are especially welcome. mzip is expected to reject any malformed
archive with a clean `FormatError`; a crash, hang, or unbounded allocation on crafted input
is a security bug.
