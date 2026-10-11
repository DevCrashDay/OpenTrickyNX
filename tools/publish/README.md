# tools/publish — release signatures

The launcher installs an update only when the release's `SHA256SUMS.txt`
carries a valid Ed25519 signature (`SHA256SUMS.txt.sig`, 64 raw bytes,
detached, RFC 8032), made with the project's key whose **public** half is
built into the launcher (`port/launcher/app/ot_release_key.h`).
While that file is all zeros, the launcher keeps the SHA-256 check only.

## Once: the key (never in git)

```sh
# outside any repository, on a trusted machine; a passphrase is recommended
openssl genpkey -algorithm ed25519 -aes-256-cbc -out ~/opentricky-release.pem
chmod 600 ~/opentricky-release.pem
python3 tools/publish/sign_release.py pubkey --key ~/opentricky-release.pem
```

Paste the 4 `0x..` lines it prints into `ot_release_key.h` and commit them
(public key only). Keep a copy of the `.pem` offline: if it is lost, a new key
and a rebuilt launcher are needed (launchers already installed will refuse it).

## For each release

```sh
python3 tools/publish/sign_release.py sign --key ~/opentricky-release.pem \
        <release dir>/SHA256SUMS.txt          # -> SHA256SUMS.txt.sig
```

Upload the three assets: the zip, `SHA256SUMS.txt` and `SHA256SUMS.txt.sig`.
Don't change `SHA256SUMS.txt` after signing it (not even a line ending).

`sign` refuses a key stored inside a git repository, and a key other than the
launcher's; `verify` checks a file against the built-in key (or `--pub`).

## Tests (the public RFC 8032 test key, never the project's key)

```sh
cc -std=c99 -O2 -Iport/launcher/app -Iport/third_party/monocypher \
   port/launcher/tools/ot_sign_test.c port/launcher/app/ot_sign.c \
   port/third_party/monocypher/monocypher.c \
   port/third_party/monocypher/optional/monocypher-ed25519.c -o ot_sign_test
./ot_sign_test
python3 tools/publish/test_sign_release.py ./ot_sign_test
```
