# Phase 4D PLAZA II VPS CGate Install And TEST Evidence

## Scope

Phase 4D provides an operator-safe workflow for installing CGate/PLAZA II on a MOEX colocated VPS and running a bounded
PLAZA II TEST read-side evidence campaign. The approved stack remains `plaza2_repl + twime_trade`, but this phase uses
PLAZA II TEST only because TWIME access is not available yet.

This phase adds scripts for:

- local Docker Linux build and deploy bundle packaging
- official or operator-approved CGate distributive fetch
- user-owned CGate install staging on the VPS
- local TEST credential/profile setup guidance
- bounded PLAZA II TEST evidence collection
- operator-assisted TEST password change via the bundled vendor utility

## Non-Goals

Phase 4D does not add production connectivity, public market data, order entry, `plaza2_trade`, write-side C ABI,
strategy logic, persistence, daemonization, systemd units, firewall changes, or compilation on the VPS.

## Local Docker Build

Do not assume a Docker image name. Inspect existing local images first:

```bash
docker image ls
docker ps -a
docker image ls --format '{{.Repository}}:{{.Tag}} {{.ID}} {{.CreatedSince}} {{.Size}}'
```

Test likely images without pulling a new one:

```bash
docker run --rm "$MOEX_BUILD_IMAGE" bash -lc \
  'uname -a && cat /etc/os-release && cmake --version || true && g++ --version || true && clang++ --version || true'
```

Once selected, keep the image in an environment variable:

```bash
export MOEX_BUILD_IMAGE='<selected-local-image>'
```

Build locally in Docker:

```bash
docker run --rm \
  -v "$PWD:/work" \
  -w /work \
  "$MOEX_BUILD_IMAGE" \
  bash -lc '
    set -euo pipefail
    cmake -S . -B build-docker-linux -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build build-docker-linux -j"$(nproc)"
    ctest --test-dir build-docker-linux --output-on-failure -R \
      "plaza2_runtime_probe_test|plaza2_scheme_drift_test|plaza2_live_session_validation_test|plaza2_live_session_runner_test|moex_phase4b_capi_read_side_test|phase0_selfcheck|source_style_check|repo_style_check|unicode_guard_check"
  '
```

If the selected image lacks tools, choose another existing image or create a documented local build image after operator
approval. Do not pull or mutate images implicitly.

## Deploy Bundle

Package only the required binaries, templates, scripts, and this document:

```bash
scripts/vps/package_plaza2_test_bundle.sh \
  --build-dir build-docker-linux \
  --out-dir /tmp/moex-connector-deploy
```

The bundle intentionally excludes credentials, endpoints, CGate archives, build cache, and evidence. Upload it with
operator-provided SSH variables:

```bash
scp -i "$SSH_KEY" /tmp/moex-connector-deploy/moex-plaza2-test-bundle.tgz \
  "$VPS_USER@$VPS_HOST:~/moex/connector/"
```

## VPS Baseline And Directories

Run baseline checks without changing system settings:

```bash
uname -a
cat /etc/os-release
whoami
pwd
nproc
free -h
df -h
timedatectl || true
chronyc tracking || true
```

Create operator-owned directories:

```bash
mkdir -p ~/moex/connector ~/moex/runtime ~/moex/dist ~/moex/evidence
mkdir -p ~/.config/moex-connector/secrets ~/.config/moex-connector/profiles
chmod 700 ~/.config/moex-connector
chmod 700 ~/.config/moex-connector/secrets
chmod 700 ~/.config/moex-connector/profiles
```

## Fetch CGate Distributive

Inspect official MOEX areas and manually select the Linux AMD64 archive matching the TEST environment:

```bash
curl -L https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/ | head
curl -L https://ftp.moex.ru/pub/ClientsAPI/Spectra/CGate/ | head
```

Fetch the selected archive:

```bash
export CGATE_DISTRIBUTIVE_URL='<official MOEX CGate Linux URL>'

scripts/vps/fetch_cgate_distributive.sh \
  --url "$CGATE_DISTRIBUTIVE_URL" \
  --out-dir ~/moex/dist
```

The fetch script refuses non-MOEX URLs unless `--allow-non-moex-url` is passed explicitly. It writes a `.sha256` file.

## Install CGate

Stage and inspect the archive first:

```bash
scripts/vps/install_cgate_linux.sh \
  --archive ~/moex/dist/<cgate-linux-archive> \
  --installer ~/moex/dist/install.sh \
  --install-dir ~/moex/runtime/cgate \
  --evidence-dir ~/moex/evidence/cgate-install
```

Run the vendor installer only after reviewing the plan:

```bash
scripts/vps/install_cgate_linux.sh \
  --archive ~/moex/dist/<cgate-linux-archive> \
  --installer ~/moex/dist/install.sh \
  --install-dir ~/moex/runtime/cgate \
  --evidence-dir ~/moex/evidence/cgate-install \
  --execute
```

The `--installer` argument is needed when MOEX distributes `install.sh` beside the archive rather than inside it.

Post-install discovery:

```bash
find ~/moex/runtime/cgate -maxdepth 8 \
  \( -name 'libcgate.so' -o -name 'forts_scheme.ini' -o -name 'change_password*' \
     -o -name 'auth_client.ini' -o -name 'client_router.ini' -o -name 'router.ini' \) \
  2>/dev/null | sort
```

## Credential And Profile Setup

Create the local secret file on the VPS. Do not paste credentials into chat, docs, or committed files:

```bash
nano ~/.config/moex-connector/secrets/plaza2_test.env
chmod 600 ~/.config/moex-connector/secrets/plaza2_test.env
```

Expected shape:

```bash
export MOEX_PLAZA2_TEST_CREDENTIALS='<operator-local-value>'
```

Verify presence without printing the value:

```bash
set +o history
source ~/.config/moex-connector/secrets/plaza2_test.env
set -o history
test -n "${MOEX_PLAZA2_TEST_CREDENTIALS:-}" && echo "PLAZA2 TEST credential variable is set"
```

Create the untracked local profile:

```bash
cp ~/moex/connector/profiles/test_plaza2_repl_live_session.template.yaml \
   ~/.config/moex-connector/profiles/plaza2_repl_test.local.yaml
chmod 600 ~/.config/moex-connector/profiles/plaza2_repl_test.local.yaml
nano ~/.config/moex-connector/profiles/plaza2_repl_test.local.yaml
```

Populate TEST-only runtime paths, `libcgate.so`, `forts_scheme.ini` directory, config directory, and private stream
settings. Keep the credential reference as `MOEX_PLAZA2_TEST_CREDENTIALS`; do not store the raw password in the profile.

## Changing The PLAZA II TEST Password

MOEX requires the one-time TEST password to be changed within 7 days. Phase 4D supports only the bundled vendor
`change_password` utility. It does not implement the password-change protocol object.

Password policy:

- at least 8 characters
- meet 3 of 4 categories: lowercase Latin, uppercase Latin, numerals, special or non-alphanumeric characters
- must not contain 3 or more repeating characters
- must not match previously used passwords

Run the utility interactively:

```bash
scripts/vps/plaza2_change_test_password.sh \
  --utility ~/moex/runtime/cgate/bin/change_password \
  --host 127.0.0.1 \
  --port 4001
```

After success, update `~/.config/moex-connector/secrets/plaza2_test.env`. If local `auth_client.ini` or router auth
configuration stores credentials, update those files according to MOEX or broker instructions and restart the local
router if required.

## Preflight

Run offline checks before a live connection attempt:

```bash
scripts/vps/plaza2_test_preflight.sh \
  --bundle-root ~/moex/connector \
  --profile ~/.config/moex-connector/profiles/plaza2_repl_test.local.yaml \
  --secret-env-file ~/.config/moex-connector/secrets/plaza2_test.env \
  --output-dir ~/moex/evidence/plaza2-test/preflight
```

The current PLAZA II wrapper does not expose `--validate-only`. Preflight therefore checks binary presence, profile
presence, and credential variable presence without opening a CGate session.

## Bounded Live TEST Evidence

The existing runner is bounded by `--max-polls`. Run a private-state-only smoke:

```bash
run_id="$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$HOME/moex/evidence/plaza2-test/$run_id"

scripts/vps/plaza2_repl_test_evidence.sh \
  --bundle-root ~/moex/connector \
  --profile ~/.config/moex-connector/profiles/plaza2_repl_test.local.yaml \
  --secret-env-file ~/.config/moex-connector/secrets/plaza2_test.env \
  --output-dir "$HOME/moex/evidence/plaza2-test/$run_id" \
  --armed-test-network \
  --armed-test-session \
  --armed-test-plaza2 \
  --max-polls 256
```

Evidence files are redacted and kept on the VPS:

- `startup.json`
- `runtime_probe.json`
- `scheme_drift.json`
- `readiness.json`
- `final_health.json`
- `operator.log`
- `run_manifest.json`

## Safety And CI Model

Tracked scripts contain no real endpoints, no credentials, no production paths, and no CGate vendor payloads. Normal CI
checks script syntax and fail-closed behavior only; it does not require Docker, VPS access, CGate runtime, MOEX network
access, or credentials.

## Known Limitations

- TWIME access is not available yet, so Phase 4D evidence is PLAZA II read-side only.
- The current PLAZA II wrapper does not have a pure `--validate-only` mode.
- Evidence scripts summarize runner outputs; deeper stream-level evidence depends on the existing runner health surface.
- CGate install behavior ultimately depends on the operator-selected vendor archive and its `install.sh`.

## Next Recommended Phase

After a successful redacted TEST evidence campaign and password rotation, the next step is to review evidence gaps before
expanding toward a later live integrated run. Do not add public market data or order entry until explicitly approved.
