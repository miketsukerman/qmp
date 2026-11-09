# Vendored Eclipse uProtocol specification

| | |
|---|---|
| Upstream | https://github.com/eclipse-uprotocol/up-spec |
| Tag | `v1.5.8` |
| Commit | `cab833880c2f1d7965efd507c06446358e2c949f` (2024-05-28) |
| License | Apache-2.0 (`LICENSE`, `NOTICE.md`) |
| Vendored | `up-core-api/uprotocol/**`, `LICENSE`, `NOTICE.md` |

Recorded in ADR-005 (`docs/DECISIONS.md`). Re-vendor with:

```sh
git clone --filter=blob:none https://github.com/eclipse-uprotocol/up-spec /tmp/up-spec
git -C /tmp/up-spec checkout v1.5.8
rm -rf third_party/up-spec/up-core-api
mkdir -p third_party/up-spec/up-core-api
cp -r /tmp/up-spec/up-core-api/uprotocol third_party/up-spec/up-core-api/
cp /tmp/up-spec/LICENSE /tmp/up-spec/NOTICE.md third_party/up-spec/
```

Protobuf import root for the vendored files: `third_party/up-spec/up-core-api`
(so `import "uprotocol/uoptions.proto";` resolves).
